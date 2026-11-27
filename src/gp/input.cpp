// SPDX-License-Identifier: MIT
#include "input.h"

#include "gcrecomp/gp/bindings.h"
#include "gcrecomp/gp/gp.h"
#include "diag.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace gcrecomp::gp::input {

// ---- Scaling and gamepad layout -----------------------------------------------------------------

void scale_stick(int16_t sdl_x, int16_t sdl_y, int8_t& x, int8_t& y, int dead_zone) {
    const double fx = sdl_x, fy = -double(sdl_y);  // SDL's y grows downwards, the SDK's upwards
    const double mag = std::hypot(fx, fy);
    if (mag <= dead_zone) {
        x = y = 0;
        return;
    }
    const double k = std::min(1.0, (mag - dead_zone) / (32767.0 - dead_zone)) / mag;
    auto to_i8 = [](double v) { return int8_t(std::clamp(std::lround(v >= 0 ? v * 127.0 : v * 128.0), -128L, 127L)); };
    x = to_i8(fx * k);
    y = to_i8(fy * k);
}

uint8_t scale_trigger(int16_t sdl) {
    if (sdl <= 0) return 0;
    return uint8_t((int(sdl) * 255 + 16383) / 32767);
}

namespace {

const char* const kGamepadInputNames[kGpCount] = {
    "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back", "Guide", "L3", "R3",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "Left Stick Up", "Left Stick Down", "Left Stick Left", "Left Stick Right",
    "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right",
};

// Stick axes in SDL units: how far past the centre in one direction, 0..32767.
int axis_part(int v) { return v > 0 ? std::min(v, 32767) : 0; }

}  // namespace

const char* gamepad_input_name(GamepadInput in) { return kGamepadInputNames[in]; }

bool parse_gamepad_input(const std::string& name, GamepadInput& out) {
    static const struct { const char* alias; GamepadInput input; } kAliases[] = {
        {"Cross", kGpA}, {"Circle", kGpB}, {"Square", kGpX}, {"Triangle", kGpY},
    };
    for (int i = 0; i < kGpCount; ++i) {
        if (name == kGamepadInputNames[i]) {
            out = GamepadInput(i);
            return true;
        }
    }
    for (const auto& a : kAliases) {
        if (name == a.alias) {
            out = a.input;
            return true;
        }
    }
    return false;
}

int gamepad_value(GamepadInput in, const GamepadSnapshot& g) {
    auto digital = [](bool b) { return b ? 32767 : 0; };
    switch (in) {
        case kGpA: return digital(g.south);
        case kGpB: return digital(g.east);
        case kGpX: return digital(g.west);
        case kGpY: return digital(g.north);
        case kGpLB: return digital(g.left_shoulder);
        case kGpRB: return digital(g.right_shoulder);
        case kGpLT: return axis_part(g.left_trigger);
        case kGpRT: return axis_part(g.right_trigger);
        case kGpStart: return digital(g.start);
        case kGpBack: return digital(g.back);
        case kGpGuide: return digital(g.guide);
        case kGpL3: return digital(g.left_stick_click);
        case kGpR3: return digital(g.right_stick_click);
        case kGpDpadUp: return digital(g.dpad_up);
        case kGpDpadDown: return digital(g.dpad_down);
        case kGpDpadLeft: return digital(g.dpad_left);
        case kGpDpadRight: return digital(g.dpad_right);
        case kGpLeftStickUp: return axis_part(-int(g.left_y));
        case kGpLeftStickDown: return axis_part(g.left_y);
        case kGpLeftStickLeft: return axis_part(-int(g.left_x));
        case kGpLeftStickRight: return axis_part(g.left_x);
        case kGpRightStickUp: return axis_part(-int(g.right_y));
        case kGpRightStickDown: return axis_part(g.right_y);
        case kGpRightStickLeft: return axis_part(-int(g.right_x));
        case kGpRightStickRight: return axis_part(g.right_x);
        case kGpCount: break;
    }
    return 0;
}

