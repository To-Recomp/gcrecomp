// SPDX-License-Identifier: MIT
// Recording and replaying the GP command stream (docs/BENCHMARK.md, "Recording a scene").
//
// GP_RECORD=FILE[:FRAMES] makes the GP module write, at the first heavy frame it sees, a snapshot of guest RAM and of the GP
// register state, followed by every byte fed to the command parser for FRAMES presented frames (default 30). tools/gp_replay then
// replays the file headless, without the CPU, as often as asked: a scene that takes minutes of play to reach becomes a
// deterministic benchmark and profile target (the guest memory is the one at the start of the recording: vertex arrays and textures
// the CPU rewrote during it are not replayed, which changes values but not the cost). Recordings hold game data: keep them out of git.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gcrecomp::gp::record {

// Reads GP_RECORD, GP_RECORD_MIN_DRAWS (draws of the frame that triggers, default 8500), GP_RECORD_AFTER (seconds since the start of the
// run, default 0) and GP_RECORD_SLOW (default 1: only a frame that took two retraces or more triggers).
void configure_from_env();
// The parser was fed these bytes.
void on_feed(const uint8_t* data, size_t n);
// A frame was presented (the copy to the XFB just ran).
void on_frame_end();
// The parser finished a feed; `idle` is true when it holds no partial command.
void on_feed_done(bool idle);

// Replays a recording `loops` times into the renderer (gp::init must have run) and prints the time per frame of each loop.
// Returns false when the file cannot be read.
bool replay(const char* path, int loops);

}  // namespace gcrecomp::gp::record
