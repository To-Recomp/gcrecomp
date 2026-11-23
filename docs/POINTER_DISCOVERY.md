# Function entries reached only through pointers

A callback (a `bsearch` comparator, a DVD/AR completion handler, a vtable slot) is called with `bctrl`/`blrl`,
so no `bl` names it. If the recompiler does not know its address as a function start, the run time prints
`[FuncTable] Unresolved call to 0xADDR` and the game usually waits forever for the flag the callback would have set.
Until now those entries were found one at a time by playing and added to `--extra-funcs`.

`gcrecomp_recompiler` now finds them statically (`--discover-pointers`, on by default as `safe`).

## Where candidates come from

| Origin | What it is |
|---|---|
| `lis+addi`, `lis+ori` | `lis rA,hi` followed within 16 instructions (same straight-line run, `rA` not overwritten) by `addi rB,rA,lo` / `ori rB,rA,lo`. `rB` need not be `rA`, the halves need not be adjacent. The result must be an address inside a text section. |
| `data` | A 4-byte aligned word of a data section that holds a text address (vtables, callback tables, function-pointer fields). The report gives the length of the run of consecutive code pointers (`run=N`). |

Small-data loads (`lwz rX,off(r13)`) are data references, not code addresses, and are not examined.

## Filters

Run after the original passes (bl targets, symbol csv, `--extra-funcs`, prologue scan) have built their functions,
in ascending address order; each promoted entry's body is added to the covered ranges before the next candidate.

Rejected by every policy:

1. already a function entry (counted, tier `known`);
2. not 4-byte aligned (`unaligned`);
3. inside the body of a known function: any address covered by a basic block of an existing function that is not that function's entry (`mid-function`). This is what jump tables and loop labels look like;
4. not preceded by a function end: the instruction before it (skipping `nop`/zero padding) must be `blr`, `b` or `bctr`, else the code above falls through into it (`not preceded by a function end`);
5. the body does not decode: a walk over every path from the target meets an instruction the disassembler rejects (`undecodable instruction`).

Doubtful shapes, which `safe` rejects and `aggressive` keeps at a score penalty:

- the entry pops a frame it never pushed (`addi r1,r1,+N`, `mtlr` before any save), see `looks_like_function_entry`;
- the body falls through into a known function start, or branches/calls outside the code ranges.

One shape is accepted as ordinary: Metrowerks schedules `mflr r0` and address loads ahead of `stwu r1,-N(r1)`, so the
prologue scan plants a bogus entry at the `stwu`. A target that opens with `mflr r0` and falls into such a `stwu` within
8 instructions, with no branch between, is the same function.

## Ranking and policies

Each surviving candidate gets a score:

| Evidence | Score |
|---|---|
| follows `blr` (35 if padding lies between) | +40 |
| follows `b` or `bctr` | +15 |
| pair inside a known function / pair in unreached code / data word | +30 / +10 / +25 (+5 if in a run of 2 or more code pointers); the best origin counts |
| more than one origin | +10 (+5 more if both a pair and a data word) |
| body decodes cleanly to terminators | +10 |
| opens a stack frame | +5 |
| preceding function has a `bctr` with no resolved table (possible unresolved switch cases) | -25 |
| each doubtful shape | -30 |

* `--discover-pointers=off`: nothing is promoted.
* `--discover-pointers=safe` (default): promote candidates that follow a `blr`, have no doubtful shape, whose body walk is clean, and that score 70 or more.
* `--discover-pointers=aggressive`: promote everything that survives with a score of 35 or more (entries after `b`/`bctr`, doubtful shapes, pairs in unreached code).

## Report

```
gcrecomp_recompiler main.dol --csv syms.csv --stats --discover-pointers=safe --report-pointers pointers.tsv
```

Tab-separated, `#` header lines with totals, one row per target: `target tier score promoted reason origins`.
`tier` is `safe`, `aggressive` (passes aggressive only), `rejected` (reason names the filter that fired) or `known`.
Origins read `lis+addi@<lis>/<addi>`, `lis+ori@...` or `data@<word>(run=N)`; `(unreached)` marks a pair outside every known function.
`--report-pointers -` writes to stdout. The report is produced whatever the policy, so `off` is a pure dry run.

`tools/pointer-discovery/validate.py` scores a report:

```
validate.py pointers.tsv --truth GQSF.extra_funcs.txt --dol main.dol --spot 10   # recall on known callbacks, disassemble a sample of extras
validate.py pointers.tsv --dtk-symbols symbols.txt                                # precision against a decomp project's function list
```

`--spot` needs `capstone`. Run the report WITHOUT the truth entries in `--extra-funcs`.

## Validation

Tales of Symphonia (GQSF, PAL FR), the 19 entries of `GQSF.extra_funcs.txt` removed from `--extra-funcs`, `GQSF.csv` kept:

| | known set rediscovered | promoted | beyond the known set |
|---|---|---|---|
| existing passes only | 2 of 19 (`80104C0C` is named in the csv, `80151D78` is a `stwu` found by the prologue scan) | | |
| `safe` | 17 of the other 17 | 59 | 42 |
| `aggressive` | 17 of 17 | 64 | 47 |

No promoted entry lies inside an existing function body. The 42 extra `safe` entries were spot-checked by disassembly (8 sampled
evenly: `li r0,1; stw ...; blr` completion callbacks, lone `blr` stubs filling a callback table at `0x80121048`, `mflr` functions
called through a table); all are plausible function starts. The 5 aggressive-only entries are hand-written asm of the OS
(exception/context code at `0x800F7550`, `0x800F7574`, `0x800F93DC`, `0x800FBA4C`, `0x80120740`).

Precision against real function lists, no symbols given to the recompiler (decomp symbol maps of the reference games, `type:function` entries):

| game | policy | promoted | true function starts | inside a real function | other |
|---|---|---|---|---|---|
| Twilight Princess PAL | safe | 129 | 129 (100%) | 0 | 0 |
| | aggressive | 136 | 136 (100%) | 0 | 0 |
| Wind Waker PAL | safe | 129 | 129 (100%) | 0 | 0 |
| | aggressive | 136 | 136 (100%) | 0 | 0 |

Of the pointer-referenced true starts the existing passes had missed, `safe` takes 129 of 137 and `aggressive` 136 of 137 on each game
(the last one lies inside a function the CFG already built). Analysis time on these DOLs: about 1 s.

## Limits

* Function pointers that are neither stored in a data section nor built from a `lis`/`addi` pair (computed from a base register plus offset, read out of a REL or a file at run time) are not seen. Those still need `--extra-funcs`.
* The `blr`-only preceding requirement of `safe` loses callbacks placed after a tail-call `b`; `aggressive` takes them.
* Entries are promoted as ordinary functions: the earlier `stwu`-scan entries that sit a few instructions inside a real function stay in the table (harmless, never called).
* Unit tests: `tests/pointer_discovery_test.cpp` builds a synthetic DOL (lis/addi with a different register and a gap, lis/ori behind padding, a data table, a mid-function label, an unaligned address, an epilogue, undecodable code, a fall-through into another function, a fall-through from code above) and checks the tier and reason of each.