PadMap default_padmap() {
    PadMap m;
    m.inputs[kCtlA] = {kGpA};  // south
    m.inputs[kCtlB] = {kGpX};  // west
    m.inputs[kCtlX] = {kGpB};  // east
    m.inputs[kCtlY] = {kGpY};  // north
    m.inputs[kCtlStart] = {kGpStart};
    m.inputs[kCtlZ] = {kGpRB};
    m.inputs[kCtlL] = {kGpLT, kGpLB};
    m.inputs[kCtlR] = {kGpRT};
    m.inputs[kCtlDpadUp] = {kGpDpadUp};
    m.inputs[kCtlDpadDown] = {kGpDpadDown};
    m.inputs[kCtlDpadLeft] = {kGpDpadLeft};
    m.inputs[kCtlDpadRight] = {kGpDpadRight};
    m.inputs[kCtlStickUp] = {kGpLeftStickUp};
    m.inputs[kCtlStickDown] = {kGpLeftStickDown};
    m.inputs[kCtlStickLeft] = {kGpLeftStickLeft};
    m.inputs[kCtlStickRight] = {kGpLeftStickRight};
    m.inputs[kCtlCStickUp] = {kGpRightStickUp};
    m.inputs[kCtlCStickDown] = {kGpRightStickDown};
    m.inputs[kCtlCStickLeft] = {kGpRightStickLeft};
    m.inputs[kCtlCStickRight] = {kGpRightStickRight};
    return m;
}

PadState gamepad_to_pad(const GamepadSnapshot& g, const PadMap& map, int dead_zone) {
    auto value = [&](Control c) {
        int v = 0;
        for (const GamepadInput in : map.inputs[c]) v = std::max(v, gamepad_value(in, g));
        return v;
    };
    auto pressed = [&](Control c) { return value(c) >= 16384; };
    PadState p;
    uint16_t b = 0;
    if (pressed(kCtlDpadLeft)) b |= kLeft;
    if (pressed(kCtlDpadRight)) b |= kRight;
    if (pressed(kCtlDpadDown)) b |= kDown;
    if (pressed(kCtlDpadUp)) b |= kUp;
    if (pressed(kCtlA)) b |= kA;
    if (pressed(kCtlB)) b |= kB;
    if (pressed(kCtlX)) b |= kX;
    if (pressed(kCtlY)) b |= kY;
    if (pressed(kCtlStart)) b |= kStart;
    if (pressed(kCtlZ)) b |= kZ;
    p.trigger_l = scale_trigger(int16_t(value(kCtlL)));
    p.trigger_r = scale_trigger(int16_t(value(kCtlR)));
    if (p.trigger_l >= kTriggerClick) b |= kL;
    if (p.trigger_r >= kTriggerClick) b |= kR;
    p.buttons = b;
    // Each stick is four directions: opposite ones cancel, as the keyboard's do.
    auto stick = [&](Control up, Control down, Control left, Control right, int8_t& out_x, int8_t& out_y) {
        const int x = std::clamp(value(right) - value(left), -32768, 32767);
        const int y = std::clamp(value(down) - value(up), -32768, 32767);  // SDL's y grows downwards
        scale_stick(int16_t(x), int16_t(y), out_x, out_y, dead_zone);
    };
    stick(kCtlStickUp, kCtlStickDown, kCtlStickLeft, kCtlStickRight, p.stick_x, p.stick_y);
    stick(kCtlCStickUp, kCtlCStickDown, kCtlCStickLeft, kCtlCStickRight, p.substick_x, p.substick_y);
    return p;
}

PadState gamepad_to_pad(const GamepadSnapshot& g) { return gamepad_to_pad(g, default_padmap(), kStickDeadZone); }

// ---- Keyboard -----------------------------------------------------------------------------------

namespace {

const char* const kControlNames[kControlCount] = {
    "a", "b", "x", "y", "start", "z", "l", "r",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "stick_up", "stick_down", "stick_left", "stick_right",
    "cstick_up", "cstick_down", "cstick_left", "cstick_right",
};

constexpr int kKeyboardStick = 80;

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }

