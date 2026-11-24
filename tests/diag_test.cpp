// SPDX-License-Identifier: MIT
// Unit tests for the diagnostic bundle (src/gp/diag.*): the trigger state machine, the names, the recorded features
// and the files written. No window, no GL, no disc.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "diag.h"
#include "test_env.h"

using namespace gcrecomp::gp::diag;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// What the parser does at each XFB copy, with a frame counter: returns the frame the bundle covers (0 when none ended).
struct Driver {
    Trigger t;
    uint64_t frame = 0;  // the frame being drawn
    std::string trace;   // what the draws of recorded frames added
    uint64_t finished = 0;
    // One frame: `draws` draws, then its XFB copy.
    void run_frame(int draws) {
        for (int i = 0; i < draws; ++i)
            if (t.capturing()) trace += "draw of frame " + std::to_string(frame) + "\n";
        const Trigger::Edge e = t.frame_presented();
        if (e == Trigger::Edge::Finish) {
            finished = frame;
            t.done();
        }
        ++frame;
    }
};

static void test_trigger() {
    Trigger t;
    CHECK(t.phase() == Phase::Idle && !t.capturing());
    CHECK(t.frame_presented() == Trigger::Edge::None);  // nothing asked: nothing happens, whatever the frame
    CHECK(t.request("a b/c"));
    CHECK(t.phase() == Phase::Armed && !t.capturing());  // the frame in progress is not recorded
    CHECK(!t.request("again"));                          // one at a time
    CHECK(t.frame_presented() == Trigger::Edge::Begin);
    CHECK(t.capturing());
    CHECK(!t.request("while recording"));
    CHECK(t.frame_presented() == Trigger::Edge::Finish);
    CHECK(t.capturing());  // still on until the caller has recorded the last copy and says done()
    CHECK(t.done() == "a_b_c");
    CHECK(t.phase() == Phase::Idle && !t.capturing());
    CHECK(t.request(""));  // a new one can follow
    CHECK(t.done().empty());
}

static void test_frame_sequence() {
    Driver d;
    d.run_frame(3);
    d.run_frame(3);
    d.t.request("x");  // pressed during frame 2
    d.run_frame(5);    // frame 2 ends: begin
    CHECK(d.trace.empty());
    d.run_frame(4);    // frame 3 is the recorded one
    CHECK(d.finished == 3);
    CHECK(!d.t.capturing());
    int lines = 0;
    for (char c : d.trace) lines += c == '\n';
    CHECK(lines == 4);
    CHECK(d.trace.find("frame 3") != std::string::npos && d.trace.find("frame 2") == std::string::npos);
    d.run_frame(2);  // zero cost afterwards
    CHECK(lines == 4 && d.trace.size() == 4 * std::string("draw of frame 3\n").size());
}

static void test_names() {
    CHECK(format_timestamp(0) == "19700101-000000");
    CHECK(format_timestamp(1700000000) == "20231114-221320");
    CHECK(format_timestamp(951782400) == "20000229-000000");
    CHECK(format_timestamp(951782399) == "20000228-235959");
    CHECK(format_timestamp(-1) == "19691231-235959");
    CHECK(sanitize_name("Va-t-en, Colette!") == "Va-t-en__Colette_");
    CHECK(sanitize_name("../..") == ".._..");
    CHECK(sanitize_name("..") == "__");
    CHECK(sanitize_name(std::string(100, 'x')).size() == 40);
    CHECK(bundle_dir_name(3, 0, "") == "diag-3-19700101-000000");
    CHECK(bundle_dir_name(12, 1700000000, "blur") == "diag-12-20231114-221320-blur");
}

static void test_appendf() {
    std::string s = "x";
    appendf(s, "%d-%s", 7, "ab");
    CHECK(s == "x7-ab");
    const std::string big(2000, 'q');
    appendf(s, "<%s>", big.c_str());
    CHECK(s.size() == 2007 && s.back() == '>');
}

// A BP array with the registers the feature scan reads.
struct Regs {
    uint32_t bp[256] = {};
    uint32_t xf[0x1100] = {};
};

static void test_features() {
    Bundle b;
    Regs r;
    r.bp[0x00] = 0;  // one stage
    note_draw_features(b, r.bp, r.xf);
    CHECK(b.features.empty());

    // Indirect texturing: GENMODE declares indirect stages and stage 0 has a command.
    r.bp[0x00] = (1u << 16);
    r.bp[0x10] = 0x000123;
    note_draw_features(b, r.bp, r.xf);
    CHECK(b.features.count("indirect texturing") && !b.features["indirect texturing"].implemented);

    // Fog with the range adjustment enabled: the fog is ours, the range adjustment is not.
    r.bp[0xF1] = 2u << 21;  // linear
    r.bp[0xE8] = 1u << 10;
    note_draw_features(b, r.bp, r.xf);
    CHECK(b.features["fog"].implemented);
    CHECK(!b.features["fog range adjust"].implemented);

    // TEV compare mode in stage 0 (bias field 3).
    r.bp[0xC0] = 3u << 16;
    note_draw_features(b, r.bp, r.xf);
    CHECK(b.features["TEV compare mode"].implemented && b.features["TEV compare mode"].count == 1);
}

