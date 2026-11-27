// SPDX-License-Identifier: MIT
//
// What the GameCube runtime reads from the instance's configuration: the values of `<data>/settings.toml` and
// `<data>/input.toml` that it applies live (window mode, V-Sync, picture shape, volume and device, stick dead zone).
// The runtime only reads. What is shown and edited, and the writing, belong to the settings overlay (To-Recomp/overlay)
// and the launcher, which both work from the game's recomplauncher.json; docs/SETTINGS.md lists the keys this module
// understands.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace gcrecomp::settings {

// How the 4:3 picture is placed in the window.
enum class Aspect : uint8_t {
    Fit,       // letterboxed at 4:3
    Stretch,   // fills the window
    Wide16x9,  // the 4:3 picture stretched into the largest 16:9 box that fits
    Wide21x9,  // the same for 21:9
};

enum class Stats : uint8_t { Off, Fps };

struct Values {
    struct Video {
        bool fullscreen = false;
        bool vsync = true;
        Aspect aspect = Aspect::Fit;
        Stats stats = Stats::Off;
        bool operator==(const Video&) const = default;
    } video;
    struct Audio {
        float volume = 1.0f;  // 0..1, a linear gain
        std::string device;   // name of a playback device; empty = the system's default
        bool operator==(const Audio&) const = default;
    } audio;
    struct Input {
        std::string glyphs = "GameCube";  // controller family drawn by the overlay: GameCube, Xbox, Nintendo, PlayStation
        float stick_deadzone = 0.125f;    // fraction of the stick's travel that reads as centred, 0..0.5
        bool operator==(const Input&) const = default;
    } input;
    bool operator==(const Values&) const = default;
};

// Names used in the files.
const char* aspect_name(Aspect a);                // "fit", "stretch", "wide16x9", "wide21x9"
bool parse_aspect(const std::string& s, Aspect& out);
const char* stats_name(Stats s);                  // "off", "fps"
bool parse_stats(const std::string& s, Stats& out);
bool is_glyph_family(const std::string& s);       // GameCube, Xbox, Nintendo, PlayStation
const char* const* glyph_families(int& count);    // the four names, in menu order

// Clamps what a file or a caller may have got wrong (volume to 0..1, dead zone to 0..0.5, unknown glyph family to
// the default). Never fails: a bad value in a file falls back to the default for that key.
Values sanitized(Values v);

// The values of the files plus the settings that apply for one run (never saved).
class Store {
public:
    // `data_dir` is the instance's data folder; empty = nothing is read or written (a development run). Missing
    // files give the defaults; a malformed value is reported on stderr and keeps the default of that key.
    explicit Store(std::string data_dir);

    // `RECOMPLAUNCHER_MODE=bigpicture` starts full screen for that run only; the stored value is unchanged.
    Values values() const;
    // The stored values, without the run's overrides.
    Values stored() const;
    bool fullscreen_forced() const { return force_fullscreen_; }

    // Re-reads the files (the launcher may have changed them while the game runs). Returns true when a value changed.
    bool reload();

    // Incremented by every change; readers keep the last value they saw and compare.
    uint64_t generation() const;

    const std::string& data_dir() const { return dir_; }
    std::string settings_path() const;  // empty without a data folder
    std::string input_path() const;

private:
    bool load(Values& out) const;

    std::string dir_;
    bool force_fullscreen_ = false;
    mutable std::mutex m_;
    Values values_;
    uint64_t generation_ = 0;
};

}  // namespace gcrecomp::settings