KeyMap default_keymap() {
    KeyMap m;
    m.keys[kCtlA] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_Z};
    m.keys[kCtlB] = {SDL_SCANCODE_X, SDL_SCANCODE_BACKSPACE};
    m.keys[kCtlX] = {SDL_SCANCODE_C};
    m.keys[kCtlY] = {SDL_SCANCODE_V};
    m.keys[kCtlStart] = {SDL_SCANCODE_SPACE};
    m.keys[kCtlZ] = {SDL_SCANCODE_W};
    m.keys[kCtlL] = {SDL_SCANCODE_Q};
    m.keys[kCtlR] = {SDL_SCANCODE_E};
    m.keys[kCtlDpadUp] = m.keys[kCtlStickUp] = {SDL_SCANCODE_UP};
    m.keys[kCtlDpadDown] = m.keys[kCtlStickDown] = {SDL_SCANCODE_DOWN};
    m.keys[kCtlDpadLeft] = m.keys[kCtlStickLeft] = {SDL_SCANCODE_LEFT};
    m.keys[kCtlDpadRight] = m.keys[kCtlStickRight] = {SDL_SCANCODE_RIGHT};
    return m;
}

namespace {

struct Cursor {
    const std::string& s;
    size_t i = 0;
    bool done() const { return i >= s.size(); }
    char peek() const { return s[i]; }
    void skip_space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
};

bool parse_string(Cursor& c, std::string& out, std::string& err) {
    if (c.done() || c.peek() != '"') {
        err = "expected a quoted string";
        return false;
    }
    ++c.i;
    out.clear();
    while (!c.done() && c.peek() != '"') {
        char ch = c.s[c.i++];
        if (ch == '\\') {
            if (c.done()) break;
            const char e = c.s[c.i++];
            if (e == '"' || e == '\\') ch = e;
            else if (e == 't') ch = '\t';
            else {
                err = "unsupported escape sequence";
                return false;
            }
        }
        out += ch;
    }
    if (c.done()) {
        err = "unterminated string";
        return false;
    }
    ++c.i;
    return true;
}

// Rest of the line after a value: spaces and an optional comment.
bool end_of_value(Cursor& c, std::string& err) {
    c.skip_space();
    if (!c.done() && c.peek() != '#' && c.peek() != '\r') {
        err = "unexpected text after the value";
        return false;
    }
    return true;
}

bool parse_value(Cursor& c, std::vector<std::string>& out, std::string& err) {
    out.clear();
    c.skip_space();
    if (!c.done() && c.peek() == '[') {
        ++c.i;
        for (;;) {
            c.skip_space();
            if (c.done()) {
                err = "unterminated array (arrays must fit on one line)";
                return false;
            }
            if (c.peek() == ']') {
                ++c.i;
                break;
            }
            std::string v;
            if (!parse_string(c, v, err)) return false;
            out.push_back(v);
            c.skip_space();
            if (!c.done() && c.peek() == ',') ++c.i;
            else if (c.done() || c.peek() != ']') {
                err = "expected ',' or ']'";
                return false;
            }
        }
        return end_of_value(c, err);
    }
    std::string v;
    if (!parse_string(c, v, err)) return false;
    // One string may hold several names joined by '|' (the launcher's way to write a list); "" is an empty list.
    for (size_t start = 0; start <= v.size();) {
        const size_t bar = v.find('|', start);
        std::string name = v.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (!name.empty()) out.push_back(std::move(name));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return end_of_value(c, err);
}

}  // namespace

bool parse_bindings(const std::string& toml, KeyMap& keys, PadMap& pads, std::string& error) {
    KeyMap new_keys = keys;
    PadMap new_pads = pads;
    enum { kTop, kKeyboard, kGamepad } table = kTop;
    std::istringstream in(toml);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        std::string err;
        auto fail = [&](const std::string& what) {
            error = "line " + std::to_string(n) + ": " + what;
            return false;
        };
        Cursor c{line};
        c.skip_space();
        if (c.done() || c.peek() == '#' || c.peek() == '\r') continue;
        if (c.peek() == '[') {
            const size_t close = line.find(']', c.i);
            if (close == std::string::npos) return fail("unterminated table header");
            std::string name = line.substr(c.i + 1, close - c.i - 1);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (name == "keyboard") table = kKeyboard;
            else if (name == "gamepad") table = kGamepad;
            else return fail("unknown table [" + name + "]");
            c.i = close + 1;
            if (!end_of_value(c, err)) return fail(err);
            continue;
        }
        const size_t eq = line.find('=', c.i);
        if (eq == std::string::npos) return fail("expected 'name = value'");
        std::string key = line.substr(c.i, eq - c.i);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (table == kTop) {
            // glyphs and stick_deadzone belong to the settings (settings.h); anything else needs its table.
            if (key == "glyphs" || key == "stick_deadzone") continue;
            return fail("key '" + key + "' outside the [keyboard] or [gamepad] table");
        }
        int control = -1;
        for (int i = 0; i < kControlCount; ++i)
            if (key == kControlNames[i]) control = i;
        if (control < 0) return fail("unknown control '" + key + "'");
        c.i = eq + 1;
        std::vector<std::string> names;
        if (!parse_value(c, names, err)) return fail(err);
        if (table == kKeyboard) {
            std::vector<int> codes;
            for (const std::string& name : names) {
                const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
                if (sc == SDL_SCANCODE_UNKNOWN) return fail("unknown key name \"" + name + "\"");
                if (is_reserved_key(int(sc)))
                    return fail("\"" + name + "\" is reserved for the " + (sc == SDL_SCANCODE_F1 ? "settings overlay" : "diagnostic hotkey"));
                codes.push_back(int(sc));
            }
            new_keys.keys[control] = std::move(codes);
        } else {
            std::vector<GamepadInput> inputs;
            for (const std::string& name : names) {
                GamepadInput input;
                if (!parse_gamepad_input(name, input)) return fail("unknown gamepad input \"" + name + "\"");
                if (input == kGpGuide) return fail("\"Guide\" is reserved for the settings overlay");
                inputs.push_back(input);
            }
            new_pads.inputs[control] = std::move(inputs);
        }
    }
    keys = std::move(new_keys);
    pads = std::move(new_pads);
    return true;
}

