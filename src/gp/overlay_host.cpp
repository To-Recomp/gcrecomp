// SPDX-License-Identifier: MIT
//
// The settings overlay's host for the GameCube runtime: lists the devices, captures a key or a button for a rebinding,
// suspends the pad while the menu is open, and re-reads the files the runtime applies live when the overlay writes
// one. The overlay itself (To-Recomp/overlay) is console-agnostic and knows nothing of the GameCube.
#include "overlay_glue.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>

#include "gcrecomp/audio/audio_sink.h"
#include "gcrecomp/gp/bindings.h"
#include "torecomp/overlay/overlay.h"

namespace gcrecomp::gp::overlay_glue {
namespace {

namespace ov = torecomp::overlay;

class GameCubeHost final : public ov::Host {
public:
    explicit GameCubeHost(settings::Store& store) : store_(store) {}

    std::vector<std::string> list(const std::string& name) override {
        if (name == "audio.devices") return audio::list_playback_devices();
        if (name == "gamepads") return input::connected_gamepads();
        return {};
    }

    // The overlay wrote a file: the runtime re-reads what it applies live (volume, window mode...) and its bindings.
    void changed(const ov::Setting&) override {
        store_.reload();
        input::reload_bindings();
    }

    void begin_capture(bool pad) override { input::begin_capture(!pad); }
    bool capturing() override { return input::capturing(); }
    bool poll_capture(std::string& name) override { return input::poll_capture(name); }
    void cancel_capture() override { input::cancel_capture(); }
    void set_suspended(bool suspended) override { input::set_suspended(suspended); }
    bool take_toggle_request() override { return input::take_overlay_request(); }
    void quit() override { quit_ = true; }

    bool quit_ = false;

private:
    settings::Store& store_;
};

std::unique_ptr<GameCubeHost> g_host;
std::unique_ptr<ov::Overlay> g_overlay;

}  // namespace

bool start(SDL_Window* window, settings::Store& store, const std::string& manifest) {
    std::string path = manifest;
    if (path.empty())
        if (const char* env = std::getenv("TOREC_MANIFEST")) path = env;
    if (path.empty()) {
        std::fprintf(stderr, "[overlay] no manifest (TOREC_MANIFEST): the settings overlay is off\n");
        return false;
    }
    g_host = std::make_unique<GameCubeHost>(store);
    ov::Options options;
    options.manifest_path = path;
    options.paths.data = store.data_dir();
    options.paths.install = std::filesystem::path(path).parent_path().string();
    options.host = g_host.get();
    std::string error;
    g_overlay = ov::Overlay::create(window, options, error);
    if (!g_overlay) {
        std::fprintf(stderr, "[overlay] %s: the settings overlay is off\n", error.c_str());
        g_host.reset();
        return false;
    }
    return true;
}

void stop() {
    g_overlay.reset();
    g_host.reset();
}

bool process_event(const SDL_Event& event) { return g_overlay && g_overlay->process_event(event); }
void draw(int window_w, int window_h, double game_fps) {
    if (g_overlay) g_overlay->draw(window_w, window_h, game_fps);
}
bool visible() { return g_overlay && g_overlay->visible(); }
bool quit_requested() { return g_host && g_host->quit_ || (g_overlay && g_overlay->quit_requested()); }

}  // namespace gcrecomp::gp::overlay_glue
