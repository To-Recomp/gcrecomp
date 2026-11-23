#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Benchmark a recompiled game: presented frames per second, VI throttling, CPU use and a profile, per scene.

Runs a harness (for example tos_boot) with scripted input (docs/INPUT_SCRIPT.md) and GCRECOMP_BENCH=1 (the reporter
of src/gp/bench.cpp prints one `[bench]` line per second on stderr), cuts the run into segments at the disc files the
game opens, and prints one row per segment. See docs/BENCHMARK.md.

  tools/benchmark.py --bin build/tos_boot --dol main.dol --files DISC_FILES \\
      --script tools/input-scripts/tos-new-game.txt --seconds 200 --out /var/tmp/bench1 \\
      [--perf field:25] [--label baseline] [--json result.json]

Segments: `--segment NAME=SUBSTRING` (repeatable, in the order the scene changes happen) starts segment NAME when the game
opens a disc file whose path contains SUBSTRING (case-insensitive; the `[script] frame N: game opened PATH` lines of the
log). `--segment NAME=cmd:TEXT` starts it when the script runs a command whose text contains TEXT instead (the
`[script] frame N: COMMAND` lines): a `shot battle-start` command in the script is a marker for something the game does not
announce with a file. The first segment,
`boot`, starts with the run. Defaults are those of Tales of Symphonia with tos-new-game.txt (title, newgame = the
save screens, movie = the 64 s movie before the opening, field = the opening cutscene in the classroom). `--perf NAME[:SECONDS]` records the host threads with `perf record` for SECONDS
(default 20) starting 4 s into that segment and prints the profile grouped by what the code is (guest code, memory
accessors, GP parser, GL renderer, GL driver, ...); needs `perf` and kernel.perf_event_paranoid <= 2.

