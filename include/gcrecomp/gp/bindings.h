// SPDX-License-Identifier: MIT
//
// What the settings overlay needs from the GameCube input layer. The bindings themselves live in `<data>/input.toml`
// (`[keyboard]` and `[gamepad]` tables, one key per control, names joined by `|`); the overlay (To-Recomp/overlay)
// edits that file from the game's manifest, and this layer re-reads it.
#pragma once

#include <string>
#include <vector>

namespace gcrecomp::gp::input {

// Re-reads `[keyboard]` and `[gamepad]` of input.toml (the overlay or the launcher may have changed them); a malformed
// file leaves the live bindings alone.
void reload_bindings();

// Capture: after begin_capture(true) the next key press (or after begin_capture(false) the next gamepad button or
// stick/trigger movement, once everything was released) is consumed instead of reaching the game or the overlay, and
// its name is returned once by poll_capture(). Escape on the keyboard abandons a capture of either kind (nothing is
// returned); cancel_capture() does the same from code. F1 and F10 are never offered as an answer.
void begin_capture(bool keyboard);
bool capturing();
bool poll_capture(std::string& name);
void cancel_capture();

// While suspended the game reads a neutral controller on every port (the overlay is open).
void set_suspended(bool suspended);
bool suspended();

// Dead zone of the gamepad sticks, a fraction of the travel (0..0.5). Applied immediately.
void set_stick_deadzone(float fraction);

// Names of the connected gamepads, in the order they were found.
std::vector<std::string> connected_gamepads();

// The gamepad's "Guide" (home) button or Back + Start together pressed since the last call: the overlay's open/close
// button. Keyboard F1 is the overlay's own.
bool take_overlay_request();

}  // namespace gcrecomp::gp::input
