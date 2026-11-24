// SPDX-License-Identifier: MIT
#include "diag.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "efb_copy.h"
#include "gp_decode.h"

namespace gcrecomp::gp::diag {

Trigger g_trigger;
Bundle g_bundle;

// ---- Trigger -------------------------------------------------------------------------------------

bool Trigger::request(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_.load() != int(Phase::Idle)) return false;
    name_ = sanitize_name(name);
    phase_.store(int(Phase::Armed));
    return true;
}

Trigger::Edge Trigger::frame_presented() {
    const int p = phase_.load(std::memory_order_relaxed);
    if (p == int(Phase::Armed)) {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_.store(int(Phase::Capturing));
        return Edge::Begin;
    }
    return p == int(Phase::Capturing) ? Edge::Finish : Edge::None;
}

std::string Trigger::done() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string name = std::move(name_);
    name_.clear();
    phase_.store(int(Phase::Idle));
    return name;
}

// ---- Bundle --------------------------------------------------------------------------------------

void Bundle::reset() {
    name.clear();
    frame = 0;
    number = 0;
    trace.clear();
    counts.clear();
    features.clear();
    efb.clear();
    efb_dropped = 0;
    window = Image();
}

void Bundle::feature(const std::string& name, bool implemented) {
    Feature& f = features[name];
    ++f.count;
    f.implemented = implemented;
}

// ---- Helpers -------------------------------------------------------------------------------------

void vappendf(std::string& out, const char* fmt, va_list ap) {
    char small[512];
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(small, sizeof small, fmt, ap);
    if (n >= 0) {
        if (size_t(n) < sizeof small) {
            out.append(small, size_t(n));
        } else {
            const size_t old = out.size();
            out.resize(old + size_t(n) + 1);
            std::vsnprintf(&out[old], size_t(n) + 1, fmt, ap2);
            out.resize(old + size_t(n));
        }
    }
    va_end(ap2);
}

void appendf(std::string& out, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vappendf(out, fmt, ap);
    va_end(ap);
}

std::string format_timestamp(int64_t t) {
    // Civil date from days since 1970-01-01 (proleptic Gregorian, valid for the whole int64 range of interest).
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    const int64_t secs = t - days * 86400;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d", int(y), int(m), int(d), int(secs / 3600), int(secs / 60 % 60), int(secs % 60));
    return buf;
}

std::string sanitize_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (out.size() >= 40) break;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    // A name of dots only would be "." or "..": a path component with a meaning.
    if (!out.empty() && out.find_first_not_of('.') == std::string::npos) out.assign(out.size(), '_');
    return out;
}

std::string bundle_dir_name(int n, int64_t unix_seconds, const std::string& name) {
    std::string s = "diag-" + std::to_string(n) + "-" + format_timestamp(unix_seconds);
    const std::string clean = sanitize_name(name);
    if (!clean.empty()) s += "-" + clean;
    return s;
}

// ---- Features ------------------------------------------------------------------------------------

void note_draw_features(Bundle& b, const uint32_t* bp, const uint32_t* xf) {
    if (indirect_tev_stages(bp)) b.feature("indirect texturing", false);
    const FogParams fog = decode_fog(bp);
    if (fog.type != kFogOff) {
        b.feature("fog", true);
        if ((bp[0xE8] >> 10) & 1) b.feature("fog range adjust", false);
    }
    const int nst = int(((bp[0] >> 10) & 0xF) + 1);
    for (int i = 0; i < nst; ++i) {
        if (((bp[0xC0 + 2 * i] >> 16) & 3) == 3 || ((bp[0xC1 + 2 * i] >> 16) & 3) == 3) {
            b.feature("TEV compare mode", true);
            break;
        }
    }
    const BlendState blend = decode_blend(bp[0x41], bp[0x42]);
    if (blend.const_alpha) b.feature("constant destination alpha", true);
    if (blend.logic_op) b.feature("blend logic op", true);
    if (early_z_writes_rejected_pixels(bp)) b.feature("early Z with alpha test", true);
    if ((bp[0xF5] >> 2) & 3) b.feature("Z texture", false);
    for (uint32_t i = 0; i < (bp[0] & 0xF) && i < 8; ++i)
        if ((xf[0x1040 + i] >> 1) & 1) {
            b.feature("projective texgen", true);
            break;
        }
    // Texture maps the TEV stages sample.
    bool used[8] = {};
    for (int i = 0; i < nst; ++i) {
        const uint32_t tref = bp[0x28 + i / 2];
        const uint32_t bits = (i & 1) ? (tref >> 12) : tref;
        if ((bits >> 6) & 1) used[bits & 7] = true;
    }
    for (int m = 0; m < 8; ++m) {
        if (!used[m]) continue;
        const uint32_t mode0 = bp[m < 4 ? 0x80 + m : 0xA0 + (m - 4)];
        if (lod_bias_of(mode0) != 0.0f) b.feature("texture LOD bias", true);
        if (((mode0 >> 5) & 7) >= 1 && ((mode0 >> 5) & 7) != 3 && ((mode0 >> 5) & 7) != 7) b.feature("mipmap minification filter", true);
    }
}

