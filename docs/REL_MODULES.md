# REL modules: relocatable code loaded at run time

Some games keep code outside `main.dol`. Tales of Symphonia loads `FR_m_Top2Btl.rel` (the battle) and
`FR_m_Top2field.rel` (the field) from the disc root into the heap with the Dolphin SDK's `OSLink`, then calls into
them through function pointers (`lwz r12,0x34(r3); mtctr r12; bctrl`). A static recompiler of the DOL alone cannot
run them: the first call prints `[FuncTable] Unresolved call to 0x81253B64` and the game hangs.

gcrecomp recompiles a REL to **position-independent** C++ (no interpreter) and registers it with a run-time
module registry that finds the module wherever the game's heap put it.

## Sources

* The module format: the WiiBrew "REL" page (header, section table, import table, relocation entries, relocation
  type numbers, version 1-3 differences), and the field names of the Dolphin SDK's `OSModuleHeader` /
  `OSModuleInfo` / `OSSectionInfo` as published by the public SDK-derived references. No code of the SDK or of any
  decompilation project is copied: the parser and the emitter are written from the layout description.
* The run-time behaviour (where the list head lives, what `OSLink` rewrites) was **read out of the game's own
  recompiled `Link`, `Relocate`, `OSUnlink`, `__OSModuleInit`** (Tales of Symphonia PAL, `0x800FBAA4..0x800FC4C4`),
  not assumed. The real `FR_m_Top2Btl.rel` / `FR_m_Top2field.rel` headers and relocation streams were dumped to
  validate the parser.

## The format (as used here)

All fields big-endian.

| Offset | Field | Note |
|---|---|---|
| 0x00 | `id` | module id (module 0 is the main DOL) |
| 0x04, 0x08 | `next`, `prev` | zero in the file; `OSLink` fills them |
| 0x0C | `numSections` | |
| 0x10 | `sectionInfoOffset` | file offset of the section table; **after `OSLink` an absolute guest pointer** |
| 0x14, 0x18 | `nameOffset`, `nameSize` | |
| 0x1C | `version` | 1, 2 or 3 |
| 0x20 | `bssSize` | |
| 0x24 | `relOffset` | file offset of the relocation entries |
| 0x28, 0x2C | `impOffset`, `impSize` | import table, 8 bytes per entry |
| 0x30 | `prologSection`, `epilogSection`, `unresolvedSection`, (1 more byte) | |
| 0x34, 0x38, 0x3C | `prolog`, `epilog`, `unresolved` | offsets inside those sections |
| 0x40, 0x44 | `align`, `bssAlign` | v2+ |
| 0x48 | `fixSize` | v3 |

Section table: `numSections` x (`offset | exec flag in bit 0`, `length`). `offset == 0 && length != 0` is BSS (the SDK
hands `OSLink` a buffer for it). Import table: (`module id`, `file offset of its relocation list`). A relocation
entry is 8 bytes: `u16 delta`, `u8 type`, `u8 section`, `u32 addend`. The cursor `(section, offset)` is advanced by
`delta` before every entry (`offset += delta`), `R_DOLPHIN_SECTION` (202) sets `section` and zeroes `offset`,
`R_DOLPHIN_NOP` (201) only advances, `R_DOLPHIN_END` (203) ends the list. Types: `R_PPC_ADDR32` 1, `ADDR24` 2,
`ADDR16` 3, `ADDR16_LO` 4, `ADDR16_HI` 5, `ADDR16_HA` 6, `ADDR14` 7, `ADDR14_BRTAKEN` 8, `ADDR14_BRNTAKEN` 9,
`REL24` 10, `REL14` 11. The value is `S + A` (`REL*`: minus the address `P` of the patched place): for module 0
`S = 0` and **the addend is the absolute address in the DOL** (the section byte is informational, checked on the
real files: `lis r3,0 ... addi r3,r3,0` pairs carry addends like `0x8036686C`); for any other module `S` is the
*run-time* base of section `section` of that module. For the 16-bit types the patched place is the immediate
halfword (instruction + 2); for the 24/14-bit types the instruction word.

