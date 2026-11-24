# Debugging a recompiled game

Switches and tools for finding out why a frame looks wrong. Nothing here needs a particular game.

## Environment variables (GP, the graphics processor)

| Variable | Effect |
| --- | --- |
| `GP_TRACE=1` | Prints the first draws, the large XF loads and the viewport/projection of a few draws. |
| `GP_TRACE_FRAME=N` | Prints one line per draw of presented frame `N` (primitive, vertex count, VCD, TEV stage 0, Z mode, blend, alpha compare, texture 0, projection, viewport), then for the first 60 draws the first vertex in clip space and normalised device coordinates, its lighting inputs and result, and for the first 3 draws four vertices with the position matrix rows they were transformed with. Frame numbers count EFB copies to the XFB (`[gp] N frames` at exit). A `[COPY]` line follows the draws with the EFB copies of that frame (destination, format, pixel format, clear). With the script command `trace-frame` (docs/INPUT_SCRIPT.md) the frame is chosen by an event instead of a number. |
| `GP_TRACE_DRAWS=first-last[,first-last...]` | With `GP_TRACE_FRAME`: only the draws numbered first..last (as printed) are rendered in that frame, to see what a group of draws contributes (compare screenshots of two ranges). With several ranges, the k-th traced frame renders the k-th range (the last one for the following frames): repeat the script commands `trace-frame` + `shot NAME` on a still scene to get one picture per group of draws from one run. Draws are numbered from 1 in each traced frame. |
| `GP_TRACE_VERTS=first-last` or `ortho` | With `GP_TRACE_FRAME`: prints every vertex (position, vertex colour 0, lit channel 0, first texture coordinates) of those draws, or of every draw with an orthographic projection (the 2D overlays, fades, windows). The last draw of a black frame is often a fade overlay: its vertex alpha says whether the game asked for black. |
| `GP_TRACE_BT=first-last` or `ortho` | With `GP_TRACE_FRAME`: prints `[bt] #N bt: ...` (the host call chain, as `GCRECOMP_WATCH` does) for those draws; `tools/guest-watch.py --bin BINARY LOG` names the guest functions: which game code issued the fade quad, a window, the HUD. |
| `GP_TRACE_STATE=n[,m...]` | With `GP_TRACE_FRAME`: dumps the full TEV, texgen, texture-map and raster state at those draws. |
| `GP_CULL=none` | Disables face culling (a quick test for geometry that vanishes). |
| `GP_DUMP_TEX=dir` | Writes the textures the game uses as PPM files (`tex_NN_fmtF_WxH_ADDR.ppm`, alpha shown over grey). Each (address, format, size) is written once. |
| `GP_DUMP_TEX_MAX=n` | How many textures `GP_DUMP_TEX` writes (default 24). |
| `GP_DUMP_TEX_AFTER=n` | Skip textures used before frame `n` (a movie, an earlier scene). |
| `GP_SHOT_EVERY=n` | Screenshot (PPM, window size) every `n` presented frames, into the screenshot directory of the host program. |
| `GP_XHIST=1` | Histogram of XF register writes at exit. |
| `GP_TEX_VALIDATE=always` | Re-reads every texture's guest bytes on every use, as the renderer did before the write stamps (docs/BENCHMARK.md): a texture that looks stale with the default and right with this has a writer into guest RAM that does not call `Memory::mark_written`. Without it a texture is re-read when a store touched its pages, and in any case every 120 presented frames. |
| `GP_TRACE_TEX=1` | Logs every texture decode (`[TEX] frame N decode map=M addr=ADDR fmt=F WxH levels=L REASON data=H palette=P`): the reason is `new`, `data changed` (the game rewrote the texture or an EFB copy landed in it) or `palette changed` (a palette format whose TLUT was loaded with other contents), with the hashes. A texture decoded every frame shows here; `GCRECOMP_BENCH` counts them (`texdec=`). |
| `GP_SKIP_DRAW=1` | The draws are parsed and decoded but not given to GL: isolates the cost of the renderer and the GPU (a game that adapts its frame rate to the host runs faster; see docs/BENCHMARK.md, the first temple boss). The picture stays empty. |
| `GP_APPLY_ALWAYS=1` | The renderer translates the BP/XF register state to GL for every draw instead of only after a register changed (`State::gen`): for comparing pictures with and without the skip. |
| `GP_COPY_FILTER=on` | Applies the vertical copy filter (BP 0x53 / 0x54, docs/EFB_COPY.md) to copies and presented frames: the deflicker taps a game sets then blur them as on a console. Off by default (implemented, not yet compared with hardware or Dolphin). |
| `GCRECOMP_DIAG_DIR=dir` | Where diagnostic bundles (below) are written; without it the screenshot directory of the host program (`TOS_SCREENSHOT_DIR`), without that the current directory. |

