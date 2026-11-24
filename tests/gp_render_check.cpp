// SPDX-License-Identifier: MIT
// Renders a few full-screen triangles through the real OpenGL path and checks the EFB pixels. Needs a GL
// 3.3 context but no window system: run it as
//   SDL_VIDEO_DRIVER=offscreen ./gp_render_check
// (it is not part of ctest because CI machines may have no EGL). Covers the blend-state features that
// are only visible on screen: logic ops, constant alpha, fog, and the vertical copy filter of a presented frame (the frames
// are written to a temporary directory and read back from there: the window is swapped by the copy).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gl_loader.h"
#include "gp_internal.h"

using namespace gcrecomp::gp;

static State s;
static float verts[3 * kVertexFloats];
static int g_failures = 0;

static void set_xf_float(uint32_t addr, float f) { std::memcpy(&s.xf[addr], &f, 4); }

static void set_color(float r, float g, float b, float a) {
    for (int i = 0; i < 3; ++i) {
        float* c = &verts[i * kVertexFloats + 4];
        c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    }
}

static void set_depth(float clip_z) {
    for (int i = 0; i < 3; ++i) verts[i * kVertexFloats + 2] = clip_z;
}

static void clear() {
    s.bp[0x4F] = 0; s.bp[0x50] = 0; s.bp[0x51] = 0xFFFFFF;
    CopyRequest r{};
    r.clear = true;
    r.to_xfb = true;  // a copy to main memory needs the guest RAM, which this check does not have
    r.w = 640;
    r.h = 480;
    renderer::copy_efb(r, s);
}

static void draw() {
    const DrawCall call{kTriangles, 3, verts};
    renderer::draw(call, s);
}