The game is run at the lowest CPU and IO priority (`--no-nice` to disable) so a developer's machine stays usable.
"""
import argparse
import json
import os
import re
import resource
import shutil
import statistics
import subprocess
import sys
import time

DEFAULT_SEGMENTS = ["title=title.tpl", "newgame=map/isa_i06", "movie=mov/s01", "field=map/isa_t02"]

# Where the time goes: (bucket, regex on "dso symbol"). First match wins.
BUCKETS = [
    ("guest code (recompiled)", r"\bfunc_[0-9A-Fa-f]{8}\b"),
    ("memory accessors", r"Memory::(read|write|translate)|gcrecomp::Memory"),
    ("function dispatch / call table", r"FuncTable|last_dispatch|trace_call|trace_args|std::_Hashtable.*(uint)"),
    ("scheduler / interrupts / HLE", r"sched::|gcrecomp::.*(os_|hle_|poll|thread)|Interrupt"),
    ("GP parser / vertex decode", r"gp_parser|gp::(parse|decode|Parser)|gcrecomp::gp::(?!renderer)|decode_vertex|vertex"),
    ("GL renderer (ours)", r"renderer::|gl_renderer"),
    ("texture decode / cache", r"gcrecomp::gx::|gx_texture|decode_texture|texture|crc|hash"),
    ("EFB copy / readback", r"efb|encode_efb|copy_efb"),
    ("audio / DSP", r"audio|Audio|ax_|dsp|Dsp|DSP|adpcm|mixer"),
    ("GL driver / GPU", r"libGL|libnvidia|nvidia|mesa|dri\.so|libEGL|libGLX|radeon|iris|libLLVM|amdgpu|libvulkan|i965"),
    ("SDL", r"libSDL"),
    ("libc (memcpy, malloc, stdio)", r"libc\.so|libc-|libm\.so|libstdc\+\+|ld-linux|\[kernel|vdso"),
]


def bucket_of(dso, sym):
    key = f"{dso} {sym}"
    for name, rx in BUCKETS:
        if re.search(rx, key):
            return name
    return "other"


def parse_bench(line):
    if not line.startswith("[bench] "):
        return None
    d = {}
    for m in re.finditer(r"(\w+)=(\S+)", line):
        d[m.group(1)] = m.group(2)
    mult = {"K": 1e3, "M": 1e6, "G": 1e9}

    def num(v):
        v = v.rstrip("%/s")
        if v and v[-1] in mult:
            return float(v[:-1]) * mult[v[-1]]
        return float(v)

    out = {}
    for k, v in d.items():
        if k == "threads":
            out[k] = v
        else:
            try:
                out[k] = num(v)
            except ValueError:
                pass
    return out if "t" in out else None


SCRIPT_LINE = re.compile(r"\[script\] frame (\d+): (.*)$")


def marker_text(line):
    """(frame, kind, text) of a `[script]` log line: kind "file" for a disc file the game opened, "cmd" for a script command."""
    m = SCRIPT_LINE.match(line)
    if not m:
        return None
    body = m.group(2)
    if body.startswith("game opened "):
        return int(m.group(1)), "file", body[len("game opened "):].strip().lower()
    return int(m.group(1)), "cmd", body.strip().lower()


def seg_hit(seg, kind, text):
    """True when the log line (kind, text) starts segment `seg` = (name, substring); `cmd:` substrings match commands."""
    sub = seg[1]
    if sub.startswith("cmd:"):
        return kind == "cmd" and sub[4:].strip() in text
    return kind == "file" and sub in text


def run_perf(pid, seconds, out_dir, name):
    data = os.path.join(out_dir, f"perf-{name}.data")
    cmd = ["perf", "record", "-F", "249", "-p", str(pid), "-o", data, "--", "sleep", str(seconds)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip()
    rep = subprocess.run(["perf", "report", "-i", data, "--no-children", "--stdio", "--sort", "dso,symbol", "--percent-limit", "0.05"],
                         capture_output=True, text=True)
    rows = []
    for ln in rep.stdout.splitlines():
        m = re.match(r"\s*([\d.]+)%\s+(\S+)\s+\[(.)\]\s+(.*)$", ln)
        if m:
            rows.append((float(m.group(1)), m.group(2), m.group(4).strip()))
    os.remove(data)
    return rows, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--files", required=True)
    ap.add_argument("--script", action="append", required=True)
    ap.add_argument("--seconds", type=int, default=200, help="run length passed to the harness (default 200)")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "bench"))
    ap.add_argument("--segment", action="append",
                    help="NAME=SUBSTRING of a disc file path, or NAME=cmd:TEXT of a script command (replaces the Tales of Symphonia defaults)")
    ap.add_argument("--card-in", help="start from a copy of this memory card image (a save) instead of a blank card")
    ap.add_argument("--perf", action="append", default=[], help="NAME[:SECONDS] profile of a segment")
    ap.add_argument("--perf-top", type=int, default=25)
    ap.add_argument("--label", default="")
    ap.add_argument("--json", help="also write the result as JSON")
    ap.add_argument("--no-nice", action="store_true")
    ap.add_argument("--audio", action="store_true", help="keep the real audio sink (default: GCRECOMP_AUDIO=null)")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the game")
    ap.add_argument("--keep", action="store_true", help="keep the output directory (default: delete it, it holds a card image)")
    args = ap.parse_args()

    segs = [("boot", None)]
    for s in (args.segment or DEFAULT_SEGMENTS):
        name, _, sub = s.partition("=")
        segs.append((name, sub.lower()))
    perf_req = {}
    for p in args.perf:
        n, _, secs = p.partition(":")
        perf_req[n] = int(secs) if secs else 20

    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    with open(os.path.join(out, "script.txt"), "w") as f:
        for s in args.script:
            f.write(open(s).read() + "\n")
    env = dict(os.environ)
    env.update({
        "GCRECOMP_BENCH": "1",
        "GCRECOMP_CARD_IMAGE": out + ".card",
        "TOS_SCRIPT": "@" + os.path.join(out, "script.txt"),
        "TOS_SCREENSHOT_DIR": out,
        "GP_SHOT_EVERY": "1000000000",
    })
    if not args.audio:
        env["GCRECOMP_AUDIO"] = "null"
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    if os.path.exists(out + ".card"):
        os.remove(out + ".card")
    if args.card_in:
        shutil.copyfile(args.card_in, out + ".card")

    cmd = [os.path.abspath(args.bin), os.path.abspath(args.dol), os.path.abspath(args.files), str(args.seconds)]
    if not args.no_nice:
        cmd = ["nice", "-n", "19"] + (["ionice", "-c3"] if shutil.which("ionice") else []) + cmd
    log_path = os.path.join(out, "run.log")

    def unlimited_stack():
        resource.setrlimit(resource.RLIMIT_STACK, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))

    load0 = os.getloadavg()[0]
    logf = open(log_path, "w")
    t_start = time.time()
    proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT, env=env, preexec_fn=unlimited_stack)

    # Follow the log: markers start segments; a perf run starts 4 s into the requested segment.
    seg_index = 0
    seg_start_wall = {"boot": 0.0}
    markers = []
    perf_results = {}
    pos = 0
    pending_perf = None  # (name, wall time at which to start)
    carry = ""
    deadline = t_start + args.seconds + 60
    while proc.poll() is None and time.time() < deadline:
        time.sleep(0.5)
        with open(log_path, "r", errors="replace") as f:
            f.seek(pos)
            chunk = f.read()
            pos = f.tell()
        if chunk:
            lines = (carry + chunk).split("\n")
            carry = lines.pop()
            for ln in lines:
                mt = marker_text(ln)
                if not mt:
                    continue
                while seg_index + 1 < len(segs) and seg_hit(segs[seg_index + 1], mt[1], mt[2]):
                    seg_index += 1
                    name = segs[seg_index][0]
                    seg_start_wall[name] = time.time() - t_start
                    markers.append((name, time.time() - t_start, mt[0]))
                    if name in perf_req:
                        pending_perf = (name, time.time() + 4)
        if pending_perf and time.time() >= pending_perf[1]:
            name = pending_perf[0]
            pending_perf = None
            rows, err = run_perf(proc.pid, perf_req[name], out, name)
            perf_results[name] = (rows, err)
    if proc.poll() is None:
        proc.kill()
    proc.wait()
    logf.close()
    wall = time.time() - t_start
    load1 = os.getloadavg()[0]

    # Parse the log: [bench] lines are tagged with the segment current when they were printed.
    samples = []  # (segment, dict)
    seg_now = "boot"
    gp_line = ""
    unresolved = 0
    seg_names = [s[0] for s in segs]
    with open(log_path, errors="replace") as f:
        for ln in f:
            mt = marker_text(ln)
            if mt:
                i = seg_names.index(seg_now)
                while i + 1 < len(segs) and seg_hit(segs[i + 1], mt[1], mt[2]):
                    i += 1
                seg_now = seg_names[i]
                continue
            b = parse_bench(ln)
            if b:
                samples.append((seg_now, b))
            elif ln.startswith("[gp] "):
                gp_line = ln.strip()
            elif "Unresolved call" in ln:
                unresolved += 1

    result = {"label": args.label, "wall": wall, "segments": {}, "markers": markers, "gp": gp_line, "unresolved": unresolved}
    print(f"== benchmark {args.label or os.path.basename(args.bin)}: {len(samples)} s of samples, {wall:.0f} s wall, bin {args.bin}")
    print(f"host load average (1 min): {load0:.1f} at start, {load1:.1f} at end ({os.cpu_count()} CPUs): a busy machine starves the "
          "nice'd game and shows as seconds with low cpu% and low idle%")
    print("time to marker (wall s since start): " + (", ".join(f"{n} {t:.0f}" for n, t, _ in markers) or "none"))
    hdr = (f"{'segment':9} {'secs':>5} {'fps':>6} {'min':>6} {'med':>6} {'retrace':>7} {'idle%':>5} {'cpu%':>5} {'disp/s':>8} {'memop/s':>8} "
           f"{'draws/s':>8} {'texdec/s':>8} {'texel/s':>8} {'hash B/s':>8} {'efbmem/s':>8}  threads (CPU%)")
    print(hdr)
    for name in seg_names:
        ss = [b for s, b in samples if s == name]
        if len(ss) < 3:
            continue
        ss = ss[1:-1] if len(ss) > 4 else ss  # drop the transition seconds
        fps = [b["fps"] for b in ss]
        dt = len(ss)
        # fps over the span from cumulative counters
        span = (ss[-1]["presented"] - ss[0]["presented"] + 0.0) / max(1e-9, ss[-1]["t"] - ss[0]["t"]) if len(ss) > 1 else fps[0]
        def avg(k):
            return statistics.mean(b.get(k, 0) for b in ss)
        # per-thread CPU of the middle sample
        thr = ss[len(ss) // 2].get("threads", "")
        row = dict(secs=dt, fps=statistics.mean(fps), fps_min=min(fps), fps_med=statistics.median(fps), retrace=avg("retrace"),
                   idle=avg("idle"), cpu=avg("cpu"), dispatch=avg("dispatch"), memops=avg("memops"), draws=avg("draws"),
                   texdec=avg("texdec"), texdec_indexed=avg("texdec_indexed"), textexels=avg("textexels"), texhash=avg("texhash"),
                   texhash_bytes=avg("texhash_bytes"), efbmem=avg("efbmem"), efbmem_px=avg("efbmem_px"), threads=thr)
        result["segments"][name] = row
        print(f"{name:9} {dt:5d} {row['fps']:6.1f} {row['fps_min']:6.1f} {row['fps_med']:6.1f} {row['retrace']:7.1f} {row['idle']:5.0f} {row['cpu']:5.0f} "
              f"{row['dispatch']/1e6:7.2f}M {row['memops']/1e6:7.1f}M {row['draws']:8.0f} {row['texdec']:8.0f} {row['textexels']/1e6:7.2f}M "
              f"{row['texhash_bytes']/1e6:7.1f}M {row['efbmem']:8.0f}  {thr}")
    print("fps = frames presented to the window (EFB copies to the XFB) per second of wall time; retrace = VI retraces per second\n"
          "(59.94 when the clock is kept); idle% = share of time every guest thread was blocked waiting for the retrace or an\n"
          "alarm (high = throttled by the VI clock, the game has CPU to spare; ~0 = flat out, the host is the limit).")
    if gp_line:
        print(gp_line)
    if unresolved:
        print(f"WARNING: {unresolved} unresolved calls")

    for name, (rows, err) in perf_results.items():
        print(f"\n-- profile of segment {name} ({perf_req[name]} s, perf)")
        if not rows:
            print(f"perf failed: {err}")
            continue
        total = sum(r[0] for r in rows)
        buckets = {}
        for pct, dso, sym in rows:
            buckets[bucket_of(dso, sym)] = buckets.get(bucket_of(dso, sym), 0) + pct
        for b, pct in sorted(buckets.items(), key=lambda kv: -kv[1]):
            print(f"  {pct:6.2f}%  {b}")
        print(f"  top symbols ({total:.1f}% of samples listed):")
        for pct, dso, sym in rows[: args.perf_top]:
            print(f"  {pct:6.2f}%  {os.path.basename(dso):28.28} {sym[:90]}")
        result.setdefault("profile", {})[name] = {"buckets": buckets, "top": rows[: args.perf_top]}

    if args.json:
        with open(args.json, "w") as f:
            json.dump(result, f, indent=1)
    if not args.keep:
        shutil.rmtree(out, ignore_errors=True)
        if os.path.exists(out + ".card"):
            os.remove(out + ".card")
    return 0


if __name__ == "__main__":
    sys.exit(main())
