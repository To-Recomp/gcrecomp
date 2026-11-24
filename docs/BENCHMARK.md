# Benchmarking and profiling a recompiled game

How fast does the game run, is it throttled by the retrace clock or flat out, and where does the host spend its time.
Nothing here is specific to a game; the segments and scripts shipped are those of Tales of Symphonia.

## The reporter: `GCRECOMP_BENCH`

`GCRECOMP_BENCH=1` (or the interval in seconds) makes `gp::init` start a thread that prints one line per interval on
stderr (`src/gp/bench.cpp`, `include/gcrecomp/bench.h`). It costs nothing when the variable is unset.

```
[bench] t=95.0 fps=12.40 retrace=59.91 presented=1180 retraces=5693 dispatch=0.01M/s memops=21.2M/s draws=128000 verts=...
        cpu=111% idle=12% threads=tos_boot:89%,tos_boot:gdrv0:6%,tos_boot:gl0:20%
```

| Field | Meaning |
| --- | --- |
| `fps` | frames presented per second of wall time: EFB copies to the XFB, the pictures the game finished. A game that renders at 30 shows 30. |
| `retrace` | VI retraces per second the clock took. 59.94 while the clock is kept (the model is wall clock, see `gcrecomp/vi.h`): it does not drop when the game is slow, the game loses retraces instead. |
| `dispatch`, `memops` | function-table dispatches (indirect calls, `FuncTable::call`) and guest memory accesses (`Memory::translate`, the unit of the 256K-op interrupt poll) per second: a rough measure of guest work. |
| `draws`, `verts` | draw calls and vertices submitted in the interval. |
| `texdec`, `texdec_indexed`, `textexels` | textures decoded and uploaded to the GPU in the interval, of which palette formats (C4, C8, C14X2), and the texels decoded. A texture is decoded again when its guest bytes change (the game rewrote it, an EFB copy landed in it) or, for a palette format, when its palette (TLUT) holds other contents. `GP_TRACE_TEX=1` says which texture and why (docs/DEBUGGING.md). |
| `texhash`, `texhash_bytes` | uses of a texture that read its guest bytes again to see whether they changed (the page write stamps skip this while nothing stored to the texture's pages), and the bytes read. |
| `efbmem`, `efbmem_px` | EFB-to-memory copies (read back from the GPU and encoded) and the pixels they read. |
| `parse_ms`, `draw_ms`, `readback_ms`, `swap_ms` | wall milliseconds per second spent in `Parser::draw` (vertex decode, transform, lighting, **including** the renderer call), in `renderer::draw` alone (state, vertex upload, GL calls), in the `glReadPixels` of EFB-to-memory copies (the wait for the GPU) and in the blit and buffer swap of a presented frame. Measured with two clock reads per call only while `GCRECOMP_BENCH` is set (about 5% overhead at 500 000 draws a second). `parse_ms - draw_ms` is the decode. |
| `efbgpu`, `efbgpu_hits`, `efbflush`, `efbguard` | EFB copies kept on the GPU, texture uses served by such a copy (no decode), copies whose bytes were written to guest memory, and CPU reads that caught a copy not yet written (see "EFB copies on the GPU"). |
| `gldraws`, `batches`, `batched`, `direct`, `replayed`, `earlyprog` | GL draw calls a second (a batch is one, two with the depth replay), batches drawn, draws collected into batches, draws made at once (lines, points, `GP_DRAW_MERGE=0`), draws drawn a second time depth-only (no early fragment tests), draws that got the early-Z depth write from the early-fragment-test program. At 60 fps in the boss fight: `batched` 535 000 for `batches` 5 300, `replayed` 0. A build without #58 prints none of these. |
| `flush_state`, `flush_tex`, `flush_prog`, `flush_size`, `flush_overlap`, `flush_direct`, `flush_other` | Why batches ended: the register state changed, a texture binding/contents/sampler changed, the program changed, the batch was full, pixels overlapped (replay fallback only), a direct draw came, other (an EFB copy). Many batches for few draws: look at the dominant reason. |
| `ez_late`, `ez_notest`, `ez_nowrite`, `ez_noreject` | Draws that need no early-Z depth write, by the first reason: Z compare after texturing (`ez_late`), no depth test, no depth write, an alpha test that rejects nothing. The others of `draws` need it (`earlyprog` or `replayed`). |
| `apply_ms`, `add_ms`, `flush_ms` | Wall milliseconds a second in `apply_state` (register translation and texture validation), in collecting draws into batches, and in drawing batches (upload, `glDrawElements`); parts of `draw_ms`. |
| `cpu` | CPU time of the whole process over wall time (100% = one core). `threads` lists the threads above 1%: the guest/main thread, the GL driver threads (`gdrv`, `gl`), audio. |
| `idle` | share of wall time every guest thread was blocked waiting for the retrace or an alarm (`sched::idle_ns`). High: the VI clock throttles the game, the host has CPU to spare. About 0 with `fps` below 60: flat out, the host is the limit. |

## `tools/benchmark.py`

```
tools/benchmark.py --bin build/tos_boot --dol extracted/disc1/sys/main.dol --files extracted/disc1/files \
    --script tools/input-scripts/tos-new-game.txt --seconds 215 --out /var/tmp/bench \
    --perf field:30 --label before --json before.json
```

It runs the harness with the script at the lowest CPU and IO priority (`nice -n 19`, `ionice -c3`), the null audio sink
and a fresh card image, cuts the `[bench]` lines into segments at the disc files the game opens
(`--segment NAME=SUBSTRING`; the defaults suit `tos-new-game.txt`: `title`, `newgame`, `movie` = the 64 s movie before
the opening, `field` = the opening cutscene in the classroom; `tos-intro-movie.txt` with
`--segment movie=mov/op.h4m` measures the intro movie) and prints a row per segment: mean / minimum / median fps over the
segment (the first and last second dropped), retraces, idle, CPU, dispatches, memory ops and draws per second, and the
time at which each marker was reached (wall seconds from the start: "time to classroom" is the `field` marker, the
opening of `map/isa_t02`; the dialogue of the classroom follows). The end prints the harness's `[gp]` statistics line: frames,
commands, draws, vertices and 0 unknown commands must match between two builds that differ only in speed.

`--perf NAME[:SECONDS]` runs `perf record -F 249 -p PID` on the process for SECONDS (default 20) from 4 s into the
segment and reports the profile grouped into what the code is (recompiled guest code, memory accessors, function
dispatch, scheduler, GP parser, GL renderer, texture decode/cache, EFB copy, audio, GL driver, SDL, libc), then the top
symbols. `perf` needs `kernel.perf_event_paranoid <= 2` for your own process; the generated code has no frame
pointers, so the profile is flat (self time), which is what is needed here. Without `perf`, `tools/guest-stacks.sh PID`
takes gdb backtraces (poor man's sampling: run it N times and count the innermost frames).

The output directory holds a memory card image: it is deleted unless `--keep`.

## A battle

`tools/input-scripts/tos-battle.txt` loads a save (the memorial circle in Iselia, right after the two opening fights), walks out
of the village onto the field map, walks in a square until the game loads a battle (a random encounter or a touched enemy symbol)
and fights with A until the victory. It needs the memory card image with that save (`--card-in`; the harness works on a copy) and
about 150 s. Its `shot` commands double as markers for the segments, and as screenshots to check that the run really got where it
should (`battle-hud` shows the HP bars of Lloyd, Colette and Genis):

```
tools/benchmark.py --bin build/tos_boot --dol extracted/disc1/sys/main.dol --files extracted/disc1/files \
    --script tools/input-scripts/tos-battle.txt --card-in CARD.raw --seconds 175 --out /var/tmp/bench-battle --keep \
    --segment load=map/isa_i06 --segment village='cmd:shot village' --segment field='cmd:shot field-start' \
    --segment battle='cmd:shot battle-start' --segment after='cmd:shot battle-end' --perf battle:15
```

`--segment NAME=cmd:TEXT` starts a segment when the script runs a command containing TEXT (the `[script] frame N: COMMAND` lines of
the log), `--segment NAME=SUBSTRING` when the game opens a disc file (as before). `--keep` leaves the screenshots (PPM) in the output
directory; look at them (`battle-start` is the transition effect, `battle-hud` the fight). The battle is not the same every time:
which enemies and how many depends on where the party walks and on the game's random numbers, so the draws per second of the
battle segment differ from run to run (200 000 to 500 000 seen); compare the rates per draw or per frame, and repeat a
measurement. The world map (field) is locked to 30 fps by the game.

## Comparing two builds

Run the same script on both, same machine, nothing else running, and compare `fps`, `idle` and the `[gp]` line (the
statistics are counts of what the game submitted: they differ only by timing noise, a few frames at the end). For
pictures use `shot NAME` commands in the script and `tools/frame_compare.py` (docs/DEBUGGING.md). Build flags of the
generated code are chosen by the game project (`tos-recomp/CMakeLists.txt`).

## Build flags of the recompiled code (measured)

Intro movie (guest-code bound: 60 fps either way, the number is the CPU needed for it), 16-core desktop, `-j4`, no ccache:

| generated code | compile | movie CPU |
| --- | --- | --- |
| `-O0` | 19 s | 53% |
| `-O1` (tos-recomp's setting) | 39 s | 30% |
| `-O2` | 60 s | 31% |
| `-O3` | 62 s | 30% |
| `-O1`, inline memory fast path (plain `inline`) | 49 s | 28% |
| `-O2`, inline memory fast path | 75 s | 24% |
| `-O1`, `-DGCRECOMP_FORCE_INLINE_MEMORY` | 120 s | 21% |

`-O1` is the right default: `-O2`/`-O3` buy nothing while every memory access is a call, and `-O0` costs 1.8 times the CPU.
`-DGCRECOMP_FORCE_INLINE_MEMORY` (a compile definition for the generated code) inlines the accessors at the ~270 000 access
sites of the game; it pays only for guest-heavy scenes (the classroom is bound by the GP, not by guest code), at 2.4 times the
compile time and twice the binary. `-fno-plt` / `-fno-semantic-interposition` do nothing here: the recompiled functions call each
other directly inside one executable.

## Results of the speed work (Tales of Symphonia, PAL French; 16-core desktop with an AMD GPU, nice 19)

`tools/benchmark.py ... tos-new-game.txt` (the opening is wall-clock driven: the game logic runs per retrace, so "fps" is the
rate at which the game finishes frames; 60 is full speed). The host was shared with a desktop session (load average 2 to 8),
so single seconds are noisy; means over the segment.

| change (PR) | title | new-game screens | 64 s movie | classroom (field) | classroom CPU |
| --- | --- | --- | --- | --- | --- |
| baseline (generated code -O1) | 34 | 58.7 (min 16) | 60 | **12.8** (min 8) | 111% |
| renderer: GL state/uniform/binding caches, texture validation by page write stamps (#41) | 59.9 | 59.3 | 59.9 | **30** (60 in quiet seconds) | 74% |
| vertex ring buffer, parser waits for the bytes it needs, faster EFB encode (#42) | 60 | 59.5 | 59.9 | **52.5** (min 31) | 74% |
| inline memory fast path (#43) | 60 | 59.7 | 59.9 | **59.7** (min 57) | 71% |
| lighting, no per-command getenv (#44) | 59.9 | 60 | 57.5 to 59.9 | 57.7 to 59.4 (min 44 to 48) | 74% |

Time to the opening classroom with player control (`tos-new-game.txt` + `tos-classroom.txt`, wall clock from the start of the
run): about 265 s (control at retrace frame 15 650). `GAMEPLAY_FINDINGS.md` recorded about 600 s for the cutscene alone after the
190 s of `tos-new-game.txt` (not re-measured here): the cutscene is paced by game frames, which now complete at 60 per second. The picture of the classroom with control looks as it
should and `[gp]` reports 0 unknown commands.

What was wrong: the classroom submits ~10 000 draws per frame. The renderer hashed the whole guest texture of every map on
every draw (55% of the time), looked uniforms up by name and re-sent unchanged state, allocated a GL buffer per draw, and the
parser decoded the vertex layout again after every 4-byte store of a vertex command. Recompiled guest code is 5% of the
classroom; the intro movie (guest-code bound) needs 21 to 28% of a core.

What remains (classroom profile, main thread at ~75% of a core): vertex decode, transform and lighting in `Parser::draw` and
`light_vertex` (~35%), `apply_state` and the GL driver's command thread (~10%), the EFB copy read-back and encode (~4%, plus
the GPU stall of `glReadPixels`), `Memory` accessors called from the recompiled code (~11% at -O1; in the movie 27% become 1.5% with
`GCRECOMP_FORCE_INLINE_MEMORY`), `memmove` of the parser's input buffer and of expanded quads (~4%). Not done: keeping EFB
copies on the GPU when the CPU never reads them back (needs the format quantisation emulated in a shader), a 144-byte vertex
with eight texture coordinate sets (a leaner vertex format would cut the upload), indexed quads instead of expanding them, the
recompiler polling at loop back-edges instead of counting every memory operation.

## Battles (Tales of Symphonia, the first fights after Iselia; `tools/input-scripts/tos-battle.txt`)

The owner's session showed 15 to 37 fps in battle seconds (200 000 to 340 000 draws a second) against 59.9 in the field of the village. The
battle benchmark (above) reproduces it: before this change the battle segment ran at 43 to 45 fps (minimum 29 to 30) with the main
thread at 65 to 68% of a core, while the seconds after the battle (results screens) already ran at 59.9.

What a battle submits and what was expensive (`GCRECOMP_BENCH` counters, `GP_TRACE_TEX=1`, `perf`):

* 4 000 to 6 000 draws a frame in the classroom style, but the profile of the battle seconds was not the draws: **46% of the main
  thread was palette texture decoding** (`tlut_entry_to_rgba` 31%, `gx::decode_texture` 9%, `decode_indexed` 6%). The counters said
  330 to 400 texture decodes and 85 to 100 million texels decoded a second, almost all C4 (`texdec_indexed`).
* The trace named two 512x512 C4 textures decoded 5 to 6 times a frame each, always `palette changed`: the battle samples the same
  texture with a different TLUT for each use (tinted effects), and a texture is decoded again whenever its palette holds other
  contents (the GL texture is keyed by the texture's address and size). Texture data that really changed (`data changed`) was a
  640x448 C4 image rewritten a few times a second (an EFB copy) and 24x24 C8 pieces.
* Page-stamp validation (#41) is not the problem here: `texhash` (guest bytes read again to validate) was 2 to 3 MB a second in
  battle against 30 MB in the village, and EFB-to-memory copies were 37 to 57 a second (about 10 million pixels read back a
  second), with 4% of the profile.

Fix: `decode_indexed` converts the palette once and walks the tiles once (a table read per texel instead of an integer-divide
conversion per texel after two intermediate passes; same bytes, unit test against the previous implementation on random data).
It does not avoid the repeated decodes (each use with another palette still decodes 262 144 texels), it makes each of them
about a dozen times cheaper.

Measured on the 16-core desktop (AMD GPU, nice 19, 140 s runs, the battle is random: two runs of each build, battle segment only,
mean / minimum fps over the seconds of the segment; `[gp]` reports 0 unknown commands on all of them):

| build | battle fps (run 1) | battle fps (run 2) | main thread | seconds after the battle (results screens) |
| --- | --- | --- | --- | --- |
| before | 43.2 (min 29) | 44.9 (min 30) | 67 to 68% | 59.9, 65% |
| palette table decode | **59.8 (min 58)** | **59.8 (min 58)** | 57 to 59% | 59.9, 44 to 46% |

(The draws a second differ between the builds because the faster one finishes more frames a second: 350 000 before, 500 000 after,
about 8 000 draws a frame in these battles.) Pictures: the screenshots of the village, the village exit and the field are
identical to those of the build before (`tools/frame_compare.py`, RMSE 0.00 to 0.25 for a field animation), the battle HUD
screenshots show the same scene.

Profile of a battle second after the change (main thread): `Parser::parse_one` with the inlined vertex decode 20%, `memmove`
10% (parser buffers, expanded quads), `light_vertex` and its helpers 13%, GL driver 8%, `apply_state` 4%, `decode_indexed` 4%,
`encode_efb_copy` 2%. The host has about 40% of a core to spare in a battle; the next costs, if a heavier battle needs them, are
the vertex decode and lighting, the quad expansion and the per-use palette decodes (an index texture with the palette applied on
the GPU would remove those, at the price of reproducing the console's filtering of palette textures in a shader).

## The first temple boss (Vidarr; `tools/input-scripts/tos-vidarr.txt`)

The owner's session showed two stretches at exactly 30.0 fps (about 10 s of arrival, three runs of 10 to 16 s in the fight). What they are:

* **"Arrival" (t=322 to 332 of his log) is the world map**: `FR_m_top2field.rel` is loaded and the game itself runs the map at 30 (the host is 54% idle
  there, 12 ms of main thread per frame). It is not a slowdown of ours.
* **The fight is.** `tos-vidarr.txt` loads the third save of his card (world map next to the Temple de Martel, play time 0:13), walks into the temple,
  through the Renegades cutscene and the soldier battle (`BTLenemy`, bgm_b000) to the boss battle (`BTLKratos.cab`, bgm_b006, ~100 s into the run;
  screenshots `e1`..`e3` cutscene, `b1`/`b2` soldiers, the boss is on screen in `frame_*` shots) and A-mashes to its end. The 30 fps stretches
  appear there at the spell effects ("Boule de feu", "Rayonnement") with the same numbers as in his log (267 000 draws and 2.33 M vertices a second
  at 30 fps, 8 900 draws a frame). The script is timing-fragile at the menus (the same input sometimes loads another save): check `load-confirm`
  ('3/4', 0:13) and `w1` ('Temple de Martel: Entrer') in the screenshots.

The game decides to run at 30 by itself when frames take too long: with `GP_SKIP_DRAW=1` (the draws are parsed and decoded but not given to GL) the
same fight stays at 59 fps from start to end, with 280 000 to 600 000 draws a second. So the host cost per frame decides, and the game's
drop lasts until the scene changes (the stretches end at effect boundaries; [INFERENCE] that the game keeps 30 while its frame time stays over the budget).

Per frame in the 30 fps stretches (16-core desktop, `tools/benchmark.py ... --segment boss=BTLKratos`; the GL device of a headless run is the
Ryzen's Radeon 610M, not the 7800 XT of the owner's window, `DRI_PRIME` does not select the other one under `SDL_VIDEO_DRIVER=offscreen`):

| | draws a frame | `Parser::draw` incl. renderer | of which `renderer::draw` | EFB read-back wait |
| --- | --- | --- | --- | --- |
| 30 fps stretches | 9 500 | 10.3 ms | 5.6 ms | 6 ms (iGPU: the GPU finishes the frame's draws here) |
| heavy 60 fps frames | 8 100 | 7.0 ms | 3.2 ms | 0.5 ms |

Profile of the boss fight (perf, main process): vertex decode with the parser 40%, GL driver and its thread ~15%, `memmove` 6 to 12%,
guest code 3 to 4%, memory accessors 8%. A frame costs ~10 ms of parse and GL calls before the guest and the read-back wait are counted, so the
16.7 ms budget is exceeded when the GPU is slow to finish the frame (the read-back is a full GPU sync once or twice a frame).

Changes of this branch (picture-identical: screenshots of the cutscene frames compare at RMSE 0.00, soldier battle frames at 0.15; `[gp]` 0 unknown;
ctest passes) and what they bought, boss segment of the script, mean fps / minimum, two runs are within 1 fps of each other:

| build | boss fps (mean / min) | 30 fps stretch: parse ms/frame | heavy 60 fps frames: parse ms/frame |
| --- | --- | --- | --- |
| before | 42.5 / 29 | 10.3 | 7.0 |
| quads written straight into the vertex buffer, no unlit-channel lighting or normal transform (the loop's `lit` array shadowed the flag, so the normal was always transformed), exact reciprocal-power-of-two scaling, layout cache, array bases resolved once per draw, texgen decode hoisted | 43.0 / 29 | 10.3 | 7.2 |

So these micro-optimisations are correct but **do not move the fps**: the boss frames light all their vertices and are dominated by what the
list above names. What the next change must do (measured leads, not done): (1) fewer GL calls per draw (a persistent mapped vertex ring instead of
map/unmap per draw, skipping `apply_state` when no BP/viewport register changed since the previous draw, merging consecutive triangle/quad draws whose
state is identical into one `glDrawArrays`: the vertices are transformed on the CPU, so a matrix load between two draws does not matter);
(2) a vertex with only the texture coordinate sets in use (the 144-byte vertex is memmove traffic); (3) the SIMD or table-driven lighting;
(4) moving the EFB read-back off the critical path where the CPU never reads the copy. A GP stream record/replay (headless, no CPU) would
allow profiling these offline; not built yet.

### Skipping the register translation of an unchanged state

`apply_state` rebuilt the 16 TEV stage words, blend, fog, viewport and scissor state for every draw. `State::gen` now counts the writes that can change that
state (a BP register whose value changes, a TLUT load, an XF load that touches the viewport words) and the renderer translates only when it moved since the last
draw (textures are still validated per draw; an EFB copy or the early-Z replay forces a new translation). `GP_APPLY_ALWAYS=1` restores the old behaviour for comparing.

`tos-vidarr.txt`, same machine (iGPU, see above), one run each: soldier battle main-thread CPU 67% -> 59 to 62% at 59.8 fps; boss fight 43.0 -> 44.3 fps (mean), per
frame in the 30 fps stretches `renderer::draw` 5.6 -> 6.0 ms (unchanged, noise) and in heavy 60 fps frames 3.2 -> 3.0 ms; so the 30 fps frames change state
at almost every draw and gain nothing, which also rules out merging identical-state draws as the cure for them. Pictures: `GP_APPLY_ALWAYS` against the default on the scripted
scene, RMSE 0.00 to 0.73 (only the idle sway of the characters differs between two runs, the difference image shows edges only).
What is left in the boss profile (perf): `Parser::draw` (vertex decode and transform) 20%, `light_vertex` + `light_factor` + `transform_normal` 9%, the Mesa glthread 12%, `memmove` 5%.

## EFB copies on the GPU

What the owner's RX 7800 XT showed in the boss fight: 12.6 ms of every 32 ms frame were `readback_ms`, the synchronous `glReadPixels` of the once-a-frame EFB copy
(a full pipeline drain: the GL driver thread and the GPU finish every draw queued before it). The copies of that scene are RGB565 and RGBA8, at full size
and half scaled (the zoom blur and the battle-start shards sample them).

A copy to memory in one of those formats (no Y scale, no vertical filter, destination rows packed as the texture decoder reads them) is now
1. drawn into a GL texture by a full-screen pass that applies the format's quantisation (5-6-5 bits expanded to 8 as `gx::decode_texture` does, or RGBA8; the 2x2 box filter of a half-scaled copy as `encode_efb_copy` does it, truncating `>> 2`);
2. read back into a pixel buffer object with a fence, without waiting;
3. written to guest memory (`encode_efb_copy`) later, when its fence has signalled at a sync point (draw-sync token, PE finish, `GXDrawDone`), or once it is two frames old.

Draws that sample the copy's address, size and format (`texture_for_map`) bind the texture instead of decoding guest memory. Exactness:
* a CPU **read** of the copy's bytes before they are written is caught by the read guard (`Memory::read_guard_hook`, one test of `guard_count` per read when nothing is guarded) and writes the copy first, waiting for the GPU if need be (`efbguard` counts these: 0 in the boss fight, the battle and the world map of ToS);
* a CPU **store** into the range after the copy wins on its pages (the write stamps say so; the GPU copy is dropped), as it did when the copy was written at once;
* a texture decode that overlaps the range, and a later copy that overlaps it, write the pending copy first; a thread other than the renderer's that reads a pending copy gets a warning (it cannot call GL);
* `GP_EFB_COPY_SYNC=1` reads every copy back at once as before. `GP_EFB_COPY_VERIFY=1` compares every GPU copy with what the texture decoder makes of the bytes written for it (0 of 6 000 differ in the boss fight, which uses all four kinds; it forces a sync read per copy, so it is slow); `GP_TRACE_COPY=1` lists the kinds of copy to memory.

Bytes read through a host pointer by the host itself (an HLE routine copying out of the range) are only caught at the first byte (`Memory::translate`).

Result on the iGPU test box (Radeon 610M, headless; `tos-vidarr.txt`, boss segment of 41 s, one run each; the owner's 7800 XT is the acceptance machine):

| build | boss fps (mean / min) | seconds under 35 fps | `readback_ms` a second (mean / max) | main thread CPU |
| --- | --- | --- | --- | --- |
| `GP_EFB_COPY_SYNC=1` (the old behaviour) | 42.4 / 29 | 20 of 41 | 144 / 317 | 59% |
| copies on the GPU, written at the sync point (waiting for the GPU there: no gain, the wait only moves) | 41.4 / 30 | 22 | 140 / 287 | 59% |
| copies on the GPU, written when ready or at two frames, read guard | **58.7 / 49** | **0** | **8 / 36** | 80% |

`tos-battle.txt` (the owner's other card): 9 039 of 9 476 copies stay on the GPU, `GP_EFB_COPY_VERIFY` finds 0 of 7 200 checked different, 0 guard hits, 0 unknown GP commands.
Pictures: the scripted cutscene frames of the two builds compare at RMSE 0.00 to 0.9 (idle sway of characters and the world map's camera), the battle-start shatter effect, which samples the copy, looks as expected.
With the stall gone the main thread is the limit (80% of a core at 59 fps): the next costs are the ones listed above (vertex decode, lighting, GL calls).

## Recording a scene and replaying it (`GP_RECORD`, `tools/gp_replay`)

A scene that takes minutes of play to reach (the boss fight needs 100 s of script) is a poor loop for optimising. `GP_RECORD=FILE[:FRAMES]` makes the GP module
write, at the first heavy frame it sees, a snapshot of guest RAM and of the GP register state, then every byte fed to the command parser for FRAMES presented
frames (default 30). The frame that triggers is one with at least `GP_RECORD_MIN_DRAWS` draws (default 8500) that took two retraces or more (`GP_RECORD_SLOW=0`
drops that condition), and only after `GP_RECORD_AFTER` seconds of the run:

```
GP_RECORD=/var/tmp/boss.gprec:40 GP_RECORD_AFTER=104 tools/benchmark.py ... --script tools/input-scripts/tos-vidarr.txt ...   # the boss fight is at 104 s
SDL_VIDEO_DRIVER=offscreen build/gp_replay /var/tmp/boss.gprec --loops 6
perf record -F 999 build/gp_replay /var/tmp/boss.gprec --loops 40        # a profile of the parser and renderer alone
TOS_SCREENSHOT_DIR=/var/tmp/s GP_SHOT_EVERY=4 build/gp_replay boss.gprec --loops 1   # PPM frames: cmp two builds, bit for bit
```

`gp_replay` replays without the CPU, flat out, and prints per loop the time per frame, the draws, the parse time (decode plus renderer) and the renderer's share,
the read-back wait, and how many draws had the state of the previous one and how many GL draw calls were made. A replay is deterministic: two loops agree within
a few percent, and the pictures of two runs are byte-identical when the renderer is. The guest memory is the one at the start of the recording, so vertex arrays and
textures the CPU rewrote during it are not replayed (values change, the cost does not). A recording is 25 MB of RAM plus the stream (15 MB for 40 boss frames) and holds
game data: keep it out of git.

## Draw batching and early fragment tests

In the replay of 40 boss frames (10 600 draws, 91 000 vertices a frame) 99% of the draws had the GL state of the previous one, yet each made
three GL calls for its vertices and, being alpha-tested with Z compare before texturing, a second draw with the colour mask and the alpha test changed (the depth
write of pixels the alpha test rejects). That second draw was 5.5 of the 7 ms the renderer spent per frame.

* Triangles, quads, strips and fans of one state (`State::gen` unchanged, same textures; anything that changes GL state calls `flush_batch()` first) are collected
  and drawn as one indexed triangle list: strips and fans become the triangles GL makes of them, a quad two triangles. `GP_DRAW_MERGE=0` draws every one at once.
* The fragment shader has a second program with `layout(early_fragment_tests) in;` (GL_ARB_shader_image_load_store): the depth test and the depth write happen before the
  shader, so a discarded fragment has written its depth, which is what the hardware does. It is used for the draws that needed the replay, so they need none
  (`GP_EARLY_TESTS=0` or a driver without the extension; the start of every run logs `[GL] draw batching on; early fragment tests active|disabled|not available`: the replay of the old code, the group of draws then limited to ones that share no pixel: boxes in clip space).

Replay of the boss recording on the iGPU box (parse = decode plus renderer, per frame): 13.1 ms, parse 11.8 (renderer 7.0) before; **8.9 ms, parse 5.4 (renderer 1.2)** after;
GL draw calls per frame 21 000 to 96. The 40 presented frames compare byte for byte with those of `GP_DRAW_MERGE=0 GP_EARLY_TESTS=0`. In the game (`tos-vidarr.txt`, boss segment, one run
each): fps 57.8 (min 45) to **59.8 (min 58)**, main thread CPU 77% to 56%, `parse_ms` 444 to 318 and `draw_ms` 234 to 104 a second.
