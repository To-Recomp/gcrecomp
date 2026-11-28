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

#include <cstdlib>
#include <memory>
#include <string>

namespace gcrecomp::gp {

namespace {
std::unique_ptr<settings::Store> g_settings;
}

bool init(const Config& config) {
    reset();
    g_graphics = renderer::init(config.window_width, config.window_height, config.screenshot_dir, config.max_screenshots);
    g_wgp_write = &wgp_write;
    g_gp_sync = [] {
        if (g_graphics) renderer::flush_copies();
    };
    if (g_graphics) {
        std::string data_dir = config.data_dir ? config.data_dir : "";
        if (data_dir.empty())
            if (const char* env = std::getenv("RECOMPLAUNCHER_DATA")) data_dir = env;
        g_settings = std::make_unique<settings::Store>(data_dir);
        const std::string input_path = g_settings->input_path();
        input::init(config.input_config ? config.input_config : input_path.c_str());
        renderer::attach_settings(*g_settings, config.manifest);
    }
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
    g_settings.reset();
}

settings::Store* settings_store() { return g_settings.get(); }

bool pump_events() { return g_graphics ? renderer::pump_events() : true; }

}  // namespace gcrecomp::gp