## The diagnostic bundle (F10)

Press **F10** in the game window (or run the script command `diag NAME`, docs/INPUT_SCRIPT.md) when the picture shows
what you want to look at: the renderer records the **next presented frame** completely and writes, as soon as that frame is
presented, a directory `diag-<n>-<UTC timestamp>[-NAME]/` under `GCRECOMP_DIAG_DIR` / `TOS_SCREENSHOT_DIR`:

| File | Content |
| --- | --- |
| `diag.txt` | Every draw of the frame in order (`[DRAW] #N` line as `GP_TRACE_FRAME` prints it: primitive, vertex count, VCD, TEV stage 0, Z mode, blend, alpha compare, texture 0, projection, viewport; the first vertices in clip space; every vertex of draws of 8 vertices or fewer as `[VERT]`: position, device coordinates, colour, texture coordinates, which is where the quads of screen-space passes and overlays are) and, after it, the full state the draw ran with (`[STATE]`, as `GP_TRACE_STATE` prints it, extended: Z / blend / alpha-test decoded, viewport, scissor and projection, every TEV stage decoded, texgens, every texture map used with size, format, address, wrap, min / mag filter, LOD bias, fog, fog range adjust, indirect stages); a draw whose state equals the previous draw's only says so. Every EFB copy in its place as `[COPY]` lines: source rectangle, destination address and format, stride, scale (BP 0x4E), clear and the clear colours, the pixel format, clamp, gamma, half scale / mipmap generation, and the vertical copy filter registers (BP 0x53 / 0x54: the seven taps and the weights they give the line above / this line / the line below; the filter has no enable bit, the SDK writes the taps for every copy and `0 0 21 22 21 0 0` is the unfiltered pattern). The XFB copy is last. |
| `summary.txt` | The frame number, counts per kind (draws, per primitive, vertices, EFB copies to textures and to the XFB), and the features the frame used with `implemented` / `NOT IMPLEMENTED`: indirect texturing, fog range adjust, Z texture, copy gamma, YUV and frame-to-field, and the ones that are implemented (the EFB copy vertical filter, TEV compare modes, constant destination alpha, blend logic ops, early Z with alpha test, projective texgens, LOD bias, mipmap filters, half scale, Y scale, depth copies). |
| `window.ppm` | The window as presented (what `shot` writes). |
| `efb-copyNN-destADDR-fmtF.ppm`, `efb-xfb.ppm` | The EFB over the source rectangle of each EFB-to-texture copy of the frame (at most 8) and of the XFB copy, taken just before the copy runs; `.alpha.pgm` next to each is the alpha channel. A blur or glow built from copies shows up as the stages: the scene in the EFB, then the copy, then what is drawn over it. |

The key is read once per frame: the frame in progress is already partly drawn, so the bundle starts at the next XFB copy and
covers the frame after it. The game is never stalled for more than that one frame (the EFB read-backs), and with nothing
requested the cost is one atomic load per draw and per copy. A second request while one is pending is ignored (`[diag]` lines
on stderr say what happened and where the files went). F10 cannot be bound to a pad control in the keyboard map. Bundles
contain pictures of the game: never commit them.

## Environment variables (guest memory and calls)

| Variable | Effect |
| --- | --- |
| `GCRECOMP_WATCH=0xADDR:LEN[,..]` | Every store overlapping a range prints `[watch] #N writeW addr = value bt: ...` with the host call chain; `tools/guest-watch.py --bin BINARY LOG [--summary]` names the guest functions. `GCRECOMP_WATCH_MAX=N` caps the lines (default 64). |
| `GCRECOMP_TRACE_CALLS=LR[,LR..]` | Prints the target and r3-r6 of function-table calls made with those link-register values (`GCRECOMP_TRACE_CALLS_MAX`, default 20). |
| `GCRECOMP_TRACE_ARGS=ADDR:SPEC[,SPEC..][;ADDR:..]` | When the function at ADDR is entered through the function table (callbacks, pointer calls), prints guest registers (`r3`) or memory words (`r13[-30980]`). `GCRECOMP_TRACE_ARGS_EVERY=N`, `GCRECOMP_TRACE_ARGS_MAX=N` (default 50). |
| `GCRECOMP_DEBUG_AFTER=S` | The three facilities above stay quiet for the first S seconds of wall-clock time. |