Real files (`tos-recomp/extracted/disc1/files`): `FR_m_Top2Btl.rel` has id 1, 16 sections, v3, `.text` = section 1 =
0x93944 bytes (~600 KB), `.ctors`/`.dtors`, `.rodata` (4), `.data` (5), `.bss` (6); `FR_m_Top2field.rel` id 2,
17 sections, `.text` = section 2. They carry only `ADDR32` (vtables and jump tables), `ADDR16_LO/HA` (always in
matching pairs) and, against module 0, `REL24`; branches inside the module are pc-relative and carry **no**
relocation.

## What `OSLink` has already done by the time the game calls into the module

`OSLink` (recompiled, runs as-is) copies nothing; the game read the file into the heap. `Link` rewrites the
`OSModuleInfo` in place (`sectionInfoOffset` and every non-BSS section `offset` become absolute guest addresses,
BSS sections get addresses from the buffer passed to `OSLink`, `prolog`/`epilog`/`unresolved` become absolute) and
`Relocate` patches every relocated place in guest memory. Guest memory therefore holds the final relocated words
at a heap address that differs from run to run. The list head is **`__OSModuleInfoHead` at guest `0x800030C8`**,
tail at `0x800030CC`, string table at `0x800030D0` (read from `__OSModuleInit` and `OSUnlink`: `lis r4,0x8000` and
offsets 12488/12492/12496; *not* `0x800000C8`). Node layout: `OSModuleInfo` as in the table above (`next` +4,
`prev` +8, `numSections` +0xC, `sectionInfo` +0x10). Section lengths, exec flags, the version and the section count
are never changed by linking: they identify the module.

## Recompiler: `gcrecomp_recompiler --rel`

```
gcrecomp_recompiler [main.dol] --rel FR_m_Top2Btl.rel --rel FR_m_Top2field.rel \
    [--rel-name NAME] [--report-rel FILE] --output DIR --project tos
```

`--rel` may be given several times, with or without a DOL; `--rel-name` names the preceding `--rel` (default: the
file stem, sanitised). `--stats` analyses without writing; `--info` prints the module headers. **Give the DOL and the
modules in the same run**: the DOL recompilation then also takes every DOL address the modules call or point at as a
function entry (see "DOL entries" below).

### 1. Image

