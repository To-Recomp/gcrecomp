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
#include <cstdint>

namespace gcrecomp::bench {

// Memory operations (every access that goes through Memory::translate, the unit the 256K-op interrupt poll counts).
extern uint64_t g_mem_ops;

// Starts the reporter if GCRECOMP_BENCH is set. Called by gp::init. Safe to call twice.
void start();

}  // namespace gcrecomp::bench
