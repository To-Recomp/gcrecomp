// SPDX-License-Identifier: MIT
//
// Diagnostic bundle: a key (F10 in the window) or the script command `diag NAME` asks for a record of the next
// presented frame, written as a directory <dir>/diag-<n>-<timestamp>[-NAME]/ (docs/DEBUGGING.md):
//   diag.txt     every draw and EFB copy of the frame with the state they used (what GP_TRACE_FRAME and
//                GP_TRACE_STATE print), the XFB copy last
//   summary.txt  frame number, counts per kind, the features the frame used and whether they are implemented
//   window.ppm   the window as presented
//   efb-NN-*.ppm the EFB (colour; alpha in the .alpha.pgm next to it) at each EFB-to-texture copy and at the XFB
//                copy, so a screen-space effect built from copies can be looked at stage by stage
//
// Nothing here touches OpenGL, SDL or the guest memory: the trigger state machine, the recorded data and the
// writer are plain C++ so that they can be unit-tested without a display (tests/diag_test.cpp). The parser and
// the renderer feed the Bundle while the Trigger says a frame is being recorded; with nothing requested the whole
// cost is one relaxed atomic load per draw and copy.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gcrecomp::gp::diag {

// ---- The trigger ----------------------------------------------------------------------------------

// Idle -> Armed (a request came in; the frame in progress is already partly drawn, so it is skipped) ->
// Capturing (everything of the next frame is recorded) -> Idle (that frame's XFB copy finished the bundle).
enum class Phase : int { Idle, Armed, Capturing };

class Trigger {
public:
    // Asks for a bundle. `name` is sanitised (see sanitize_name). False when one is already pending.
    bool request(const std::string& name);
    // True while the frame being drawn is recorded. The only call on the hot path.
    bool capturing() const { return phase_.load(std::memory_order_relaxed) == int(Phase::Capturing); }
    Phase phase() const { return Phase(phase_.load(std::memory_order_relaxed)); }

    enum class Edge { None, Begin, Finish };
    // Called at every EFB copy to the XFB (a presented frame), before the copy itself is recorded. Armed becomes
    // Capturing (Begin: the frame that starts now is the one recorded); Capturing stays Capturing and reports
    // Finish: the caller records that last copy, completes the Bundle and calls done().
    Edge frame_presented();
    // The bundle was written (or given up): back to Idle. Returns the name that was requested.
    std::string done();

private:
    std::atomic<int> phase_{int(Phase::Idle)};
    std::mutex mutex_;
    std::string name_;
};

// ---- What a frame records -------------------------------------------------------------------------

constexpr int kMaxEfbImages = 8;  // EFB-to-texture copies kept as pictures (the XFB copy is always kept)

struct Image {
    std::string label;  // file name part: "copy03-0x00A1B2C0-I8", "xfb"
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;    // w*h*3, top row first
    std::vector<uint8_t> alpha;  // w*h, empty when not captured
};

struct Feature {
    uint64_t count = 0;
    bool implemented = true;
};

struct Bundle {
    std::string name;
    uint64_t frame = 0;  // number of the frame (GP_TRACE_FRAME numbering)
    int number = 0;      // n of diag-<n>-...
    std::string trace;   // diag.txt
    std::map<std::string, uint64_t> counts;
    std::map<std::string, Feature> features;
    std::vector<Image> efb;
    int efb_dropped = 0;  // texture copies beyond kMaxEfbImages
    Image window;         // the presented window (empty when there is no renderer)

    void reset();
    void count(const std::string& kind, uint64_t n = 1) { counts[kind] += n; }
    void feature(const std::string& name, bool implemented);
};

extern Trigger g_trigger;
extern Bundle g_bundle;

// ---- Pure helpers ---------------------------------------------------------------------------------

// printf into a string.
void vappendf(std::string& out, const char* fmt, va_list ap);
void appendf(std::string& out, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// "YYYYMMDD-HHMMSS" in UTC from a Unix time (no locale, no time zone database: the same on every platform).
std::string format_timestamp(int64_t unix_seconds);
// Keeps A-Z a-z 0-9 . _ - (anything else becomes '_'), at most 40 characters: the name becomes part of a path.
std::string sanitize_name(const std::string& name);
// "diag-<n>-<timestamp>" plus "-<name>" when the name is not empty.
std::string bundle_dir_name(int n, int64_t unix_seconds, const std::string& name);

// The register-level features of a draw (BP and XF register arrays of gp::State): counted in `b` with whether this
// renderer implements them. A frame that uses a feature we do not implement is what the owner looks for first.
void note_draw_features(Bundle& b, const uint32_t* bp, const uint32_t* xf);
// The same for an EFB copy (BP 0x52 command, BP 0x53/0x54 filter coefficients).
void note_copy_features(Bundle& b, uint32_t copy_cmd, uint32_t bp53, uint32_t bp54, bool depth);

std::string summary_text(const Bundle& b);

// Writes the bundle into `dir` (created). False and `error` set when a file cannot be written.
bool write_bundle(const std::string& dir, const Bundle& b, std::string& error);

// Where bundles go: GCRECOMP_DIAG_DIR, else the screenshot directory the renderer was given (TOS_SCREENSHOT_DIR of the
// host program), else the current directory.
void set_default_dir(const char* screenshot_dir);
std::string output_dir();

// The next frame's bundle is complete (the XFB copy was recorded): numbers it, writes it, logs the path on stderr and
// returns the trigger to Idle. `frame` is the number of the frame that was recorded.
void finish(uint64_t frame);

}  // namespace gcrecomp::gp::diag
