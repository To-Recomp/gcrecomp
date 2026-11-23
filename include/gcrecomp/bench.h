#pragma once
// SPDX-License-Identifier: MIT
// =============================================================================
// Run-time benchmark counters (docs/BENCHMARK.md).
//
// GCRECOMP_BENCH=S (seconds, default 1 when set to 1 or empty-ish) makes the GP module start a reporter thread that
// prints one line per interval on stderr:
//
//   [bench] t=12.0 fps=59.8 retrace=59.9 presented=718 retraces=719 dispatch=1.2M/s memops=96M/s draws=412 verts=21K
//           cpu=87% guest=84% idle=3% gl=2% threads=...
//
// Everything is cumulative-safe (the tool takes deltas) and costs nothing when the variable is unset.
// =============================================================================
#include <chrono>
#include <cstdint>

namespace gcrecomp::bench {

// Memory operations (every access that goes through Memory::translate, the unit the 256K-op interrupt poll counts).
extern uint64_t g_mem_ops;

// True once start() found GCRECOMP_BENCH: the renderer then times its calls (RenderStats::*_ns).
inline bool g_timing = false;

// Adds the wall time of its scope to `acc` while g_timing is set (two clock reads per scope, none otherwise).
class ScopedNs {
public:
    explicit ScopedNs(uint64_t& acc) : acc_(g_timing ? &acc : nullptr) {
        if (acc_) t0_ = now();
    }
    ~ScopedNs() {
        if (acc_) *acc_ += now() - t0_;
    }
    ScopedNs(const ScopedNs&) = delete;
    ScopedNs& operator=(const ScopedNs&) = delete;

private:
    static uint64_t now() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    uint64_t* acc_;
    uint64_t t0_ = 0;
};

// Starts the reporter if GCRECOMP_BENCH is set. Called by gp::init. Safe to call twice.
void start();

}  // namespace gcrecomp::bench
