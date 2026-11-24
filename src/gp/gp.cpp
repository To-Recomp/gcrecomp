// SPDX-License-Identifier: MIT
//
// GP module entry points: window and renderer lifetime. The command stream itself is handled in
// gp_parser.cpp, which has no dependency on SDL or OpenGL.
#include "gcrecomp/gp/gp.h"

#include "gcrecomp/bench.h"

#include "gp_internal.h"
#include "gcrecomp/runtime.h"
#include "gp_record.h"
#include "input.h"

namespace gcrecomp::gp {

bool init(const Config& config) {
    reset();
    g_graphics = renderer::init(config.window_width, config.window_height, config.screenshot_dir, config.max_screenshots);
    g_wgp_write = &wgp_write;
    g_gp_sync = [] {
        if (g_graphics) renderer::flush_copies();
    };
    if (g_graphics) input::init(config.input_config);
    record::configure_from_env();
    bench::start();
    return g_graphics;
}

void shutdown() {
    g_wgp_write = nullptr;
    g_gp_sync = nullptr;
    if (g_graphics) {
        input::shutdown();
        renderer::shutdown();
    }
    g_graphics = false;
}

bool pump_events() { return g_graphics ? renderer::pump_events() : true; }

}  // namespace gcrecomp::gp