static void expect(const char* what, int r, int g, int b, int a) {
    unsigned char p[4];
    glReadPixels(320, 264, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    const bool ok = std::abs(p[0] - r) <= 1 && std::abs(p[1] - g) <= 1 && std::abs(p[2] - b) <= 1 && std::abs(p[3] - a) <= 1 &&
                    glGetError() == GL_NO_ERROR;
    std::printf("%s %-34s got %3d %3d %3d %3d, expected %3d %3d %3d %3d\n", ok ? "ok  " : "FAIL", what, p[0], p[1], p[2], p[3], r, g, b, a);
    if (!ok) ++g_failures;
}

// The window as the renderer presented it last: the newest frame_NNNN.ppm of `dir`. Returns the pixel (x, y), y from the top.
static bool presented_pixel(const std::filesystem::path& dir, int x, int y, int rgb[3]) {
    std::filesystem::path newest;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.path().extension() == ".ppm" && e.path() > newest) newest = e.path();
    std::ifstream f(newest, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, maxv = 0;
    f >> magic >> w >> h >> maxv;
    f.get();
    if (!f || magic != "P6" || x >= w || y >= h) return false;
    std::vector<unsigned char> px(size_t(w) * h * 3);
    f.read(reinterpret_cast<char*>(px.data()), std::streamsize(px.size()));
    for (int c = 0; c < 3; ++c) rgb[c] = px[(size_t(y) * w + x) * 3 + c];
    return bool(f);
}

static void expect_presented(const std::filesystem::path& dir, const char* what, int y, int r) {
    int p[3] = {};
    const bool ok = presented_pixel(dir, 320, y, p) && std::abs(p[0] - r) <= 2 && std::abs(p[1] - r) <= 2 && std::abs(p[2] - r) <= 2;
    std::printf("%s %-34s got %3d %3d %3d, expected %3d\n", ok ? "ok  " : "FAIL", what, p[0], p[1], p[2], r);
    if (!ok) ++g_failures;
}

int main() {
    const std::filesystem::path shots = std::filesystem::temp_directory_path() / "gp_render_check_frames";
    std::filesystem::remove_all(shots);
    std::filesystem::create_directories(shots);
    const std::string shots_dir = shots.string();  // the renderer keeps the pointer
    if (!renderer::init(640, 528, shots_dir.c_str(), 0)) {
        std::fprintf(stderr, "cannot create a GL context (try SDL_VIDEO_DRIVER=offscreen)\n");
        return 2;
    }
    const float pos[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};  // covers the whole viewport, depth 0.5
    for (int i = 0; i < 3; ++i) std::memcpy(&verts[i * kVertexFloats], pos[i], sizeof(pos[i]));
    set_xf_float(0x101A, 320); set_xf_float(0x101B, -240); set_xf_float(0x101D, 320); set_xf_float(0x101E, 288);
    s.bp[0x20] = (342 << 12) | 342;
    s.bp[0x21] = ((342 + 639) << 12) | (342 + 527);
    s.bp[0xC0] = 10;       // TEV color: d = rasterized color
    s.bp[0xC1] = 5 << 4;   // TEV alpha: d = rasterized alpha
    s.bp[0xF6] = 4; s.bp[0xF7] = 14; s.bp[0xF8] = 4; s.bp[0xF9] = 14;  // identity swap tables
    s.bp[0xF3] = (7 << 16) | (7 << 19);                                  // alpha test: always
    const uint32_t kUpdate = 8 | 0x10;                                   // color and alpha update
    const uint32_t kCopy = 3u << 12;

    s.bp[0x41] = kUpdate | kCopy;
    set_color(0.2f, 0.4f, 0.6f, 1.0f);
    draw();
    expect("plain write", 51, 102, 153, 255);

    s.bp[0x41] = kUpdate | 2 | (6u << 12);  // logic op XOR with white inverts every channel
    set_color(1, 1, 1, 1);
    draw();
    expect("logic op xor", 204, 153, 102, 0);

    s.bp[0x41] = kUpdate | kCopy;
    set_color(0, 0, 0, 1);
    draw();
    s.bp[0x41] = kUpdate | 1 | (5u << 5) | (4u << 8);  // blend: src alpha, inverse src alpha
    set_color(1, 0, 0, 0.5f);
    draw();
    expect("blend, TEV alpha .5", 128, 0, 0, 191);

    s.bp[0x42] = 0x100 | 0x40;  // constant alpha 0x40: written alpha changes, blend factors do not
    draw();
    expect("blend, constant alpha 0x40", 191, 0, 0, 64);

    s.bp[0x41] = kUpdate | kCopy;
    s.bp[0x42] = 0x100 | 0x20;
    set_color(0.3f, 0.3f, 0.3f, 0.9f);
    draw();
    expect("no blend, constant alpha 0x20", 77, 77, 77, 32);

    s.bp[0x42] = 0;
    s.bp[0xEE] = 0x3F800;                 // A = 1.0
    s.bp[0xF1] = (2u << 21) | (1u << 20); // linear, orthographic, C = 0
    s.bp[0xF2] = 0x00FF00;                // green
    set_color(1, 0, 0, 1);
    draw();
    expect("orthographic linear fog at z=.5", 128, 128, 0, 255);

    // TEV compare mode (bias field 3): out = d + (a OP b ? c : 0). a = rasterized color, b = konst 0.5 (kcsel 4), c = one.
    s.bp[0x41] = kUpdate | kCopy;
    s.bp[0x40] = 0;                               // no depth test
    s.bp[0xF1] = 0;                               // fog off
    s.bp[0xF3] = (7 << 16) | (7 << 19);           // alpha test: always
    const uint32_t ksel0 = s.bp[0xF6];
    s.bp[0xF6] = ksel0 | (4u << 4) | (4u << 9);   // stage 0: konst color 4/8, konst alpha 4/8
    const uint32_t kCompare = 3u << 16;
    const uint32_t cmp_abcd = (10u << 12) | (14u << 8) | (12u << 4) | 15u;  // a RASC, b KONST, c ONE, d ZERO
    clear();
    s.bp[0xC0] = kCompare | cmp_abcd;             // R8, greater than
    set_color(0.8f, 0.2f, 0.2f, 1);
    draw();
    expect("compare R8 >, true", 255, 255, 255, 255);
    set_color(0.3f, 0.9f, 0.9f, 1);
    draw();
    expect("compare R8 >, false (only red counts)", 0, 0, 0, 255);
    s.bp[0xC0] = kCompare | (3u << 20) | cmp_abcd;  // RGB8, greater than
    set_color(0.8f, 0.2f, 0.9f, 1);
    draw();
    expect("compare RGB8 >, per component", 255, 0, 255, 255);
    s.bp[0xC0] = kCompare | (1u << 20) | cmp_abcd;  // GR16, greater than
    set_color(0.2f, 0.9f, 0.1f, 1);
    draw();
    expect("compare GR16 >, green byte decides", 255, 255, 255, 255);
    s.bp[0xC0] = kCompare | (1u << 18) | cmp_abcd;  // R8, equal
    set_color(0.5f, 0.1f, 0.1f, 1);
    draw();
    expect("compare R8 ==, true", 255, 255, 255, 255);
    set_color(0.6f, 0.1f, 0.1f, 1);
    draw();
    expect("compare R8 ==, false", 0, 0, 0, 255);
    // The stage after a compare sees the 0/1 mask: d = rasterized color, minus half the mask (a = ZERO, b = HALF, c = PREV,
    // subtract bit): the shape the world map uses for its shading. Two stages: mask from 3 = bias, then the subtract.
    s.bp[0] = (s.bp[0] & ~(0xFu << 10)) | (1u << 10);  // two TEV stages
    s.bp[0xC0] = kCompare | cmp_abcd;                  // stage 0: mask = (RASC.r > 0.5)
    s.bp[0xC1] = 7u << 4;                              // stage 0 alpha: d = ZERO (the mask has no alpha here)
    s.bp[0xC2] = (1u << 18) | (1u << 19) | (15u << 12) | (13u << 8) | (0u << 4) | 10u;  // stage 1: RASC - HALF * PREV, clamped
    s.bp[0xC3] = 5u << 4;                              // stage 1 alpha: d = RASA
    set_color(0.8f, 0.8f, 0.8f, 1);
    draw();
    expect("mask then RASC - 0.5 * mask", 77, 77, 77, 255);
    set_color(0.4f, 0.4f, 0.4f, 1);
    draw();
    expect("no mask: RASC unchanged", 102, 102, 102, 255);
    s.bp[0] &= ~(0xFu << 10);
    s.bp[0xC0] = 10;
    s.bp[0xC1] = 5 << 4;
    // alpha compare: a = RASA, b = KONST alpha 0.5, c = KONST alpha 0.5, d = ZERO
    s.bp[0xC0] = 15;  // color: zero
    s.bp[0xC1] = (3u << 16) | (5u << 13) | (6u << 10) | (6u << 7) | (7u << 4);
    set_color(0, 0, 0, 0.8f);
    draw();
    expect("alpha compare >, true", 0, 0, 0, 128);
    set_color(0, 0, 0, 0.2f);
    draw();
    expect("alpha compare >, false", 0, 0, 0, 0);
    s.bp[0xC0] = 10;
    s.bp[0xC1] = 5 << 4;
    s.bp[0xF6] = ksel0;

    // Z compare location: an alpha-rejected pixel updates depth only with early Z (BP 0x43 bit 6).
    // A is rejected by the alpha test at depth .5; B, green and behind A at depth .75, must then fail the
    // depth test with early Z and pass it with late Z.
    s.bp[0x41] = kUpdate | kCopy;
    s.bp[0x40] = 1 | (1 << 1) | 0x10;           // depth test LESS, depth update on
    s.bp[0xF3] = (4 << 16) | (7 << 19) | 128;   // alpha > 128
    for (int early = 1; early >= 0; --early) {
        clear();
        s.bp[0x43] = early ? 0x40 : 0;
        set_depth(0.0f);
        set_color(1, 1, 1, 0.0f);
        draw();
        set_depth(0.5f);
        set_color(0, 1, 0, 1.0f);
        draw();
        if (early) expect("early Z: behind a rejected pixel", 0, 0, 0, 0);
        else expect("late Z: behind a rejected pixel", 0, 255, 0, 255);
    }

    // Vertical copy filter on a presented frame. The upper half of the viewport (EFB lines 48..287) is white over black: the
    // deflicker taps (8 8 10 12 10 8 8 = 16/32/16 of 64) soften the line on each side of an edge and leave the rest alone;
    // taps that were never written, or the identity pattern (0 0 21 22 21 0 0), change nothing.
    s.bp[0x41] = kUpdate | kCopy;
    s.bp[0x40] = 0;
    s.bp[0x43] = 0;
    s.bp[0xF3] = (7 << 16) | (7 << 19);
    auto present = [&](uint32_t bp53, uint32_t bp54, uint32_t cmd) {
        clear();
        const float upper[3][4] = {{-1, 0, 0, 1}, {3, 0, 0, 1}, {-1, 4, 0, 1}};
        for (int i = 0; i < 3; ++i) std::memcpy(&verts[i * kVertexFloats], upper[i], sizeof(upper[i]));
        set_color(1, 1, 1, 1);
        draw();
        for (int i = 0; i < 3; ++i) std::memcpy(&verts[i * kVertexFloats], pos[i], sizeof(pos[i]));
        s.bp[0x53] = bp53;
        s.bp[0x54] = bp54;
        s.bp[0x52] = cmd;
        CopyRequest r{};
        r.to_xfb = true;
        r.w = 640;
        r.h = 528;
        renderer::copy_efb(r, s);
    };
    const uint32_t id53 = (21u << 12) | (22u << 18), id54 = 21u;
    const uint32_t df53 = 8u | (8u << 6) | (10u << 12) | (12u << 18), df54 = 10u | (8u << 6) | (8u << 12);
    present(df53, df54, 3);  // clamp top and bottom, as the game does
    expect_presented(shots, "deflicker: white line at the edge", 287, 191);
    expect_presented(shots, "deflicker: black line at the edge", 288, 64);
    expect_presented(shots, "deflicker: inside the white", 200, 255);
    expect_presented(shots, "deflicker: inside the black", 400, 0);
    expect_presented(shots, "deflicker: black above the white", 47, 64);
    expect_presented(shots, "deflicker: white below the top edge", 48, 191);
    // Asymmetric weights tell the line above (smaller EFB y, the top of the screen) from the line below: 48/16/0 of 64 shows
    // the white above a black line as 3/4 white and nothing of the white below a white line.
    present(24u | (24u << 6) | (5u << 12) | (6u << 18), 5u, 3);
    expect_presented(shots, "above-weighted: black under white", 288, 191);
    expect_presented(shots, "above-weighted: white above black", 287, 255);
    expect_presented(shots, "above-weighted: black further down", 289, 0);
    present(id53, id54, 3);
    expect_presented(shots, "identity taps: edge stays sharp", 287, 255);
    expect_presented(shots, "identity taps: edge stays sharp", 288, 0);
    present(0, 0, 3);
    expect_presented(shots, "taps never written: sharp", 287, 255);
    present(32u << 12, 0, 3);  // a single tap of 32/64 on the line itself: the whole copy fades to half
    expect_presented(shots, "fade taps: white halves", 200, 128);
    expect_presented(shots, "fade taps: black stays", 400, 0);

    renderer::shutdown();
    std::error_code ec;
    if (!std::getenv("GP_RENDER_CHECK_KEEP")) std::filesystem::remove_all(shots, ec);
    return g_failures ? 1 : 0;
}