The sections are laid out contiguously at a fixed *virtual* base (`0x90000000`, 32-byte aligned; a module is limited
to 8 MB; `0x90...` never collides with the DOL's `0x80...`) and every relocation is applied to that image with section
bases = the virtual layout. The image looks like an ordinary linked program to the existing CFG builder, pointer
discovery (`docs/POINTER_DISCOVERY.md`) and jump-table detection, which are reused unchanged: `bl` between sections,
vtables (`ADDR32` data words that hold code addresses), `lis+addi` pairs, `bctr` tables, prologue scan and the header
entries (`prolog`, `epilog`, `unresolved`) all work. References into the DOL fall outside every code range (not
called, not followed). A branch that leaves the module is patched to a stub 16 MB away (a conditional one to its
fall-through) and remembered, so the analysis never sees a target that is not the module's.

### 2. Which addresses are function entries

After the passes above, relocations add certainty (they are the compiler's own list of address uses):

1. **Sweep.** Runs of code that no function contains (padding excepted) are entries: dead code, or code reached
   through a pointer the relocations do not describe. Repeated until nothing is left, so every instruction of every
   code section belongs to a function. (On the real modules this finds 96 and 32 more entries.)
2. **Relocation targets.** Every code address held by an `ADDR32` data word or named by an `ADDR16*` pair is an entry,
   *except* the cases of a resolved jump table (they are labels inside a function). A target that lies inside another
   function's body (a function that the CFG merged into its caller through a tail `b`) but is not a table label is
   promoted to a function too. On both real modules every such target was already an entry.

### 3. Position independence

The emitter takes the virtual image address of an instruction as a *name*, never as a value that reaches a
generated expression, except through these rules:

* an instruction with an `ADDR16*` relocation (`lis/addis`, `addi`, `ori`, loads/stores displacements) gets its
  immediate computed from the module base at run time: `T = base[sec] + addend` for the module itself,
  `T = addend` for module 0 (an absolute constant), `T = rel_import_base(id, sec) + addend` for another
  module; `LO` is `(int16_t)T`, `HI` `T >> 16`, `HA` `(T + 0x8000) >> 16`. Exact for any base. `addi`, `addis`,
  `ori`, `oris` are written out; any other instruction (every load and store form, update forms, `lfs`, `lfd`) is
  emitted by the normal path with a marker for its displacement and the expression put in its place;
* a call or tail call to module 0 goes through the function table with the absolute address, the function pointer
  cached per call site (the same result as a direct call from the DOL); a conditional branch out of the module becomes
  `if (cond) { call; return; }`;
* `bl` between module functions are direct C++ calls (position independent by construction);
* the value `bl`/`bctrl`/`blrl` leave in LR is `base[sec] + offset` of the return address;
* a `bctr` jump table is switched on `ctr - base[sec]` (case labels are section offsets); the table itself is guest
  memory that `Relocate` filled with run-time addresses;
* everything else (data/BSS accesses, `lwz` through registers) is plain guest memory.

After writing, the recompiler scans its own output for any constant in the analysed layout's range
(`0x90000000 + size`); one is an error (`baked_addresses`, must be 0), as is any relocated instruction it could not
make position independent (`unhandled_reloc`) or a relocation it does not support:

* an absolute branch (`ADDR24`, `ADDR14*`) into the module itself,
* a branch relocation not on an instruction, an `ADDR16*` not on an instruction immediate, a relocation outside its
  section, an unknown type.

### 4. Output

In namespace `rel_<name>`: `func_<virtual address>` functions (the name only; the code never uses the address), split
over `rel_<name>_NNNN.cpp` (`--funcs-per-file`, default 200), a header `rel_<name>.h`, and `rel_<name>_register.cpp`
whose static initialiser hands a `RelModuleDesc` (name, version, section sizes and exec mask = the signature, the base
array the code reads, `{section, offset, function}` entries) to the registry. `recomp_common.h` is written when
missing (a project's own is kept, as for the DOL). **No change to the project's `main` is needed.**

### DOL entries

A module calls DOL functions that nothing in the DOL may call: on Tales of Symphonia 19 of the 295 distinct DOL
functions the two modules call are not function entries of the DOL as the recompiler found it (e.g. `0x8006FFD8`,
`0x800D76A0`); a call to them would be `Unresolved call`. With the DOL in the same run, `rel_dol_entries()` collects
every DOL code address that a module relocation names (calls, branches, address pairs, data words) and
`add_extra_entries()` makes them entries (`[CFG] 390 code addresses of the DOL are used by REL modules`).

## Run time (`src/runtime/rel_modules.cpp`)

`FuncTable::call` already routes a miss to the "Unresolved call" path. Before reporting, it asks the module registry:

1. Walk the guest `OSModuleInfo` list from `0x800030C8` and find the node whose section range contains the address.
2. Identify the recompiled module: signature = `numSections`, version, per-section length and exec flag. Two
   recompiled modules with the same signature are indistinguishable (not the case for the real files; the registry
   keeps the first and says so on stderr).
3. **Activate**: read the run-time section bases out of the node, store them in the module's base array
   (the generated code reads them), build the `{base + offset -> function}` map, remember the node
   (`[REL] module 'X' (id N) linked at 0x...: M functions registered`).
4. Dispatch. A later address of an active module is served from the map after a cheap **validation**: the node
   must still be in the guest list with the same section bases. An `OSUnlink`ed module that was freed and
   overwritten, or relinked elsewhere, fails validation and is re-resolved; no stale entry is ever returned.
   `ICInvalidateRange` (the replacement now forwards it) over a module's sections drops the activation immediately.
5. An address inside a linked, recognised module that is not a recompiled entry prints module name, section and offset
   instead of a bare address.

## Tests

* `tests/rel_synth.h`: a tiny assembler and a REL writer; builds a synthetic module in the test (5 sections, `ADDR32`
  vtable/jump table/BSS pointer/DOL word, `ADDR16`, `LO`, `HI`, `HA` with the carry case, `ADDR24`, `ADDR14`,
  `ADDR14_BRTAKEN`, `ADDR14_BRNTAKEN`, `REL24` between sections and into module 0, `REL14` between sections, relocations
  against a second module, `NOP`/`SECTION`/`END` entries).
* `rel_recomp_test`: the parser reads what the writer wrote; the analysis finds the functions, counts every relocation
  type, classifies the table cases as labels, finds the DOL entries; unsupported relocations are refused.
* `rel_module_test`: `rel_synth_gen` writes the file, `gcrecomp_recompiler` recompiles it at build time, the generated
  code is linked into the test, which plays `OSLink` (its own implementation of the relocation semantics) at two
  different sets of addresses (different order, different alignment, opposite HA/LO carry), calls every function
  through the function table and checks the results, the return address a DOL function sees in LR, the vtable
  functions, the jump table, the host functions called in the DOL; then unlinks (memory overwritten), checks that old
  addresses are dead, relinks elsewhere, and once more at the first addresses.

## Validation on the real modules (Tales of Symphonia, PAL)

`gcrecomp_recompiler main.dol --csv ... --extra-funcs ... --rel FR_m_Top2Btl.rel --rel FR_m_Top2field.rel --report-rel r.txt`:

| | `FR_m_Top2Btl` | `FR_m_Top2field` |
|---|---|---|
| code | 604 484 bytes, all in functions | 182 176 bytes, all in functions |
| relocations | `ADDR32` 1653, `LO` 5428, `HA` 5428, `REL24` 2068 | `ADDR32` 210, `LO` 3378, `HA` 3378, `REL24` 1567 |
| against itself / the DOL / another module | 11 701 / 2 876 / 0 | 6 332 / 2 201 / 0 |
| functions | 1492 | 254 |
| ... named by a `bl` | 588 | 77 |
| ... by the header | 3 | 3 |
| ... by a data relocation (vtables) | 475 (467 by nothing else) | 24 (24) |
| ... by an `ADDR16` pair | 145 (144) | 5 (5) |
| ... swept up (no reference) | 96 | 32 |
| jump-table cases / bctr without a resolved table | 160 / 0 | 55 / 0 |
| relocation targets not already an entry | 0 | 0 |
| instructions rejected by the disassembler / not translated | 0 / 0 | 0 / 0 |
| relocated instructions not made position independent | 0 | 0 |
| `HA` without a `LO` partner / `LO` without an `HA` or `HI` | 0 / 0 | 0 / 0 |
| analysed-layout addresses in the generated code | 0 | 0 |
| generated files | 10 | 4 |

Independent check on the unrelocated files: no `lis rX,0` in the code lacks an `HA` relocation, every `HA` place is an
`addis`, every `LO` place is one of `addi, lwz, lwzu, stw, stb, stbu, lhz, lhzu, sth, lfs, lfsu, lfd, lfdu`.

The generated code of both modules compiles (`-O1 -w`, GCC) with the harness; see the pull request for the build.

## Wiring a project (tos-recomp)

`scripts/regen.sh` passes the disc's modules in the same run as the DOL:

```bash
rel_args=()
for rel in "$here"/extracted/disc1/files/*.rel; do rel_args+=(--rel "$rel"); done
"$gcrecomp/build/gcrecomp_recompiler" "$dol" --csv ... --extra-funcs ... "${rel_args[@]}" --output "$out" --project tos
```

(the `.so` files of the disc are not RELs). `generated/gcrecomp/*.cpp` is already globbed by the project's CMake, the
modules register themselves, nothing else changes. Build time grows by the generated code of the modules (about 28 MB of
C++, roughly 35 s at `-O1 -j4`).

## Limits

* One active instance per recompiled module (the generated code reads one base array). A second simultaneous link
  of the same module is reported and ignored.
* Absolute branches (`ADDR24`, `ADDR14*`) into the module itself are rejected. Against the DOL they mean "call `S+A`"
  (not the hardware sign-extension of an absolute branch).
* Relocations against a third module (`rel_import_base`) are resolved when the code runs; the third module must be
  linked by then. The real modules do not have any.
* A pointer into the module that no relocation describes and that is not a function start the sweep finds (computed
  at run time from a table that is not relocated) would be an `Unresolved call ... inside module 'X' section S +0xN`
  message naming the offset; there is no `--extra-funcs` equivalent for modules yet.
* Code is analysed under the assumption that it is not self-modifying; a module that patches its own text would run
  the recompiled original.
