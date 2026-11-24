// SPDX-License-Identifier: MIT
#include "gp_record.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gcrecomp/bench.h"
#include "gcrecomp/gp/gp.h"
#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"
#include "gp_internal.h"

namespace gcrecomp::gp::record {

namespace {

constexpr char kMagic[8] = {'G', 'P', 'R', 'E', 'C', '0', '0', '1'};
constexpr uint32_t kFrameMark = 0xFFFFFFFFu;

enum class Mode { Off, Armed, StartPending, Recording };

struct Recorder {
    Mode mode = Mode::Off;
    std::string path;
    int frames = 30, recorded = 0;
    uint64_t min_draws = 8500;
    double after_s = 0;
    bool slow_only = true;
    std::FILE* file = nullptr;
    std::vector<uint8_t> pending;  // records not written yet
    uint64_t last_draws = 0, last_retraces = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
} g_rec;

void put32(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t b[4];
    std::memcpy(b, &x, 4);
    v.insert(v.end(), b, b + 4);
}

void flush_pending() {
    if (g_rec.file && !g_rec.pending.empty()) std::fwrite(g_rec.pending.data(), 1, g_rec.pending.size(), g_rec.file);
    g_rec.pending.clear();
}

}  // namespace

void configure_from_env() {
    const char* env = std::getenv("GP_RECORD");
    if (!env || !*env) return;
    std::string spec = env;
    const size_t colon = spec.rfind(':');
    // FILE[:FRAMES]; a colon that is a drive letter ("C:\...") is not a separator.
    if (colon != std::string::npos && colon + 1 < spec.size() && spec.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
        g_rec.frames = std::max(1, std::atoi(spec.c_str() + colon + 1));
        spec.resize(colon);
    }
    g_rec.path = spec;
    if (const char* v = std::getenv("GP_RECORD_MIN_DRAWS")) g_rec.min_draws = uint64_t(std::strtoull(v, nullptr, 10));
    if (const char* v = std::getenv("GP_RECORD_AFTER")) g_rec.after_s = std::atof(v);
    if (const char* v = std::getenv("GP_RECORD_SLOW")) g_rec.slow_only = std::atoi(v) != 0;
    g_rec.t0 = std::chrono::steady_clock::now();
    g_rec.mode = Mode::Armed;
    std::fprintf(stderr, "[record] armed: %s, %d frames, from a frame of at least %llu draws%s after %g s\n", g_rec.path.c_str(), g_rec.frames,
                 (unsigned long long)g_rec.min_draws, g_rec.slow_only ? " that took two retraces or more" : "", g_rec.after_s);
}

void on_feed(const uint8_t* data, size_t n) {
    if (g_rec.mode != Mode::Recording || n == 0) return;
    put32(g_rec.pending, uint32_t(n));
    g_rec.pending.insert(g_rec.pending.end(), data, data + n);
    if (g_rec.pending.size() > (8u << 20)) flush_pending();
}

void on_frame_end() {
    if (g_rec.mode == Mode::Off) return;
    const uint64_t draws = g_stats.draws - g_rec.last_draws;
    g_rec.last_draws = g_stats.draws;
    const uint64_t retraces = sched::retrace_count();
    const uint64_t took = retraces - g_rec.last_retraces;
    g_rec.last_retraces = retraces;
    if (g_rec.mode == Mode::Recording) {
        put32(g_rec.pending, kFrameMark);
        flush_pending();
        if (++g_rec.recorded >= g_rec.frames) {
            std::fclose(g_rec.file);
            g_rec.file = nullptr;
            g_rec.mode = Mode::Off;
            std::fprintf(stderr, "[record] done: %d frames in %s\n", g_rec.recorded, g_rec.path.c_str());
        }
        return;
    }
    if (g_rec.mode == Mode::Armed && draws >= g_rec.min_draws && (!g_rec.slow_only || took >= 2) &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_rec.t0).count() >= g_rec.after_s)
        g_rec.mode = Mode::StartPending;  // the snapshot is taken once the parser sits between two commands
}