### REL modules

`[REL] module 'X' (id N) linked at 0xADDR: M functions registered` is printed the first time a call reaches a module
the guest linked. `[FuncTable] Unresolved call to 0xADDR ... inside module 'X' section S +0xOFF` is a call into a
linked module at an offset that is not a recompiled function entry (each function in the generated `rel_X_*.cpp`
is headed `// ---- section S +0xOFF ----`, so the offset can be looked up there).
`... (no recompiled module has this signature)` means the guest linked a REL that was not given to
`gcrecomp_recompiler --rel`. See docs/REL_MODULES.md.

`GCRECOMP_TRACE_REL=1` prints every activation (with the section bases and the guest heap state), every drop of an
activation (and why: node gone from the guest list, sections moved) and every `ICInvalidateRange` while a module is
active.

## Failing loudly (hangs, panics, heap exhaustion)

A console halts in `OSPanic` and a game that jumps to garbage crashes. A recompiled game that is allowed to carry on
does neither: it spins at hundreds of millions of memory operations a second with nothing on screen. The runtime now
stops instead, with a diagnosis, and exits with status 70:

* `OSPanic` prints `[GameCube PANIC] file:line: message`, the guest call chain (`[stack] lr=... <- ...`), the guest SDK
  heaps (below) and `[FATAL]`. `GCRECOMP_PANIC_CONTINUE=1` restores the old "print and return" behaviour.
* After `GCRECOMP_UNRESOLVED_LIMIT` (default 1000, 0 = never) calls to one address that neither the DOL table nor a
  linked module serves, `FuncTable::call` prints the address, its module (or that it is in none), the guest module list
  with the validity of every activation (`rel_dump_state`), the call chain and the heaps, then stops.

| Variable | Effect |
| --- | --- |
| `GCRECOMP_GUEST_HEAP=ARRAY:NUM:CURR` | Where the game's SDK keeps its heap state, as signed offsets from r13 (`HeapArray`, `NumHeaps`, `__OSCurrHeap`; Tales of Symphonia: `-30536:-30532:-32248`). Needed by every heap dump; find the offsets in the game's `OSInitAlloc` (the first two stores of r3 and r5). |
| `GCRECOMP_TRACE_HEAP=1` / `2` | A summary of every heap (size, free and allocated cells and bytes, largest block) at every `DVDOpen`; `2` adds the 16 biggest allocated blocks with their guest addresses. Compare the runs of two scenes: a heap that never recovers is a leak, one that is merely too small is a layout difference. |
| `GCRECOMP_TRACE_REL=1` | See "REL modules" above. |
| `GCRECOMP_UNRESOLVED_LIMIT=N` | See above. |
| `GCRECOMP_PANIC_CONTINUE=1` | `OSPanic` returns instead of stopping. |
| `GCRECOMP_ARENA_HI=0xADDR` | Overrides the arena's high bound (below). |

### Heap exhaustion: the arena's high bound