bool parse_keymap(const std::string& toml, KeyMap& map, std::string& error) {
    PadMap scratch = default_padmap();
    return parse_bindings(toml, map, scratch, error);
}

PadState keyboard_to_pad(const KeyMap& map, const bool* keys, int count) {
    auto down = [&](Control c) {
        for (int k : map.keys[c])
            if (k >= 0 && k < count && keys[k]) return true;
        return false;
    };
    PadState p;
    uint16_t b = 0;
    if (down(kCtlDpadLeft)) b |= kLeft;
    if (down(kCtlDpadRight)) b |= kRight;
    if (down(kCtlDpadDown)) b |= kDown;
    if (down(kCtlDpadUp)) b |= kUp;
    if (down(kCtlZ)) b |= kZ;
    if (down(kCtlR)) b |= kR;
    if (down(kCtlL)) b |= kL;
    if (down(kCtlA)) b |= kA;
    if (down(kCtlB)) b |= kB;
    if (down(kCtlX)) b |= kX;
    if (down(kCtlY)) b |= kY;
    if (down(kCtlStart)) b |= kStart;
    p.buttons = b;
    auto axis = [&](Control pos, Control neg) { return int8_t((down(pos) ? kKeyboardStick : 0) - (down(neg) ? kKeyboardStick : 0)); };
    p.stick_x = axis(kCtlStickRight, kCtlStickLeft);
    p.stick_y = axis(kCtlStickUp, kCtlStickDown);
    p.substick_x = axis(kCtlCStickRight, kCtlCStickLeft);
    p.substick_y = axis(kCtlCStickUp, kCtlCStickDown);
    p.trigger_l = (b & kL) ? 255 : 0;
    p.trigger_r = (b & kR) ? 255 : 0;
    return p;
}

bool is_reserved_key(int scancode) { return scancode == SDL_SCANCODE_F10 || scancode == SDL_SCANCODE_F1; }

bool is_neutral(const PadState& p) {
    return p.buttons == 0 && p.stick_x == 0 && p.stick_y == 0 && p.substick_x == 0 && p.substick_y == 0 &&
           p.trigger_l == 0 && p.trigger_r == 0;
}

// ---- Ports --------------------------------------------------------------------------------------

int PortAssigner::port_for(uint32_t device, bool active) {
    for (int i = 0; i < 4; ++i)
        if (owner_[i] == device) return i;
    if (!active) return -1;
    for (int i = 0; i < 4; ++i) {
        if (owner_[i] == 0) {
            owner_[i] = device;
            return i;
        }
    }
    return -1;
}

