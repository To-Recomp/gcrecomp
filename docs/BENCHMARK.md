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