static void test_copy_features() {
    // GXSetCopyFilter's two patterns: vf off writes 0 0 21 22 21 0 0 (taps in 6-bit fields).
    const uint32_t id53 = (21u << 12) | (22u << 18), id54 = 21u;
    const uint32_t df53 = 8u | (8u << 6) | (10u << 12) | (12u << 18), df54 = 10u | (8u << 6) | (8u << 12);
    Bundle b;
    note_copy_features(b, 0x4000, id53, id54, false);
    CHECK(b.features.empty());  // identity filter, no gamma: nothing special
    note_copy_features(b, 0x4000, df53, df54, false);
    CHECK(b.features.count("EFB copy vertical filter") && b.features["EFB copy vertical filter"].implemented);
    Bundle unprogrammed;
    note_copy_features(unprogrammed, 0x4000, 0, 0, false);  // taps never written: not a filtered copy
    CHECK(unprogrammed.features.empty());
    note_copy_features(b, 0x4000 | (1u << 7), id53, id54, false);
    CHECK(b.features.count("EFB copy gamma") && !b.features["EFB copy gamma"].implemented);
    note_copy_features(b, 0x4000 | (1u << 9) | (1u << 10), id53, id54, false);
    CHECK(b.features.count("EFB copy half scale / mip generation") && b.features["EFB copy half scale / mip generation"].implemented);
    CHECK(b.features.count("EFB copy Y scale"));
    // A depth copy is not filtered.
    Bundle d;
    note_copy_features(d, 0x4000, df53, df54, true);
    CHECK(!d.features.count("EFB copy vertical filter") && d.features.count("EFB depth copy"));
}

static void test_write_bundle() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("gcrecomp-diag-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Bundle b;
    b.name = "unit";
    b.frame = 42;
    b.number = 1;
    b.trace = "[DRAW] #1\n";
    b.count("draws", 2);
    b.feature("indirect texturing", false);
    b.window.w = 2;
    b.window.h = 1;
    b.window.rgb = {1, 2, 3, 4, 5, 6};
    Image im;
    im.label = "copy01-dest000000-fmt4";
    im.w = 1;
    im.h = 2;
    im.rgb = {9, 8, 7, 6, 5, 4};
    im.alpha = {200, 100};
    b.efb.push_back(im);
    const std::string dir = (root / bundle_dir_name(1, 0, "unit")).string();
    std::string error;
    CHECK(write_bundle(dir, b, error));
    CHECK(error.empty());
    CHECK(slurp(fs::path(dir) / "diag.txt") == "[DRAW] #1\n");
    const std::string summary = slurp(fs::path(dir) / "summary.txt");
    CHECK(summary.find("frame 42") != std::string::npos);
    CHECK(summary.find("draws") != std::string::npos && summary.find("NOT IMPLEMENTED") != std::string::npos);
    CHECK(summary.find("efb-copy01-dest000000-fmt4.ppm 1x2") != std::string::npos);
    CHECK(slurp(fs::path(dir) / "window.ppm") == std::string("P6\n2 1\n255\n") + std::string("\x01\x02\x03\x04\x05\x06", 6));
    CHECK(slurp(fs::path(dir) / "efb-copy01-dest000000-fmt4.ppm") == std::string("P6\n1 2\n255\n") + std::string("\x09\x08\x07\x06\x05\x04", 6));
    CHECK(slurp(fs::path(dir) / "efb-copy01-dest000000-fmt4.alpha.pgm") == std::string("P5\n1 2\n255\n") + std::string("\xC8\x64", 2));
    // A directory that cannot be created is reported, not ignored.
    const std::string blocked = (fs::path(dir) / "diag.txt" / "sub").string();
    CHECK(!write_bundle(blocked, b, error) && !error.empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

static void test_output_dir() {
    unset_env("GCRECOMP_DIAG_DIR");
    set_default_dir(nullptr);
    CHECK(output_dir() == ".");
    set_default_dir("shots");
    CHECK(output_dir() == "shots");
    set_env("GCRECOMP_DIAG_DIR", "diag-here");
    CHECK(output_dir() == "diag-here");
    unset_env("GCRECOMP_DIAG_DIR");
    set_default_dir(nullptr);
}

int main() {
    test_trigger();
    test_frame_sequence();
    test_names();
    test_appendf();
    test_features();
    test_copy_features();
    test_write_bundle();
    test_output_dir();
    if (g_failures) std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    else std::puts("diag tests passed");
    return g_failures ? 1 : 0;
}
