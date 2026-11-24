// SPDX-License-Identifier: MIT
// gp_replay: replays a GP stream recording (GP_RECORD, src/gp/gp_record.h) into the renderer without any game code.
//
//   SDL_VIDEO_DRIVER=offscreen tools/gp_replay FILE [--loops N] [--window WxH]
//
// Prints the time per frame of each loop; run it under `perf record` for a profile of the parser and renderer alone. With
// TOS_SCREENSHOT_DIR and GP_SHOT_EVERY=K the presented frames are written as PPM files (identical output of two builds can then be
// checked with cmp). See docs/BENCHMARK.md.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gcrecomp/gp/gp.h"
#include "gcrecomp/runtime.h"

namespace gcrecomp::gp::record {
bool replay(const char* path, int loops);
}

int main(int argc, char** argv) {
    const char* file = nullptr;
    int loops = 3, w = 1280, h = 960;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--loops") && i + 1 < argc) loops = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--window") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else file = argv[i];
    }
    if (!file) {
        std::fprintf(stderr, "usage: gp_replay FILE [--loops N] [--window WxH]\n");
        return 2;
    }
    gcrecomp::gp::Config config;
    config.window_width = w;
    config.window_height = h;
    config.screenshot_dir = std::getenv("TOS_SCREENSHOT_DIR");
    if (!gcrecomp::gp::init(config)) {
        std::fprintf(stderr, "no window or OpenGL context (SDL_VIDEO_DRIVER=offscreen runs headless)\n");
        return 1;
    }
    const bool ok = gcrecomp::gp::record::replay(file, loops);
    gcrecomp::gp::shutdown();
    return ok ? 0 : 1;
}
