// SPDX-License-Identifier: MIT
//
// GCRECOMP_BENCH reporter: see include/gcrecomp/bench.h and docs/BENCHMARK.md.
#include "gcrecomp/bench.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "gcrecomp/gp/gp.h"
#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

namespace gcrecomp::bench {

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
    double t = 0;
    uint64_t presented = 0, retraces = 0, dispatch = 0, memops = 0, draws = 0, verts = 0, idle_ns = 0;
    std::map<std::string, double> cpu;  // CPU seconds per thread name
};

double clock_ticks() {
#if defined(__linux__)
    static const double hz = double(sysconf(_SC_CLK_TCK));
    return hz;
#else
    return 100.0;
#endif
}

// CPU seconds used so far by every thread of the process, summed by thread name (Linux: /proc/self/task/*/stat).
// The guest runs on the thread that called the entry point (named after the executable) and on host threads the
// scheduler creates for guest threads; the SDL audio, GL driver and reporter threads have their own names.
std::map<std::string, double> thread_cpu() {
    std::map<std::string, double> out;
#if defined(__linux__)
    DIR* d = opendir("/proc/self/task");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char path[96];
        std::snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        std::FILE* f = std::fopen(path, "r");
        if (!f) continue;
        char line[1024];
        const bool ok = std::fgets(line, sizeof line, f) != nullptr;
        std::fclose(f);
        if (!ok) continue;
        // pid (comm) state ... : comm may hold blanks and parentheses, so split at the last ')'.
        char* open = std::strchr(line, '(');
        char* close = std::strrchr(line, ')');
        if (!open || !close) continue;
        std::string comm(open + 1, close);
        unsigned long long ut = 0, st = 0;
        // After ") ": state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15)
        if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        out[comm] += double(ut + st) / clock_ticks();
    }
    closedir(d);
#endif
    return out;
}

Sample take(Clock::time_point t0) {
    Sample s;
    s.t = std::chrono::duration<double>(Clock::now() - t0).count();
    s.presented = gp::frames_presented();
    s.retraces = sched::retrace_count();
    s.dispatch = last_dispatch(nullptr, nullptr);
    s.memops = g_mem_ops;
    s.draws = gp::stats().draws;
    s.verts = gp::stats().vertices;
    s.idle_ns = sched::idle_ns();
    s.cpu = thread_cpu();
    return s;
}

std::string human(double v) {
    char b[32];
    if (v >= 1e9) std::snprintf(b, sizeof b, "%.2fG", v / 1e9);
    else if (v >= 1e6) std::snprintf(b, sizeof b, "%.2fM", v / 1e6);
    else if (v >= 1e3) std::snprintf(b, sizeof b, "%.1fK", v / 1e3);
    else std::snprintf(b, sizeof b, "%.0f", v);
    return b;
}

void report(const Sample& a, const Sample& b) {
    const double dt = b.t - a.t;
    if (dt <= 0) return;
    double cpu_total = 0;
    std::string threads;
    for (const auto& [name, v] : b.cpu) {
        const auto it = a.cpu.find(name);
        const double d = v - (it == a.cpu.end() ? 0.0 : it->second);
        cpu_total += d;
        if (d / dt >= 0.01) {
            char t[64];
            std::snprintf(t, sizeof t, "%s%s:%.0f%%", threads.empty() ? "" : ",", name.c_str(), 100.0 * d / dt);
            threads += t;
        }
    }
    const double idle = double(b.idle_ns - a.idle_ns) * 1e-9;
    std::fprintf(stderr,
                 "[bench] t=%.1f fps=%.2f retrace=%.2f presented=%llu retraces=%llu dispatch=%s/s memops=%s/s draws=%llu verts=%llu "
                 "cpu=%.0f%% idle=%.0f%% threads=%s\n",
                 b.t, double(b.presented - a.presented) / dt, double(b.retraces - a.retraces) / dt, (unsigned long long)b.presented,
                 (unsigned long long)b.retraces, human(double(b.dispatch - a.dispatch) / dt).c_str(),
                 human(double(b.memops - a.memops) / dt).c_str(), (unsigned long long)(b.draws - a.draws),
                 (unsigned long long)(b.verts - a.verts), 100.0 * cpu_total / dt, 100.0 * idle / dt, threads.c_str());
}

}  // namespace

void start() {
    static bool started = false;
    const char* env = std::getenv("GCRECOMP_BENCH");
    if (started || !env || !*env) return;
    started = true;
    double interval = std::atof(env);
    if (interval <= 0) interval = 1;
    std::thread([interval] {
        const auto t0 = Clock::now();
        Sample prev = take(t0);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::duration<double>(interval));
            Sample cur = take(t0);
            report(prev, cur);
            prev = std::move(cur);
        }
    }).detach();
}

}  // namespace gcrecomp::bench