void PortAssigner::release(uint32_t device) {
    for (uint32_t& o : owner_)
        if (o == device) o = 0;
}

// ---- SDL layer ----------------------------------------------------------------------------------

namespace {

constexpr uint32_t kKeyboardDevice = 0xFFFFFFFFu;  // SDL joystick ids are small; this one is never one

struct Device {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
};

KeyMap g_keymap = default_keymap();
PadMap g_padmap = default_padmap();
std::mutex g_mutex;  // the window thread's events and frame update, and the overlay's edits, share the state below
std::string g_input_path;
bool g_suspended = false;
std::atomic<int> g_dead_zone{kStickDeadZone};
bool g_overlay_request = false, g_overlay_button_down = false;

// A binding being captured for the overlay (begin_capture).
struct Capture {
    bool active = false, keyboard = false, armed = false, ready = false;
    std::string name;
} g_capture;
std::vector<Device> g_gamepads;
PortAssigner g_ports;
Script g_script;
FrameSignature g_signature;
uint64_t g_presented = 0, g_signature_at = 0;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
int g_frame = 0;
bool g_rumbling[4] = {};

GamepadSnapshot read_gamepad(SDL_Gamepad* g) {
    GamepadSnapshot s;
    auto b = [&](SDL_GamepadButton btn) { return SDL_GetGamepadButton(g, btn); };
    auto a = [&](SDL_GamepadAxis ax) { return SDL_GetGamepadAxis(g, ax); };
    s.south = b(SDL_GAMEPAD_BUTTON_SOUTH);
    s.east = b(SDL_GAMEPAD_BUTTON_EAST);
    s.west = b(SDL_GAMEPAD_BUTTON_WEST);
    s.north = b(SDL_GAMEPAD_BUTTON_NORTH);
    s.start = b(SDL_GAMEPAD_BUTTON_START);
    s.left_shoulder = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    s.right_shoulder = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    s.back = b(SDL_GAMEPAD_BUTTON_BACK);
    s.guide = b(SDL_GAMEPAD_BUTTON_GUIDE);
    s.left_stick_click = b(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    s.right_stick_click = b(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    s.dpad_up = b(SDL_GAMEPAD_BUTTON_DPAD_UP);
    s.dpad_down = b(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    s.dpad_left = b(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    s.dpad_right = b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    s.left_x = a(SDL_GAMEPAD_AXIS_LEFTX);
    s.left_y = a(SDL_GAMEPAD_AXIS_LEFTY);
    s.right_x = a(SDL_GAMEPAD_AXIS_RIGHTX);
    s.right_y = a(SDL_GAMEPAD_AXIS_RIGHTY);
    s.left_trigger = a(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s.right_trigger = a(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return s;
}

void open_gamepad(SDL_JoystickID id) {
    for (const Device& d : g_gamepads)
        if (d.id == id) return;
    if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
        g_gamepads.push_back({id, pad});
        std::fprintf(stderr, "[input] gamepad connected: %s\n", SDL_GetGamepadName(pad));
    }
}

void stop_rumble(int port) {
    if (!g_rumbling[port]) return;
    g_rumbling[port] = false;
    for (const Device& d : g_gamepads)
        if (d.id == g_ports.owner(port)) SDL_RumbleGamepad(d.pad, 0, 0, 0);
}


// TOS_SCRIPT is the script text, or "@path" to read it from a file.
void load_script() {
    const char* env = std::getenv("TOS_SCRIPT");
    g_start = std::chrono::steady_clock::now();
    if (!env || !*env) return;
    std::string text = env;
    if (env[0] == '@') {
        std::ifstream f(env + 1);
        if (!f) {
            std::fprintf(stderr, "[script] cannot read %s\n", env + 1);
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    std::string error;
    if (!g_script.parse(text.c_str(), error)) std::fprintf(stderr, "[script] %s\n", error.c_str());
    else std::fprintf(stderr, "[script] %zu command(s) loaded\n", g_script.commands().size());
}

// F10 or the script command `diag NAME`: record the next presented frame (diag.h).
void request_diag(const char* name) {
    if (diag::g_trigger.request(name)) std::fprintf(stderr, "[diag] requested: the next presented frame will be recorded\n");
    else std::fprintf(stderr, "[diag] a bundle is already being recorded\n");
}

uint16_t run_script() {
    if (g_script.empty()) return 0;
    Observation o;
    o.frame = g_frame;
    o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    o.presented = g_presented;
    o.sig = g_signature.empty() ? nullptr : &g_signature;
    o.sig_at = g_signature_at;
    take_opened_disc_files(o.files);
    for (const std::string& f : o.files) std::fprintf(stderr, "[script] frame %d: game opened %s\n", g_frame, f.c_str());
    const uint16_t buttons = g_script.step(o);
    for (int n : g_script.take_traces()) request_trace_frame(uint64_t(n));
    for (const std::string& name : g_script.take_diags()) request_diag(name.c_str());
    for (const std::string& line : g_script.take_log()) std::fprintf(stderr, "[script] %s\n", line.c_str());
    return buttons;
}

}  // namespace

void init(const char* toml_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_keymap = default_keymap();
    g_padmap = default_padmap();
    g_input_path = toml_path ? toml_path : "";
    g_suspended = false;
    g_capture = Capture{};
    if (!g_input_path.empty()) {
        std::ifstream f(g_input_path);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string error;
            if (parse_bindings(ss.str(), g_keymap, g_padmap, error)) std::fprintf(stderr, "[input] bindings loaded from %s\n", g_input_path.c_str());
            else std::fprintf(stderr, "[input] %s: %s; using the default bindings\n", g_input_path.c_str(), error.c_str());
        }
    }
    load_script();
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "[input] gamepads unavailable: %s\n", SDL_GetError());
        return;
    }
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; ++i) open_gamepad(ids[i]);
        SDL_free(ids);
    }
}

void shutdown() {
    for (int p = 0; p < 4; ++p) stop_rumble(p);
    for (const Device& d : g_gamepads) SDL_CloseGamepad(d.pad);
    g_gamepads.clear();
    g_ports = PortAssigner();
}

void handle_event(const SDL_Event& e) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && g_capture.active) {
        // The next key is the answer; Escape abandons it. F1 and F10 belong to the program and are not offered.
        const SDL_Scancode sc = e.key.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) {
            g_capture = Capture{};
        } else if (g_capture.keyboard && !is_reserved_key(int(sc))) {
            const char* name = SDL_GetScancodeName(sc);
            if (name && *name && SDL_GetScancodeFromName(name) == sc) {  // only names that read back to the same key
                g_capture.name = name;
                g_capture.ready = true;
                g_capture.active = false;
            }
        }
        return;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F10) {
        request_diag("");
    } else if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
        open_gamepad(e.gdevice.which);
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (size_t i = 0; i < g_gamepads.size(); ++i) {
            if (g_gamepads[i].id != e.gdevice.which) continue;
            for (int p = 0; p < 4; ++p)
                if (g_ports.owner(p) == g_gamepads[i].id) g_rumbling[p] = false;  // the device is gone
            g_ports.release(g_gamepads[i].id);
            SDL_CloseGamepad(g_gamepads[i].pad);
            g_gamepads.erase(g_gamepads.begin() + long(i));
            std::fprintf(stderr, "[input] gamepad disconnected\n");
            break;
        }
    }
}