void note_copy_features(Bundle& b, uint32_t cmd, uint32_t bp53, uint32_t bp54, bool depth) {
    if (!depth && !make_copy_filter(bp53, bp54).identity()) b.feature("EFB copy vertical filter", true);
    if ((cmd >> 7) & 3) b.feature("EFB copy gamma", false);
    if ((cmd >> 2) & 1) b.feature("EFB copy YUV", false);
    if ((cmd >> 12) & 3) b.feature("EFB copy frame-to-field", false);
    if ((cmd >> 9) & 1) b.feature("EFB copy half scale / mip generation", true);
    if ((cmd >> 10) & 1) b.feature("EFB copy Y scale", true);
    if (depth) b.feature("EFB depth copy", true);
}

// ---- Summary and files ---------------------------------------------------------------------------

std::string summary_text(const Bundle& b) {
    std::string s;
    appendf(s, "frame %llu\n", (unsigned long long)b.frame);
    if (!b.name.empty()) appendf(s, "name %s\n", b.name.c_str());
    appendf(s, "bundle %d\n\n", b.number);
    s += "counts\n";
    if (b.counts.empty()) s += "  (none)\n";
    for (const auto& kv : b.counts) appendf(s, "  %-28s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
    s += "\nfeatures used (draws or copies that used them)\n";
    if (b.features.empty()) s += "  (none of the tracked ones)\n";
    for (const auto& kv : b.features)
        appendf(s, "  %-40s %-16s %llu\n", kv.first.c_str(), kv.second.implemented ? "implemented" : "NOT IMPLEMENTED", (unsigned long long)kv.second.count);
    appendf(s, "\nefb pictures: %zu (%d more texture copies were not kept; the limit is %d)\n", b.efb.size(), b.efb_dropped, kMaxEfbImages);
    for (const Image& im : b.efb) appendf(s, "  efb-%s.ppm %dx%d\n", im.label.c_str(), im.w, im.h);
    if (!b.window.rgb.empty()) appendf(s, "window.ppm %dx%d\n", b.window.w, b.window.h);
    else s += "window.ppm not written (no renderer)\n";
    return s;
}

namespace {

bool write_file(const std::filesystem::path& path, const void* data, size_t n, const char* header, std::string& error) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) {
        error = "cannot write " + path.string();
        return false;
    }
    bool ok = true;
    if (header) ok = std::fputs(header, f) >= 0;
    if (n) ok = ok && std::fwrite(data, 1, n, f) == n;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) error = "short write to " + path.string();
    return ok;
}

bool write_image(const std::filesystem::path& dir, const std::string& stem, const Image& im, std::string& error) {
    if (im.rgb.empty()) return true;
    char head[64];
    std::snprintf(head, sizeof head, "P6\n%d %d\n255\n", im.w, im.h);
    if (!write_file(dir / (stem + ".ppm"), im.rgb.data(), im.rgb.size(), head, error)) return false;
    if (im.alpha.empty()) return true;
    std::snprintf(head, sizeof head, "P5\n%d %d\n255\n", im.w, im.h);
    return write_file(dir / (stem + ".alpha.pgm"), im.alpha.data(), im.alpha.size(), head, error);
}

}  // namespace

bool write_bundle(const std::string& dir, const Bundle& b, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    const fs::path d(dir);
    if (!write_file(d / "diag.txt", b.trace.data(), b.trace.size(), nullptr, error)) return false;
    const std::string summary = summary_text(b);
    if (!write_file(d / "summary.txt", summary.data(), summary.size(), nullptr, error)) return false;
    if (!write_image(d, "window", b.window, error)) return false;
    for (const Image& im : b.efb)
        if (!write_image(d, "efb-" + im.label, im, error)) return false;
    return true;
}

// ---- Output directory and completion -------------------------------------------------------------

namespace {
std::string g_screenshot_dir;
int g_bundle_number = 0;
}  // namespace

void set_default_dir(const char* screenshot_dir) { g_screenshot_dir = screenshot_dir ? screenshot_dir : ""; }

std::string output_dir() {
    if (const char* e = std::getenv("GCRECOMP_DIAG_DIR"); e && *e) return e;
    if (!g_screenshot_dir.empty()) return g_screenshot_dir;
    return ".";
}

void finish(uint64_t frame) {
    Bundle& b = g_bundle;
    b.frame = frame;
    b.number = ++g_bundle_number;
    b.name = g_trigger.done();
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string dir = (std::filesystem::path(output_dir()) / bundle_dir_name(b.number, now, b.name)).string();
    std::string error;
    if (write_bundle(dir, b, error)) std::fprintf(stderr, "[diag] frame %llu: bundle written to %s\n", (unsigned long long)frame, dir.c_str());
    else std::fprintf(stderr, "[diag] frame %llu: %s\n", (unsigned long long)frame, error.c_str());
    b.reset();
}

}  // namespace gcrecomp::gp::diag
