// SPDX-License-Identifier: MIT
//
// The window layer's view of the settings overlay (To-Recomp/overlay): a handful of calls that do nothing when the
// runtime is built without it (GCRECOMP_NO_OVERLAY) or no manifest was given. The overlay reads the game's
// recomplauncher.json to know what to show and edit; this side only supplies the devices (overlay_host.cpp).
#pragma once

#include <string>

#include "gcrecomp/settings.h"

struct SDL_Window;
union SDL_Event;

namespace gcrecomp::gp::overlay_glue {

#ifdef GCRECOMP_NO_OVERLAY
inline bool start(SDL_Window*, settings::Store&, const std::string&) { return false; }
inline void stop() {}
inline bool process_event(const SDL_Event&) { return false; }
inline void draw(int, int, double) {}
inline bool visible() { return false; }
inline bool quit_requested() { return false; }
#else
// Creates the overlay over `window` (the GL context must be current). `manifest` is the path of the game's
// recomplauncher.json; empty = the TOREC_MANIFEST environment variable; neither = no overlay (false). The data folder
// is the store's.
bool start(SDL_Window* window, settings::Store& store, const std::string& manifest);
void stop();
bool process_event(const SDL_Event& event);
void draw(int window_w, int window_h, double game_fps);
bool visible();
bool quit_requested();
#endif

}  // namespace gcrecomp::gp::overlay_glue