void on_feed_done(bool idle) {
    if (g_rec.mode != Mode::StartPending || !idle) return;
    g_rec.file = std::fopen(g_rec.path.c_str(), "wb");
    if (!g_rec.file) {
        std::fprintf(stderr, "[record] cannot write %s\n", g_rec.path.c_str());
        g_rec.mode = Mode::Off;
        return;
    }
    const uint32_t header[3] = {g_mem.ram_size, uint32_t(sizeof(State)), 0};
    std::fwrite(kMagic, 1, sizeof kMagic, g_rec.file);
    std::fwrite(header, sizeof(uint32_t), 3, g_rec.file);
    std::fwrite(g_mem.ram, 1, g_mem.ram_size, g_rec.file);
    std::fwrite(&g_state, 1, sizeof(State), g_rec.file);
    g_rec.recorded = 0;
    g_rec.mode = Mode::Recording;
    std::fprintf(stderr, "[record] recording %d frames to %s\n", g_rec.frames, g_rec.path.c_str());
}

bool replay(const char* path, int loops) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    char magic[8];
    uint32_t header[3];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, kMagic, 8) != 0 || std::fread(header, sizeof(uint32_t), 3, f) != 3 ||
        header[1] != sizeof(State)) {
        std::fprintf(stderr, "%s is not a recording of this build\n", path);
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> ram(header[0]), state(header[1]);
    if (std::fread(ram.data(), 1, ram.size(), f) != ram.size() || std::fread(state.data(), 1, state.size(), f) != state.size()) {
        std::fprintf(stderr, "%s is truncated\n", path);
        std::fclose(f);
        return false;
    }
    // The stream: chunks and frame marks, read whole.
    std::vector<uint8_t> stream;
    {
        uint8_t buf[1 << 16];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) stream.insert(stream.end(), buf, buf + n);
    }
    std::fclose(f);
    if (!g_mem.ram && !g_mem.init(header[0])) {
        std::fprintf(stderr, "cannot allocate %u bytes of guest RAM\n", header[0]);
        return false;
    }
    if (g_mem.ram_size != header[0]) {
        std::fprintf(stderr, "guest RAM is %u bytes, the recording's is %u\n", g_mem.ram_size, header[0]);
        return false;
    }
    bench::g_timing = true;
    for (int loop = 0; loop < loops; ++loop) {
        reset();
        std::memcpy(g_mem.ram, ram.data(), ram.size());
        g_mem.mark_written(Memory::MAIN_RAM_BASE, g_mem.ram_size);  // every cached texture revalidates
        std::memcpy(&g_state, state.data(), state.size());
        g_state.gen += 1000003u * uint32_t(loop + 1);  // the renderer's applied state is another one now
        const RenderStats before = render_stats();
        const auto t0 = std::chrono::steady_clock::now();
        int frames = 0;
        size_t pos = 0;
        while (pos + 4 <= stream.size()) {
            uint32_t n;
            std::memcpy(&n, &stream[pos], 4);
            pos += 4;
            if (n == kFrameMark) {
                ++frames;
                continue;
            }
            if (pos + n > stream.size()) break;
            feed(&stream[pos], n);
            pos += n;
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const RenderStats after = render_stats();
        const double fr = frames ? frames : 1;
        std::printf("loop %d: %d frames, %.2f ms/frame, %.0f draws/frame, parse %.2f ms/frame (renderer %.2f, read-back wait %.2f), %.0f vertices/frame, %.0f draws/frame with the state of the previous one, %.0f GL draw calls/frame\n", loop + 1,
                    frames, wall * 1e3 / fr, double(stats().draws) / fr, double(after.parse_ns - before.parse_ns) * 1e-6 / fr,
                    double(after.draw_ns - before.draw_ns) * 1e-6 / fr, double(after.readback_ns - before.readback_ns) * 1e-6 / fr,
                    double(stats().vertices) / fr, double(after.draws_same_state - before.draws_same_state) / fr, double(after.gl_draws - before.gl_draws) / fr);
        std::fflush(stdout);
    }
    return true;
}

}  // namespace gcrecomp::gp::record