void update() {
    std::lock_guard<std::mutex> lock(g_mutex);
    PadState next[4];
    auto place = [&](uint32_t device, const PadState& state) {
        const int port = g_ports.port_for(device, !is_neutral(state));
        if (port >= 0) next[port] = state;
    };
    int key_count = 0;
    const bool* keys = SDL_GetKeyboardState(&key_count);
    const int dead_zone = g_dead_zone.load();
    place(kKeyboardDevice, keyboard_to_pad(g_keymap, keys, key_count));
    bool overlay_button = false;
    int capture_value = 0;
    GamepadInput capture_input = kGpA;
    for (const Device& d : g_gamepads) {
        const GamepadSnapshot snapshot = read_gamepad(d.pad);
        place(d.id, gamepad_to_pad(snapshot, g_padmap, dead_zone));
        overlay_button = overlay_button || snapshot.guide || (snapshot.back && snapshot.start);
        if (g_capture.active && !g_capture.keyboard) {
            for (int i = 0; i < kGpCount; ++i) {
                const int v = i == kGpGuide ? 0 : gamepad_value(GamepadInput(i), snapshot);
                if (v > capture_value) capture_value = v, capture_input = GamepadInput(i);
            }
        }
    }
    // Gamepad capture: wait until everything is released, then the first input pressed or deflected far enough wins.
    if (g_capture.active && !g_capture.keyboard) {
        if (!g_capture.armed) {
            g_capture.armed = capture_value < 8000;
        } else if (capture_value >= 24000) {
            g_capture.name = gamepad_input_name(capture_input);
            g_capture.ready = true;
            g_capture.active = false;
        }
    }
    if (overlay_button && !g_overlay_button_down) g_overlay_request = true;  // on the press, not while it is held
    g_overlay_button_down = overlay_button;
    if (g_suspended) {
        for (PadState& p : next) p = PadState{};  // the overlay is open: the game sees a pad at rest
    }

    ++g_frame;
    next[0].buttons |= run_script();
    if (const Script::Sticks& st = g_script.sticks(); st.x || st.y || st.sub_x || st.sub_y) {
        if (st.x || st.y) next[0].stick_x = st.x, next[0].stick_y = st.y;
        if (st.sub_x || st.sub_y) next[0].substick_x = st.sub_x, next[0].substick_y = st.sub_y;
    }

    for (int p = 0; p < 4; ++p) {
        const bool owned = g_ports.owner(p) != 0;
        PadState& out = g_pad[p];
        const bool rumble = out.rumble;
        out = next[p];
        out.rumble = rumble;
        out.connected = p == 0 || owned;
        if (!out.connected) out.rumble = false;
        if (out.rumble) {
            for (const Device& d : g_gamepads) {
                if (d.id != g_ports.owner(p)) continue;
                SDL_RumbleGamepad(d.pad, 0xC000, 0xC000, 250);  // refreshed every frame while requested
                g_rumbling[p] = true;
            }
        } else {
            stop_rumble(p);
        }
    }
}

