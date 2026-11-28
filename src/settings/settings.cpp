// SPDX-License-Identifier: MIT
#include "gcrecomp/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "toml_edit.h"

namespace gcrecomp::settings {

namespace {

constexpr const char* kGlyphFamilies[] = {"GameCube", "Xbox", "Nintendo", "PlayStation"};

// Reads one key; a value of the wrong type is reported and ignored.
template <class T, class Get>
void read_key(const TomlDoc& doc, const char* file, const std::string& table, const char* key, Get get, T& out) {
    if (!doc.raw(table, key)) return;
    if (const auto v = get(doc, table, key)) {
        out = *v;
        return;
    }
    std::fprintf(stderr, "[settings] %s: %s%s%s has an unusable value: using the default\n", file, table.c_str(), table.empty() ? "" : "/", key);
}

}  // namespace

const char* aspect_name(Aspect a) {
    switch (a) {
        case Aspect::Fit: return "fit";
        case Aspect::Stretch: return "stretch";
        case Aspect::Wide16x9: return "wide16x9";
        case Aspect::Wide21x9: return "wide21x9";
    }
    return "fit";
}

bool parse_aspect(const std::string& s, Aspect& out) {
    for (const Aspect a : {Aspect::Fit, Aspect::Stretch, Aspect::Wide16x9, Aspect::Wide21x9}) {
        if (s == aspect_name(a)) {
            out = a;
            return true;
        }
    }
    return false;
}

const char* stats_name(Stats s) { return s == Stats::Fps ? "fps" : "off"; }

bool parse_stats(const std::string& s, Stats& out) {
    if (s == "off") out = Stats::Off;
    else if (s == "fps") out = Stats::Fps;
    else return false;
    return true;
}

bool is_glyph_family(const std::string& s) {
    return std::any_of(std::begin(kGlyphFamilies), std::end(kGlyphFamilies), [&](const char* f) { return s == f; });
}

const char* const* glyph_families(int& count) {
    count = int(std::size(kGlyphFamilies));
    return kGlyphFamilies;
}

Values sanitized(Values v) {
    if (!std::isfinite(v.audio.volume)) v.audio.volume = Values{}.audio.volume;
    v.audio.volume = std::clamp(v.audio.volume, 0.0f, 1.0f);
    if (!std::isfinite(v.input.stick_deadzone)) v.input.stick_deadzone = Values{}.input.stick_deadzone;
    v.input.stick_deadzone = std::clamp(v.input.stick_deadzone, 0.0f, 0.5f);
    if (!is_glyph_family(v.input.glyphs)) v.input.glyphs = Values{}.input.glyphs;
    return v;
}

Store::Store(std::string data_dir) : dir_(std::move(data_dir)) {
    const char* mode = std::getenv("RECOMPLAUNCHER_MODE");
    force_fullscreen_ = mode && std::strcmp(mode, "bigpicture") == 0;
    load(values_);
}

std::string Store::settings_path() const { return dir_.empty() ? std::string() : dir_ + "/settings.toml"; }
std::string Store::input_path() const { return dir_.empty() ? std::string() : dir_ + "/input.toml"; }

bool Store::load(Values& out) const {
    Values v;
    if (dir_.empty()) {
        out = v;
        return true;
    }
    std::string text;
    if (read_text_file(settings_path(), text)) {
        const TomlDoc doc(text);
        read_key(doc, "settings.toml", "video", "fullscreen", [](auto& d, auto& t, auto k) { return d.get_bool(t, k); }, v.video.fullscreen);
        read_key(doc, "settings.toml", "video", "vsync", [](auto& d, auto& t, auto k) { return d.get_bool(t, k); }, v.video.vsync);
        if (doc.raw("video", "aspect")) {
            if (const auto a = doc.get_string("video", "aspect"); !a || !parse_aspect(*a, v.video.aspect))
                std::fprintf(stderr, "[settings] settings.toml: video/aspect is not fit, stretch, wide16x9 or wide21x9: using the default\n");
        }
        if (doc.raw("video", "stats")) {
            if (const auto a = doc.get_string("video", "stats"); !a || !parse_stats(*a, v.video.stats))
                std::fprintf(stderr, "[settings] settings.toml: video/stats is not off or fps: using the default\n");
        }
        double volume = v.audio.volume;
        read_key(doc, "settings.toml", "audio", "volume", [](auto& d, auto& t, auto k) { return d.get_number(t, k); }, volume);
        v.audio.volume = float(volume);
        read_key(doc, "settings.toml", "audio", "device", [](auto& d, auto& t, auto k) { return d.get_string(t, k); }, v.audio.device);
    }
    text.clear();
    if (read_text_file(input_path(), text)) {
        const TomlDoc doc(text);
        if (doc.raw("", "glyphs")) {
            if (const auto g = doc.get_string("", "glyphs"); g && is_glyph_family(*g)) v.input.glyphs = *g;
            else std::fprintf(stderr, "[settings] input.toml: glyphs is not GameCube, Xbox, Nintendo or PlayStation: using the default\n");
        }
        double dz = v.input.stick_deadzone;
        read_key(doc, "input.toml", "", "stick_deadzone", [](auto& d, auto& t, auto k) { return d.get_number(t, k); }, dz);
        v.input.stick_deadzone = float(dz);
    }
    out = sanitized(std::move(v));
    return true;
}

Values Store::stored() const {
    std::lock_guard<std::mutex> lock(m_);
    return values_;
}

Values Store::values() const {
    Values v = stored();
    if (force_fullscreen_) v.video.fullscreen = true;
    return v;
}

uint64_t Store::generation() const {
    std::lock_guard<std::mutex> lock(m_);
    return generation_;
}

bool Store::reload() {
    Values v;
    load(v);
    std::lock_guard<std::mutex> lock(m_);
    if (v == values_) return false;
    values_ = v;
    ++generation_;
    return true;
}

}  // namespace gcrecomp::settings