The symptom (Tales of Symphonia, the first boss of the Iselia temple): a nearly black frame, no more input, `[GameCube
PANIC] GCSYS.c:1958` (the game's `malloc` got NULL from `OSAllocFromHeap`) and `FQNEW:ERROR` in the log, then
`[FuncTable] Unresolved call` to the first function of the battle module and, once the log was 1 MB long, `Bad address`
lines. The chain was: the allocation failed, `OSPanic` returned, the game read a file into the NULL buffer (guest
address 0 is physical RAM: the OS globals, among them the REL module list at `0x800030C8`), the registry found no
module any more and the calls into it were unresolved. The module registry was innocent; the cause was memory.

The game creates its battle heap as `OSCreateHeap(0x81200000, OSGetArenaHi())`. The arena's high bound is not a free
parameter: on a console the apploader loads the disc's FST at the very top of RAM and `OSInit` starts the arena just
below it (boot.bin offset `0x42C` is the FST's maximum size: `0x9EC4` for this disc, so `0x817F6120`). A fixed
`GameConfig::arena_hi = 0x81700000` made that heap 5 242 880 bytes instead of 6 250 784: 1 MB short. The boss itself was
not replayed (the save is before the temple); at an ordinary battle the heap has 2.3 MB free before the background is
extracted and 1.1 MB after, so a battle with more party members and a larger background plausibly needs more than the
short heap holds. With the bound derived the same battle keeps 2.1 MB free. Give `GameConfig::fst_max_size`
(`disc_fst_max_size()` reads it
from boot.bin) and `init_low_memory()` derives the bound; `GCRECOMP_ARENA_HI` overrides it for experiments. How the
numbers were found: `GCRECOMP_TRACE_HEAP=2` at a normal battle showed the battle heap at 5 MB with 2.3 MB free before
the background is extracted and 1.1 MB after, and `GCRECOMP_WATCH` on the two biggest cells with `tools/guest-watch.py`
named the module's prolog and `func_9003F2F4` as the allocators of fixed 632 KB and 1 MB blocks.

### Writing tests

Tests must build on MSVC (a CI job builds them on Windows). Set environment variables with `set_env` / `unset_env` from
`tests/test_env.h`, not `setenv` / `unsetenv`, and keep POSIX-only calls (`unistd.h`, `mkdtemp`, `fork`, `getpid`,
`sleep`) out of `tests/`; use `std::filesystem::temp_directory_path()` for scratch files.

## Tools

* `tools/benchmark.py`: frames per second per scene, VI throttling, CPU use and a `perf` profile of a scripted run (docs/BENCHMARK.md).
* `tools/frame_compare.py OURS REFERENCE [--out sheet.png]`: compares one of our screenshots (PPM) with a
  reference frame (for example from Dolphin) and prints the error and a luminance grid of both, so a black
  render next to a bright reference is obvious. `--max-rmse` turns it into a check.
* `tools/prune_frames.sh --dir DIR --keep N [--while dolphin-emu]`: Dolphin's frame dump writes 60 PNGs per
  second; run this next to it to keep only the newest N frames.
* `tests/ps_semantics_*`: the floating-point and paired-single semantics of the code the recompiler emits
  (see below). Add an instruction by listing its word in `tests/ps_semantics_cases.h` and its expectation in
  `tests/ps_semantics_test.cpp`.
* `tools/ppc-diff/run.sh`: differential test of the recompiler's emitted code against a reference PowerPC CPU
  (Unicorn) on random integer / CR / FP / paired-single / load-store vectors, see `tools/ppc-diff/README.md`. Run it
  after touching `tools/recompiler/ppc_to_c.cpp` or `ppc_disasm.cpp`, and whenever a game does something subtly
  wrong (a silent audio channel, a slightly wrong value) with no visible crash: a recompiler instruction bug is
  the usual cause.
* `tools/guest-gdb.sh --bin BINARY --break func_8017AD3C --args "r3 r4" --mem r4:8 [--ignore N] -- PROGRAM-ARGS`: gdb
  breakpoints on guest functions (including the ones called directly with `bl`, which `GCRECOMP_TRACE_ARGS` cannot
  see): prints guest registers, memory words at an address or at `rN[+off]`, and the guest call chain, then continues.
  To compare a recompiled function with the real CPU, dump its inputs and outputs from gdb
  (`dump binary memory`) and replay them with `tools/ppc-diff/replay.py` (Unicorn, from the DOL).
* `tools/wav-channels.py A.wav [B.wav ...]`: per-channel RMS, L-R and the share of identical samples per time bin
  of a stereo WAV (`GCRECOMP_DUMP_AUDIO` or Dolphin's dumps), with a verdict mono / left only / right only / stereo.

## Reference frames from Dolphin

Dolphin writes the video with `[Movie] DumpFrames = True` in its `Dolphin.ini` (frames in
`<user>/Dump/Frames/framedump_N.png`). Press the buttons of the scene you want through a pipe controller
or `dolphin-run.sh`, run `prune_frames.sh` beside it, then compare with `frame_compare.py`.

## Wrong matrices, wrong geometry

When the geometry of a scene is garbage but the GP parser is right (compare the XF position matrix in the
`GP_TRACE_FRAME` output: rows with a shear or a non-uniform scale are suspicious), the matrices come from guest
code running on the recompiled CPU: suspect the floating-point instructions. The paired-single registers have
one representation: `f[i]` is ps0 and `ps1[i]` is ps1; every scalar FP instruction, `ps_*` instruction and
`psq_l` / `psq_st` goes through those two, and `tests/ps_semantics_test.cpp` runs the emitted code of a set of
instructions against hand-computed results (including a destination register that is also a source, and the
quantised load/store with saturation and signed scales).

## A screen that stays black (a fade that never ends)

The picture is rendered but a full-screen quad of the game's fade covers it. Find what the game waits for:

1. `GP_TRACE_FRAME=N` (or the script command `trace-frame`) with `GP_TRACE_VERTS=ortho GP_TRACE_BT=ortho` prints the
   overlay's vertices and the guest call chain that drew it (`tools/guest-watch.py` names the functions). The vertex
   alpha is read by that function from a global (the fade state: alpha, rate, a flag).
2. `GCRECOMP_WATCH=` on those globals (SDA-relative: `r13 + offset`, `r13` is the value `__start` loads) shows which guest
   functions start, step and end the fade, and the values they write. A fade that is never started again means the
   code that starts it is not reached: the game state machine is waiting for something else.
3. Asynchronous completions are the usual suspect. The SDK delivers callbacks (DVD reads, card operations) from an
   interrupt, after the call returned. A replacement that runs the callback inside the call breaks code that sets its
   own "busy" state after the call (Tales of Symphonia's file loader: slot state 2 "loading" written after the call
   returns, 3 "loaded" written by the callback); the loader then waits forever. `src/os/dvd_funcs.cpp` and
   `src/os/card_funcs.cpp` queue the callbacks and run them from the retrace interrupt and from status polls.

## Audio

Where the sound goes: the SDK's AI functions are replaced (`src/os/ai_funcs.cpp`); the game's PCM blocks reach an
`AudioSink` (SDL3 by default, `GCRECOMP_AUDIO=null` for none). Some scenes mix on the DSP (the AX microcode, lists
of commands in `src/audio/ax_hle.cpp`), others do not: Tales of Symphonia's intro movie (`mov/op.h4m`) stops the AX
lists and feeds the AI DMA itself, the title music is mixed by game code and handed to AX as aux buffers.

Environment switches:

| Variable | Effect |
| --- | --- |
| `GCRECOMP_DUMP_AUDIO=file.wav` | Everything played, as a 16-bit stereo WAV (valid after every block). |
| `GCRECOMP_AUDIO_TRACE=1` | Once a second: DMA blocks started, all-zero blocks, underruns, sink stalls, longest gap between polls, block size, rate, sink queue. |
| `GCRECOMP_AX_TRACE=1` | The first running AX voices (parameter block, format, addresses, ratio) and, once a second, the command histogram, voice slices and the RMS at stages of the mix (upload L/R/S, surround buffer in, output L/R/S): shows at which stage a channel dies. |
| `GCRECOMP_SOUND_MODE=mono` | The console sound setting `OSGetSoundMode` reports (default stereo, as a console and Dolphin; the SDK's own code would read the never-loaded SRAM as mono). Games choose a stereo or mono mix / movie audio track by it. |
| `DSP_TRACE=1` | DSP mails, control writes, every 500th AX list. |

A sink that holds a full lead and does not drain for 250 ms (device suspended or lost) is detected: the DMA then keeps
its real-time pace on the host clock and discards the audio, so the game does not freeze (`[AI] the audio device
stopped consuming`).

The AX model follows the version of the microcode the game uploaded, identified by Dolphin's HashEctor over its code
(`GCRECOMP_AX_TRACE=1` prints `[dsp] ucode (crc XXXXXXXX) starts`). `4E8A8B21`, the first GameCube AX (Tales of
Symphonia), reads a voice's mixer control word as "aux A / aux B / surround / ramp" with main L and R always mixed, has no
low-pass filter section in its parameter blocks, and later versions use one bit per bus. Reading a first-version game
with the later meaning drops the right channel of every voice whose word has no "R" bit (the title music was left
only). If a channel is silent at the `uploadR` stage of the `GCRECOMP_AX_TRACE` RMS line, look at the voices' mixer
control words first.

The console's sound setting (`OSGetSoundMode`) selects stereo or mono mixes and movie tracks in some games; it is
stereo unless `GCRECOMP_SOUND_MODE=mono`.

Tools:

* `tools/wav-compare.py A.wav [B.wav] [--align] [--a-from/--a-to/--b-from/--b-to S]`: non-zero ratio, RMS and peak per
  second, energy per band, and for two files envelope correlation, band-share distance and the time offset. Works on our
  dump and on Dolphin's `Dump/Audio/*dspdump.wav`; `--require-nonzero` makes it a check.
* `tools/dolphin-run.sh --iso game.iso --user DIR --dump-audio -- wait:20 press:A ...`: Dolphin reference run with
  scripted input (ported from fepor-recomp).
* `tools/audio-capture.sh --out cap.wav -- program args...`: records what the program really sends to the host audio
  server (its streams are moved to a temporary null sink and the sink's monitor is recorded), to test the default SDL
  sink end to end without listening. `build/gcrecomp_audio_probe --seconds 4` plays a tone through the same sink;
  `tools/audio-capture.sh --out c.wav -- build/gcrecomp_audio_probe` then `tools/wav-compare.py c.wav --require-nonzero`.