bool wants_signature() { return g_script.needs_signature(); }

void notify_frame(const FrameSignature* sig) {
    ++g_presented;
    if (sig) {
        g_signature = *sig;
        g_signature_at = g_presented;
    }
}

std::vector<std::string> take_shot_requests() { return g_script.take_shots(); }

// ---- Bindings file (bindings.h) -------------------------------------------------------------------

namespace {

bool read_input_file(const std::string& path, std::string& text) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

}  // namespace

void reload_bindings() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string text;
    if (g_input_path.empty() || !read_input_file(g_input_path, text)) return;
    KeyMap keys = default_keymap();
    PadMap pads = default_padmap();
    std::string error;
    if (!parse_bindings(text, keys, pads, error)) {
        std::fprintf(stderr, "[input] %s: %s; keeping the current bindings\n", g_input_path.c_str(), error.c_str());
        return;
    }
    g_keymap = std::move(keys);
    g_padmap = std::move(pads);
}

void begin_capture(bool keyboard) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_capture = Capture{};
    g_capture.active = true;
    g_capture.keyboard = keyboard;
}

bool capturing() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_capture.active;
}

bool poll_capture(std::string& name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_capture.ready) return false;
    name = std::move(g_capture.name);
    g_capture = Capture{};
    return true;
}

void cancel_capture() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_capture = Capture{};
}

void set_suspended(bool suspended) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_suspended = suspended;
}

bool suspended() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_suspended;
}

void set_stick_deadzone(float fraction) { g_dead_zone = int(std::lround(std::clamp(fraction, 0.0f, 0.5f) * 32767.0f)); }

std::vector<std::string> connected_gamepads() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::string> names;
    for (const Device& d : g_gamepads) {
        const char* n = SDL_GetGamepadName(d.pad);
        names.push_back(n && *n ? n : "Gamepad");
    }
    return names;
}

bool take_overlay_request() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool requested = g_overlay_request;
    g_overlay_request = false;
    return requested;
}
}  // namespace gcrecomp::gp::input
