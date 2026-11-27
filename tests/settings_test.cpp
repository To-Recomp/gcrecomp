// SPDX-License-Identifier: MIT
// Unit tests for what the runtime reads from the instance's configuration (src/settings, include/gcrecomp/settings.h):
// the TOML reader and the Store. No window, no device. Writing the files is the overlay's job (To-Recomp/overlay).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "gcrecomp/settings.h"
#include "test_env.h"
#include "toml_edit.h"

using namespace gcrecomp::settings;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

namespace fs = std::filesystem;

static void spit(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A fresh empty folder under the temp directory, removed at the end of the scope.
struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() / ("gcrecomp-settings-test-" + std::to_string(++n) + "-" +
                                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

static void test_reading() {
    const TomlDoc d(
        "# header\n"
        "glyphs = \"Xbox\"   # trailing comment\n"
        "[video]\n"
        "fullscreen = true\n"
        "\"vsync\" = false\n"
        "ratio = 0.25\n"
        "weird = \"a # not a comment\"\n"
        "[keyboard]\r\n"
        "a = \"Return|Z\"\r\n");
    CHECK(d.get_string("", "glyphs") == "Xbox");
    CHECK(d.get_bool("video", "fullscreen") == true);
    CHECK(d.get_bool("video", "vsync") == false);  // a quoted key reads like a bare one
    CHECK(d.get_number("video", "ratio") == 0.25);
    CHECK(d.get_string("video", "weird") == "a # not a comment");
    CHECK(!d.get_bool("video", "missing") && !d.get_bool("keyboard", "fullscreen"));  // tables do not leak into each other
    CHECK(!d.get_number("", "glyphs"));                                                // wrong type: absent, not zero
    CHECK(d.get_string("keyboard", "a") == "Return|Z");                                // CRLF files read the same
}

static void test_store() {
    unset_env("RECOMPLAUNCHER_MODE");
    Store none("");  // a development run: no files at all
    CHECK(none.values() == Values{} && none.generation() == 0);

    TempDir dir;
    spit(dir.path / "settings.toml",
         "[video]\nfullscreen = true\nvsync = false\naspect = \"wide16x9\"\nstats = \"fps\"\n[audio]\nvolume = 0.4\ndevice = \"USB\"\n");
    spit(dir.path / "input.toml", "glyphs = \"Nintendo\"\nstick_deadzone = 0.2\n[keyboard]\na = \"J\"\n");
    Store s(dir.path.string());
    const Values got = s.values();
    CHECK(got.video.fullscreen && !got.video.vsync && got.video.aspect == Aspect::Wide16x9 && got.video.stats == Stats::Fps);
    CHECK(got.audio.volume == 0.4f && got.audio.device == "USB");
    CHECK(got.input.glyphs == "Nintendo" && got.input.stick_deadzone == 0.2f);

    // Values nobody should write are repaired key by key, the rest is kept.
    TempDir bad;
    spit(bad.path / "settings.toml", "[video]\naspect = \"cinema\"\nvsync = false\n[audio]\nvolume = 7\n");
    spit(bad.path / "input.toml", "glyphs = \"Sega\"\nstick_deadzone = 0.9\n");
    Store b(bad.path.string());
    CHECK(b.values().video.aspect == Aspect::Fit && !b.values().video.vsync);
    CHECK(b.values().audio.volume == 1.0f);          // out of range: clamped
    CHECK(b.values().input.glyphs == "GameCube");    // unknown family: default
    CHECK(b.values().input.stick_deadzone == 0.5f);  // clamped

    // The launcher asks for full screen for one run: the stored value is unchanged.
    set_env("RECOMPLAUNCHER_MODE", "bigpicture");
    TempDir big;
    Store p(big.path.string());
    CHECK(p.values().video.fullscreen && !p.stored().video.fullscreen && p.fullscreen_forced());
    unset_env("RECOMPLAUNCHER_MODE");
}

static void test_reload() {
    unset_env("RECOMPLAUNCHER_MODE");
    TempDir dir;
    spit(dir.path / "settings.toml", "[video]\nvsync = true\n[audio]\nvolume = 1.0\n");
    Store s(dir.path.string());
    CHECK(s.values().video.vsync && s.generation() == 0);
    CHECK(!s.reload() && s.generation() == 0);  // nothing changed

    // The overlay or the launcher edits the file while the game runs: reload() notices and counts it.
    spit(dir.path / "settings.toml", "[video]\nvsync = false\n[audio]\nvolume = 0.5\ndevice = \"HDMI\"\n");
    CHECK(s.reload() && s.generation() == 1);
    CHECK(!s.values().video.vsync && s.values().audio.volume == 0.5f && s.values().audio.device == "HDMI");
    CHECK(!s.reload() && s.generation() == 1);

    // A malformed value in the middle of a run keeps the default of that key, not the previous value.
    spit(dir.path / "settings.toml", "[video]\nvsync = \"maybe\"\n");
    CHECK(s.reload() && s.values().video.vsync);
}

int main() {
    test_reading();
    test_store();
    test_reload();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all settings tests passed");
    return 0;
}
