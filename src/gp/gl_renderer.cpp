// SPDX-License-Identifier: MIT
//
// OpenGL 3.3 renderer behind the GP command stream, using SDL3 for the window and context.
//
// One fragment shader implements the TEV (texture environment) pipeline with the stage
// configuration passed as uniforms. The EFB is a 640x528 framebuffer object; a copy to the
// external frame buffer blits it to the window.
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gcrecomp/bench.h"
#include "gcrecomp/gx/gx.h"
#include "gcrecomp/runtime.h"
#include "efb_copy.h"
#include "efb_gpu_copy.h"
#include "gl_loader.h"
#include "gp_decode.h"
#include "gp_internal.h"
#include "input.h"
#include "diag.h"
#include "gcrecomp/audio/audio_sink.h"
#include "gcrecomp/gp/bindings.h"
#include "overlay_glue.h"
#include "gcrecomp/settings.h"

namespace gcrecomp::gx {
// Defined in src/gx/gx_texture.cpp.
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt);
}  // namespace gcrecomp::gx

namespace gcrecomp::gp::renderer {

namespace {

constexpr int kEfbWidth = 640;
constexpr int kEfbHeight = 528;

SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
int g_window_w = 0, g_window_h = 0;
bool g_closed = false;
settings::Store* g_store = nullptr;
settings::Values g_applied;                 // the settings last applied to the window and the sink
bool g_applied_valid = false;
audio::AudioSink* g_audio_target = nullptr;  // the sink the audio settings were last applied to
settings::Aspect g_aspect = settings::Aspect::Stretch;  // without settings (tests, tools) the picture fills the window, as it always did
bool g_overlay_was_visible = false;
std::chrono::steady_clock::time_point g_last_reload = std::chrono::steady_clock::now();
double g_fps = 0.0;  // presented frames per second, measured over half a second
int g_fps_frames = 0;
std::chrono::steady_clock::time_point g_fps_since = std::chrono::steady_clock::now();

// Where the picture goes in the window: the largest box of the chosen shape that fits, centred, or all of the window.
struct PresentRect {
    int x, y, w, h;
};
PresentRect g_present_rect = {0, 0, 0, 0};

PresentRect present_rect_for(settings::Aspect aspect, int window_w, int window_h) {
    double shape = 0.0;  // width / height of the box; 0 = the whole window
    switch (aspect) {
        case settings::Aspect::Fit: shape = 4.0 / 3.0; break;
        case settings::Aspect::Wide16x9: shape = 16.0 / 9.0; break;
        case settings::Aspect::Wide21x9: shape = 21.0 / 9.0; break;
        case settings::Aspect::Stretch: break;
    }
    if (shape == 0.0 || window_w <= 0 || window_h <= 0) return {0, 0, window_w, window_h};
    int w = window_w, h = int(std::lround(window_w / shape));
    if (h > window_h) {
        h = window_h;
        w = int(std::lround(window_h * shape));
    }
    return {(window_w - w) / 2, (window_h - h) / 2, w, h};
}
const char* g_shot_dir = nullptr;
int g_shot_max = 0;
int g_shot_count = 0;

GLuint g_fbo = 0, g_color = 0, g_depth = 0;
GLuint g_vao = 0, g_vbo = 0, g_program = 0;
// The same shader with early fragment tests (GL_ARB_shader_image_load_store): the depth test AND the depth write happen before the shader, so a
// fragment it discards has written its depth: GX's "Z compare before texturing" with an alpha test, in one pass (0: not available, the draw is
// replayed depth-only instead, see flush_batch). g_active is the program in use.
GLuint g_program_early = 0, g_active = 0;
GLuint g_white = 0;
GLuint g_present_program = 0, g_present_vao = 0;  // the pass that presents an EFB copy through the vertical copy filter
struct PresentLocations {
    GLint rect = -1, misc = -1, weights = -1, view = -1;
} g_present_loc;
constexpr int kEfbUnit = 7;  // texture unit the EFB's colour texture is bound to while that pass samples it
bool g_ring_valid = false;  // the vertex buffer holds the ring (an oversized draw replaced it)
bool g_efb_bound = false;  // GL_FRAMEBUFFER is the EFB framebuffer (draw() binds it lazily; EFB copies unbind it)
uint64_t g_indirect_draws = 0;
// How often the game used the features that are only visible on screen, reported at shutdown.
struct FeatureUse {
    uint64_t c14x2_textures = 0, mip_textures = 0, fog_draws = 0, const_alpha_draws = 0, logic_op_draws = 0, early_z_passes = 0, tev_compare_draws = 0, filtered_copies = 0;
} g_use;

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_c0;
layout(location = 2) in vec4 a_c1;
layout(location = 3) in vec3 a_uv0;
layout(location = 4) in vec3 a_uv1;
layout(location = 5) in vec3 a_uv2;
layout(location = 6) in vec3 a_uv3;
layout(location = 7) in vec3 a_uv4;
layout(location = 8) in vec3 a_uv5;
layout(location = 9) in vec3 a_uv6;
layout(location = 10) in vec3 a_uv7;
out vec4 v_c0;
out vec4 v_c1;
out vec3 v_uv[8];
void main() {
    gl_Position = a_pos;
    v_c0 = a_c0;
    v_c1 = a_c1;
    v_uv[0] = a_uv0; v_uv[1] = a_uv1; v_uv[2] = a_uv2; v_uv[3] = a_uv3;
    v_uv[4] = a_uv4; v_uv[5] = a_uv5; v_uv[6] = a_uv6; v_uv[7] = a_uv7;
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec4 v_c0;
in vec4 v_c1;
in vec3 v_uv[8];
layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_src1;   // TEV alpha, for blend factors when a constant alpha replaces o_color.a

uniform sampler2D u_tex0; uniform sampler2D u_tex1; uniform sampler2D u_tex2; uniform sampler2D u_tex3;
uniform sampler2D u_tex4; uniform sampler2D u_tex5; uniform sampler2D u_tex6; uniform sampler2D u_tex7;
uniform int u_nstages;
uniform uvec4 u_stage[16];   // color env, alpha env, tref (map | coord << 3 | enable << 6 | chan << 7), ksel
uniform ivec4 u_swap[4];     // component selectors r, g, b, a of each swap table
uniform vec4 u_reg[4];       // PREV, C0, C1, C2
uniform vec4 u_konst[4];
uniform vec4 u_fog_f;        // x: A, y: C
uniform ivec4 u_fog_i;       // x: B magnitude, y: B shift, z: orthographic, w: type (0 off, 2 linear, 4-7 exp)
uniform vec3 u_fog_color;
uniform float u_const_alpha; // alpha written to the EFB when >= 0 (GXSetDstAlpha), else the TEV alpha
uniform uvec2 u_alpha_cmp;   // x: comparison codes (c0 | c1 << 3 | logic << 6), y: refs (ref0 | ref1 << 8)

vec4 sample_map(int map, vec2 uv) {
    if (map == 0) return texture(u_tex0, uv);
    if (map == 1) return texture(u_tex1, uv);
    if (map == 2) return texture(u_tex2, uv);
    if (map == 3) return texture(u_tex3, uv);
    if (map == 4) return texture(u_tex4, uv);
    if (map == 5) return texture(u_tex5, uv);
    if (map == 6) return texture(u_tex6, uv);
    return texture(u_tex7, uv);
}

vec4 swizzle(vec4 v, int table) {
    ivec4 s = u_swap[table];
    float c[4] = float[4](v.r, v.g, v.b, v.a);
    return vec4(c[s.x], c[s.y], c[s.z], c[s.w]);
}


float frac8(uint sel) {
    float f[8] = float[8](1.0, 0.875, 0.75, 0.625, 0.5, 0.375, 0.25, 0.125);
    return f[sel & 7u];
}

vec3 kcolor_sel(uint sel) {
    if (sel < 8u) return vec3(frac8(sel));
    if (sel >= 12u && sel < 16u) return u_konst[sel - 12u].rgb;
    if (sel >= 16u && sel < 20u) return vec3(u_konst[sel - 16u].r);
    if (sel >= 20u && sel < 24u) return vec3(u_konst[sel - 20u].g);
    if (sel >= 24u && sel < 28u) return vec3(u_konst[sel - 24u].b);
    if (sel >= 28u && sel < 32u) return vec3(u_konst[sel - 28u].a);
    return vec3(1.0);
}

float kalpha_sel(uint sel) {
    if (sel < 8u) return frac8(sel);
    if (sel >= 16u && sel < 20u) return u_konst[sel - 16u].r;
    if (sel >= 20u && sel < 24u) return u_konst[sel - 20u].g;
    if (sel >= 24u && sel < 28u) return u_konst[sel - 24u].b;
    if (sel >= 28u && sel < 32u) return u_konst[sel - 28u].a;
    return 1.0;
}

bool compare(uint func, float a, float ref) {
    if (func == 0u) return false;
    if (func == 1u) return a < ref;
    if (func == 2u) return a == ref;
    if (func == 3u) return a <= ref;
    if (func == 4u) return a > ref;
    if (func == 5u) return a != ref;
    if (func == 6u) return a >= ref;
    return true;
}

// Mirrors fog_factor in gp_decode.cpp.
float fog_amount() {
    int type = u_fog_i.w;
    if (type == 0) return 0.0;
    int z24 = int(gl_FragCoord.z * 16777215.0 + 0.5);
    float ze;
    if (u_fog_i.z != 0) {
        ze = u_fog_f.x * float(z24) / 16777216.0;
    } else {
        int denom = u_fog_i.x - (z24 >> u_fog_i.y);
        ze = denom != 0 ? u_fog_f.x * 16777216.0 / float(denom) : 1e30;
    }
    float f = clamp(ze - u_fog_f.y, 0.0, 1.0);
    if (type == 4) return 1.0 - exp2(-8.0 * f);
    if (type == 5) return 1.0 - exp2(-8.0 * f * f);
    if (type == 6) return exp2(-8.0 * (1.0 - f));
    if (type == 7) return exp2(-8.0 * (1.0 - f) * (1.0 - f));
    return f;
}

// TEV compare mode (bias field 3): out = d + (a OP b ? c : 0) instead of the linear blend. OP is > (sub bit 0) or ==
// (sub bit 1); the scale field picks what is compared: 0 the red byte (R8), 1 green:red as 16 bits (GR16), 2 blue:green:red
// as 24 bits (BGR24), 3 each component alone (RGB8). Bytes are the inputs rounded to 8 bits.
vec3 tev_compare_color(vec3 a, vec3 b, vec3 c, vec3 d, uint mode, bool eq) {
    ivec3 ia = ivec3(floor(clamp(a, 0.0, 1.0) * 255.0 + 0.5));
    ivec3 ib = ivec3(floor(clamp(b, 0.0, 1.0) * 255.0 + 0.5));
    bvec3 hit;
    if (mode == 3u) {
        hit = eq ? equal(ia, ib) : greaterThan(ia, ib);
    } else {
        int va, vb;
        if (mode == 0u) { va = ia.r; vb = ib.r; }
        else if (mode == 1u) { va = ia.g * 256 + ia.r; vb = ib.g * 256 + ib.r; }
        else { va = (ia.b * 256 + ia.g) * 256 + ia.r; vb = (ib.b * 256 + ib.g) * 256 + ib.r; }
        hit = bvec3(eq ? va == vb : va > vb);
    }
    return d + vec3(hit.x ? c.x : 0.0, hit.y ? c.y : 0.0, hit.z ? c.z : 0.0);
}

// The alpha environment has one value per input: every mode compares the alpha bytes (the multi-byte modes of the
// colour environment have no extra channels to read).
float tev_compare_alpha(float a, float b, float c, float d, bool eq) {
    int ia = int(floor(clamp(a, 0.0, 1.0) * 255.0 + 0.5)), ib = int(floor(clamp(b, 0.0, 1.0) * 255.0 + 0.5));
    return d + ((eq ? ia == ib : ia > ib) ? c : 0.0);
}

void main() {
    vec4 reg[4] = vec4[4](u_reg[0], u_reg[1], u_reg[2], u_reg[3]);
    for (int i = 0; i < u_nstages; ++i) {
        uint ce = u_stage[i].x, ae = u_stage[i].y, tr = u_stage[i].z, ks = u_stage[i].w;
        int texmap = int(tr & 7u);
        int texcoord = int((tr >> 3) & 7u);
        bool texen = ((tr >> 6) & 1u) != 0u;
        uint chan = (tr >> 7) & 7u;

        vec4 tex = vec4(1.0);
        if (texen) {
            vec3 uv = v_uv[texcoord];
            vec2 st = uv.z != 0.0 ? uv.xy / uv.z : uv.xy;
            tex = swizzle(sample_map(texmap, st), int((ae >> 2) & 3u));
        }
        vec4 ras = vec4(0.0);
        if (chan == 0u) ras = v_c0;
        else if (chan == 1u) ras = v_c1;
        ras = swizzle(ras, int(ae & 3u));

        vec3 kc = kcolor_sel(ks & 31u);
        float ka = kalpha_sel((ks >> 5) & 31u);

        vec3 cin[16] = vec3[16](
            reg[0].rgb, vec3(reg[0].a), reg[1].rgb, vec3(reg[1].a),
            reg[2].rgb, vec3(reg[2].a), reg[3].rgb, vec3(reg[3].a),
            tex.rgb, vec3(tex.a), ras.rgb, vec3(ras.a),
            vec3(1.0), vec3(0.5), kc, vec3(0.0));
        vec3 ca = cin[(ce >> 12) & 15u], cb = cin[(ce >> 8) & 15u], cc = cin[(ce >> 4) & 15u], cd = cin[ce & 15u];
        float bias_c[4] = float[4](0.0, 0.5, -0.5, 0.0);
        float scale[4] = float[4](1.0, 2.0, 4.0, 0.5);
        float sign_c = ((ce >> 18) & 1u) != 0u ? -1.0 : 1.0;
        vec3 color = ((ce >> 16) & 3u) == 3u
            ? tev_compare_color(ca, cb, cc, cd, (ce >> 20) & 3u, ((ce >> 18) & 1u) != 0u)
            : (cd + sign_c * mix(ca, cb, cc) + bias_c[(ce >> 16) & 3u]) * scale[(ce >> 20) & 3u];
        if (((ce >> 19) & 1u) != 0u) color = clamp(color, 0.0, 1.0);

        float ain[8] = float[8](reg[0].a, reg[1].a, reg[2].a, reg[3].a, tex.a, ras.a, ka, 0.0);
        float aa = ain[(ae >> 13) & 7u], ab = ain[(ae >> 10) & 7u], ac = ain[(ae >> 7) & 7u], ad = ain[(ae >> 4) & 7u];
        float sign_a = ((ae >> 18) & 1u) != 0u ? -1.0 : 1.0;
        float alpha = ((ae >> 16) & 3u) == 3u
            ? tev_compare_alpha(aa, ab, ac, ad, ((ae >> 18) & 1u) != 0u)
            : (ad + sign_a * mix(aa, ab, ac) + bias_c[(ae >> 16) & 3u]) * scale[(ae >> 20) & 3u];
        if (((ae >> 19) & 1u) != 0u) alpha = clamp(alpha, 0.0, 1.0);

        uint cdest = (ce >> 22) & 3u, adest = (ae >> 22) & 3u;
        reg[cdest].rgb = color;
        reg[adest].a = alpha;
    }
    vec4 result = vec4(reg[0].rgb, reg[0].a);
    result.rgb = mix(result.rgb, u_fog_color, fog_amount());

    uint c0 = u_alpha_cmp.x & 7u, c1 = (u_alpha_cmp.x >> 3) & 7u, logic = (u_alpha_cmp.x >> 6) & 3u;
    float r0 = float(u_alpha_cmp.y & 255u) / 255.0, r1 = float((u_alpha_cmp.y >> 8) & 255u) / 255.0;
    bool p0 = compare(c0, result.a, r0), p1 = compare(c1, result.a, r1);
    bool pass = logic == 0u ? (p0 && p1) : logic == 1u ? (p0 || p1) : logic == 2u ? (p0 != p1) : (p0 == p1);
    if (!pass) discard;
    o_src1 = vec4(vec3(result.a), result.a);
    if (u_const_alpha >= 0.0) result.a = u_const_alpha;
    o_color = result;
}
)";

// Presents an EFB copy through the vertical copy filter (efb_copy.h): one full-screen triangle that samples the EFB's colour
// texture three times, one EFB line apart, and weighs the samples. The bilinear stretch to the window is the plain blit's.
const char* kPresentVertexShader = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kPresentFragmentShader = R"(#version 330 core
uniform sampler2D u_efb;
uniform vec4 u_rect;     // the copied rectangle in EFB texels: x, y of its lowest line (GL rows), width, height
uniform vec4 u_misc;     // picture width and height in the window; lines the filter may read above and below the rectangle (0 or 1)
uniform vec4 u_view;     // x, y of the picture's lower-left corner in the window
uniform vec4 u_weights;  // line above, line itself, line below, each over 64
out vec4 o_color;
void main() {
    vec2 p = u_rect.xy + (gl_FragCoord.xy - u_view.xy) / u_misc.xy * u_rect.zw;
    vec2 inv = 1.0 / vec2(textureSize(u_efb, 0));
    float x = clamp(p.x, u_rect.x + 0.5, u_rect.x + u_rect.z - 0.5);
    float lo = u_rect.y + 0.5, hi = u_rect.y + u_rect.w - 0.5;
    // GL rows run bottom to top: the line above on the screen is the next row up.
    vec4 c = u_weights.y * texture(u_efb, vec2(x, clamp(p.y, lo, hi)) * inv);
    c += u_weights.x * texture(u_efb, vec2(x, clamp(p.y + 1.0, lo, hi + u_misc.z)) * inv);
    c += u_weights.z * texture(u_efb, vec2(x, clamp(p.y - 1.0, lo - u_misc.w, hi)) * inv);
    o_color = vec4(c.rgb, 1.0);
}
)";

// The copy to a GPU-resident texture (docs/BENCHMARK.md, "EFB copies on the GPU"): one full-screen triangle that writes the copied
// EFB rectangle as the RGB565 format keeps it (5-6-5 bits, expanded back to 8 the way gx::decode_texture does), top row first.
const char* kCopyFragmentShader = R"(#version 330 core
uniform sampler2D u_efb;
uniform ivec2 u_origin;  // EFB texel (GL rows) of the copy's top-left pixel; rows of the copy go down from there
uniform ivec2 u_size;    // the copied rectangle in texels
uniform ivec2 u_mode;    // x: 1 when the copy is half scaled (2x2 box filter); y: 0 RGB565, 1 RGBA8
out vec4 o_color;
uvec4 texel(int x, int y) {
    return uvec4(round(texelFetch(u_efb, ivec2(u_origin.x + min(x, u_size.x - 1), u_origin.y - min(y, u_size.y - 1)), 0) * 255.0));
}
void main() {
    ivec2 f = ivec2(gl_FragCoord.xy);
    uvec4 c;
    if (u_mode.x != 0) {
        c = (texel(f.x * 2, f.y * 2) + texel(f.x * 2 + 1, f.y * 2) + texel(f.x * 2, f.y * 2 + 1) + texel(f.x * 2 + 1, f.y * 2 + 1)) >> 2;
    } else {
        c = texel(f.x, f.y);
    }
    if (u_mode.y == 0) {
        c = uvec4((c.r >> 3) * 255u / 31u, (c.g >> 2) * 255u / 63u, (c.b >> 3) * 255u / 31u, 255u);
    }
    o_color = vec4(c) / 255.0;
}
)";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[GL] shader compile failed:\n%s\n", log);
        return 0;
    }
    return shader;
}

// Sampler state last applied to a cached texture object: glTexParameter calls are skipped while it is unchanged.
struct SamplerParams {
    GLint wrap_s = 0, wrap_t = 0, mag = 0, min = 0;
    float lod_bias = 0, min_lod = 0, max_lod = 0;
    bool operator==(const SamplerParams& o) const {
        return wrap_s == o.wrap_s && wrap_t == o.wrap_t && mag == o.mag && min == o.min && lod_bias == o.lod_bias &&
               min_lod == o.min_lod && max_lod == o.max_lod;
    }
};

struct CachedTexture {
    GLuint id = 0;
    uint64_t hash = 0;       // what the GL texture was decoded from: data_hash combined with the palette's
    uint64_t data_hash = 0;  // hash of the guest bytes, as of the last time they were read
    uint32_t clock = 0;      // Memory write clock at that time: no store to the texture's pages since means data_hash holds
    uint64_t hash_frame = 0;
    uint32_t tlut_gen = ~0u, settlut = 0;  // the palette (State::tlut_gen, the TLUT offset register) tlut_hash is of
    uint64_t tlut_hash = 0;
    uint64_t decoded_data = 0, decoded_tlut = 0;  // data_hash / tlut_hash of the last decode (GP_TRACE_TEX says which one changed)
    SamplerParams params;
    bool params_valid = false;
};
std::unordered_map<uint64_t, CachedTexture> g_textures;

// EFB copies kept on the GPU. A copy to memory in a simple format is drawn into a GL texture (g_copy_gl[slot]) and read back
// asynchronously (PBO + fence); draws that sample its guest address take the texture, and the guest bytes are written
// when the game synchronises with the GPU (draw-sync token, draw done), before any texture decode that reads them, or
// before a later copy overlaps them. GP_EFB_COPY_SYNC=1 reads every copy back at once, as before.
CopyTracker g_copy_tracker;
struct GpuCopyGl {
    GLuint tex = 0;
    int tw = 0, th = 0;
    SamplerParams params;
    bool params_valid = false;
};
GpuCopyGl g_copy_gl[CopyTracker::kSlots];
struct PendingReadback {
    GLuint pbo;
    GLsync fence;
    EfbCopyParams p;
    uint32_t addr;
    int slot;
    uint64_t frame;  // frames presented when the copy was made
};
std::vector<PendingReadback> g_pending;
std::vector<GLuint> g_pbo_free;
GLuint g_copy_program = 0, g_copy_fbo = 0;
GLint g_copy_origin_loc = -1, g_copy_size_loc = -1, g_copy_mode_loc = -1;
bool g_copy_sync = std::getenv("GP_EFB_COPY_SYNC") != nullptr;
std::thread::id g_gl_thread;  // the thread that made the GL context current: only it may call GL

// The texture bound to each unit and the active unit, so that a draw that uses what the previous one used issues no GL
// call. ~0u / -1: unknown.
GLuint g_bound[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
int g_active_unit = -1;

// Binds `id` to `unit` for sampling; the active unit is left alone when the texture is already bound there.
// Why a batch was drawn (RenderStats::flush_why).
enum FlushWhy { kWhyState, kWhyTexture, kWhyProgram, kWhySize, kWhyOverlap, kWhyDirect, kWhyOther };
void flush_batch(int why = kWhyOther);  // draws the batched triangles (see "Draw batching" below); before any change of GL state they depend on

void bind_unit_texture(int unit, GLuint id) {
    if (g_bound[unit] == id) return;
    flush_batch(kWhyTexture);
    if (g_active_unit != unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        g_active_unit = unit;
    }
    glBindTexture(GL_TEXTURE_2D, id);
    g_bound[unit] = id;
}

// The same, and makes `unit` the active one: for the calls that modify the bound texture (glTexImage2D, glTexParameter).
void edit_unit_texture(int unit, GLuint id) {
    if (g_active_unit != unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        g_active_unit = unit;
    }
    if (g_bound[unit] != id) {
        flush_batch(kWhyTexture);
        glBindTexture(GL_TEXTURE_2D, id);
        g_bound[unit] = id;
    }
}

// Change detection of the guest bytes behind a texture: eight independent multiply-xor lanes (a single FNV chain
// is latency bound, ~1 GB/s; this one runs at memory speed). Only equality over time matters, not the value.
uint64_t hash_bytes(const uint8_t* p, size_t n) {
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h[8] = {1469598103934665603ull, 0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull,
                     0x27D4EB2F165667C5ull, 0xD6E8FEB86659FD93ull, 0xFF51AFD7ED558CCDull, 0xC4CEB9FE1A85EC53ull};
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t w[8];
        std::memcpy(w, p + i, 64);
        for (int k = 0; k < 8; ++k) h[k] = (h[k] ^ w[k]) * kPrime;
    }
    uint64_t r = n;
    for (int k = 0; k < 8; ++k) r = (r ^ (h[k] >> 29) ^ h[k]) * kPrime;
    for (; i < n; ++i) r = (r ^ p[i]) * kPrime;
    return r;
}

// The guest ranges of the pending copies are what a CPU read must not see before the copy is written.
void update_read_guard() {
    uint32_t lo[8], hi[8], n = 0;
    for (const PendingReadback& pr : g_pending) {
        if (n == 8) break;
        lo[n] = pr.addr;
        hi[n] = pr.addr + g_copy_tracker.at(pr.slot).extent;
        ++n;
    }
    g_mem.set_read_guard(lo, hi, n);
}

void flush_overlapping(uint32_t addr, uint32_t len);

// Memory::read_guard_hook: the CPU (or the host on its behalf) reads bytes of a copy the GPU has not delivered yet.
void read_guard_hit(uint32_t off, uint32_t len) {
    if (std::this_thread::get_id() != g_gl_thread) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "[GL] a thread other than the renderer's read an EFB copy that is not written yet (GP_EFB_COPY_SYNC=1 avoids this)\n");
        }
        return;
    }
    ++g_render_stats.efb_guard_hits;
    flush_overlapping(off, len);
}

// Writes the guest bytes of the pending copy at g_pending[i]: waits for the GPU, reads the PBO, encodes. A store by the CPU
// into the range since the copy was made wins on its pages (the copy came first on the console), and the GPU copy is dropped.
void flush_pending_at(size_t i) {
    const PendingReadback pr = g_pending[i];
    g_pending.erase(g_pending.begin() + std::ptrdiff_t(i));
    update_read_guard();  // before the bytes are touched: this copy's own range must not trap
    const size_t w = size_t(pr.p.w), h = size_t(pr.p.h);
    static std::vector<uint8_t> rgba, tmp;
    rgba.resize(w * h * 4);
    {
        bench::ScopedNs timer(g_render_stats.readback_ns);
        for (;;) {
            const GLenum r = glClientWaitSync(pr.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            if (r != GL_TIMEOUT_EXPIRED) break;  // signaled, or an error (nothing better to do): go on
        }
        glDeleteSync(pr.fence);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pr.pbo);
        const uint8_t* mapped = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, GLsizeiptr(rgba.size()), GL_MAP_READ_BIT));
        if (mapped) {
            for (size_t r = 0; r < h; ++r) std::memcpy(&rgba[r * w * 4], mapped + (h - 1 - r) * w * 4, w * 4);  // GL rows run bottom to top
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    g_pbo_free.push_back(pr.pbo);
    GpuCopy& c = g_copy_tracker.at(pr.slot);
    const uint32_t extent = c.extent;
    const bool stale = g_mem.written_since(pr.addr, extent, c.clock);
    uint8_t* dst = const_cast<uint8_t*>(g_mem.read_ptr(pr.addr));  // stamped as a whole below, after the decision
    if (!stale) {
        encode_efb_copy(pr.p, rgba.data(), nullptr, dst);
    } else {
        tmp.assign(extent, 0);
        encode_efb_copy(pr.p, rgba.data(), nullptr, tmp.data());
        for (uint32_t a = pr.addr; a < pr.addr + extent;) {
            const uint32_t end = std::min((a | 0xFFFu) + 1, pr.addr + extent);
            if (!g_mem.written_since(a, end - a, c.clock)) std::memcpy(dst + (a - pr.addr), tmp.data() + (a - pr.addr), end - a);
            a = end;
        }
    }
    // GP_EFB_COPY_VERIFY=1: the GPU copy must equal what the texture decoder makes of the bytes just written (any difference is
    // reported once per kind of copy, with its largest channel error).
    static const bool verify = std::getenv("GP_EFB_COPY_VERIFY") != nullptr;
    if (verify && !stale) {
        int ow, oh;
        efb_copy_output_size(pr.p, &ow, &oh);
        const GpuCopyGl& gl = g_copy_gl[pr.slot];
        const std::vector<uint8_t> want = gx::decode_texture(dst, uint32_t(ow), uint32_t(oh), gx::GXTexFmt(pr.p.format));
        std::vector<uint8_t> got(size_t(ow) * oh * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_copy_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.tex, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        int worst = 0;
        size_t bad = 0;
        for (size_t k = 0; k < got.size(); ++k) {
            const int d = std::abs(int(got[k]) - int(want[k]));
            worst = std::max(worst, d);
            bad += d != 0;
        }
        static uint64_t checked = 0, mismatched = 0;
        ++checked;
        if (bad) ++mismatched;
        if (bad && mismatched <= 5)
            std::fprintf(stderr, "[GL] EFB copy VERIFY: format %u %dx%d half=%d at 0x%06X: %zu of %zu bytes differ, largest %d\n", pr.p.format, ow, oh,
                         pr.p.half_scale, pr.addr, bad, got.size(), worst);
        if (checked % 600 == 0) std::fprintf(stderr, "[GL] EFB copy VERIFY: %llu copies checked, %llu differ\n", (unsigned long long)checked, (unsigned long long)mismatched);
    }
    g_mem.mark_written(pr.addr | Memory::MAIN_RAM_BASE, extent);  // the cached textures made from this memory are stale
    g_copy_tracker.mark_written(pr.slot, g_mem.advance_clock());
    if (stale) g_copy_tracker.invalidate(pr.slot);
    ++g_render_stats.efb_flushes;
}

void flush_slot(int slot) {
    for (size_t i = 0; i < g_pending.size(); ++i)
        if (g_pending[i].slot == slot) {
            flush_pending_at(i);
            return;
        }
}

// Before guest bytes of [addr, addr + len) are read or overwritten by anything but the CPU.
void flush_overlapping(uint32_t addr, uint32_t len) {
    int slots[CopyTracker::kSlots];
    const int n = g_copy_tracker.pending_overlapping(addr, len, slots);
    for (int i = 0; i < n; ++i) flush_slot(slots[i]);
}

void flush_all_copies() {
    while (!g_pending.empty()) flush_pending_at(0);
}

SamplerParams wanted_sampler(uint32_t mode0, uint32_t mode1, uint32_t levels) {
    static const GLint wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    static const GLint min_filters[6] = {GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                         GL_LINEAR,  GL_LINEAR_MIPMAP_NEAREST,  GL_LINEAR_MIPMAP_LINEAR};
    SamplerParams want;
    want.wrap_s = wrap[(mode0 & 3) % 3];
    want.wrap_t = wrap[((mode0 >> 2) & 3) % 3];
    want.mag = (mode0 & 0x10) ? GL_LINEAR : GL_NEAREST;
    want.min = min_filters[uint32_t(effective_min_filter(mode0, levels))];
    want.lod_bias = lod_bias_of(mode0);
    want.min_lod = min_lod_of(mode1);
    want.max_lod = max_lod_of(mode1);
    return want;
}

void apply_sampler(int map, GLuint id, SamplerParams& cache, bool& valid, const SamplerParams& want) {
    if (valid && cache == want) return;
    flush_batch(kWhyTexture);
    edit_unit_texture(map, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, want.wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, want.wrap_t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, want.mag);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, want.min);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, want.lod_bias);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, want.min_lod);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, want.max_lod);
    cache = want;
    valid = true;
}

GLuint texture_for_map(const State& s, int map) {
    const uint32_t img0 = s.bp[map < 4 ? 0x88 + map : 0xA8 + (map - 4)];
    const uint32_t img3 = s.bp[map < 4 ? 0x94 + map : 0xB4 + (map - 4)];
    const uint32_t mode0 = s.bp[map < 4 ? 0x80 + map : 0xA0 + (map - 4)];
    const uint32_t mode1 = s.bp[map < 4 ? 0x84 + map : 0xA4 + (map - 4)];
    const uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
    const uint32_t addr = (img3 & 0xFFFFFF) << 5;
    const bool indexed = fmt == kTexC4 || fmt == kTexC8 || fmt == kTexC14X2;
    const uint32_t settlut = indexed ? s.bp[map < 4 ? 0x98 + map : 0xB8 + (map - 4)] : 0;
    const uint32_t levels = mip_level_count(w, h, mode0, mode1);
    const size_t bytes = mip_chain_bytes(w, h, fmt, levels);
    if (!g_copy_sync && levels == 1 && !indexed) {
        const int slot = g_copy_tracker.find(addr, w, h, uint8_t(fmt));
        if (slot >= 0) {
            GpuCopy& c = g_copy_tracker.at(slot);
            if (g_mem.written_since(addr, c.extent, c.clock)) {
                // The CPU stored into the copy's bytes: guest memory is the truth from now on.
                if (c.pending) flush_slot(slot);
                g_copy_tracker.invalidate(slot);
            } else {
                GpuCopyGl& gl = g_copy_gl[slot];
                ++g_render_stats.efb_gpu_hits;
                bind_unit_texture(map, gl.tex);
                apply_sampler(map, gl.tex, gl.params, gl.params_valid, wanted_sampler(mode0, mode1, levels));
                return gl.tex;
            }
        }
    }
    if (!g_pending.empty()) flush_overlapping(addr, uint32_t(bytes));
    const uint8_t* src = g_mem.read_ptr(addr);
    const uint64_t key = (uint64_t(addr) << 24) ^ (uint64_t(w) << 12) ^ (uint64_t(h) << 4) ^ fmt ^ (uint64_t(levels) << 56);
    CachedTexture& t = g_textures[key];
    // The guest bytes are hashed to notice that the game rewrote the texture. Reading them all on every use dominated a
    // frame (thousands of draws per frame use the same few textures), so the hash is kept while the Memory write stamps
    // (runtime.h) say that no store touched the texture's pages since it was taken. A texture is still re-read every
    // kRevalidateFrames presented frames, a safety net against a writer that does not stamp, and always with
    // GP_TEX_VALIDATE=always.
    static const bool always = [] {
        const char* v = std::getenv("GP_TEX_VALIDATE");
        return v && !std::strcmp(v, "always");
    }();
    constexpr uint64_t kRevalidateFrames = 120;
    if (always || t.id == 0 || t.clock == 0 || g_frames - t.hash_frame >= kRevalidateFrames ||
        g_mem.written_since(addr, uint32_t(bytes), t.clock)) {
        t.data_hash = hash_bytes(src, bytes);
        ++g_render_stats.tex_validations;
        g_render_stats.tex_validated_bytes += bytes;
        t.hash_frame = g_frames;
    }
    t.clock = g_mem.advance_clock();
    uint64_t hash = t.data_hash;
    if (indexed) {
        // Hash the part of the TLUT the format can reach: 16, 256 or 16384 entries of 2 bytes.
        if (t.tlut_gen != s.tlut_gen || t.settlut != settlut) {
            const size_t entries = fmt == kTexC4 ? 16 : fmt == kTexC8 ? 256 : 16384;
            const size_t base = size_t(settlut & 0x3FF) * 512;
            const size_t len = std::min(entries * 2, sizeof(s.tlut) - base);
            t.tlut_hash = hash_bytes(s.tlut + base, len) * 31 + settlut;
            t.tlut_gen = s.tlut_gen;
            t.settlut = settlut;
        }
        hash ^= t.tlut_hash;
    }
    if (t.id == 0 || t.hash != hash) {
        auto decode_level = [&](uint32_t level) {
            const uint32_t lw = mip_dim(w, level), lh = mip_dim(h, level);
            const uint8_t* lsrc = src + mip_level_offset(w, h, fmt, level);
            return indexed ? decode_indexed(s.tlut, sizeof(s.tlut), lsrc, lw, lh, fmt, settlut)
                           : gx::decode_texture(lsrc, lw, lh, gx::GXTexFmt(fmt));
        };
        static const bool trace_tex = std::getenv("GP_TRACE_TEX") != nullptr;
        if (trace_tex)
            std::fprintf(stderr, "[TEX] frame %llu decode map=%d addr=%06X fmt=%X %ux%u levels=%u %s data=%08X palette=%08X\n", (unsigned long long)g_frames, map, addr, fmt, w, h,
                         levels, t.id == 0 ? "new" : t.decoded_data != t.data_hash ? "data changed" : "palette changed", unsigned(t.data_hash), unsigned(t.tlut_hash));
        flush_batch(kWhyTexture);  // queued draws sample the old contents
        t.decoded_data = t.data_hash;
        t.decoded_tlut = t.tlut_hash;
        const std::vector<uint8_t> rgba = decode_level(0);
        ++g_render_stats.tex_decodes;
        g_render_stats.tex_decoded_texels += size_t(w) * h;
        if (indexed) ++g_render_stats.tex_indexed_decodes;
        if (const char* dir = std::getenv("GP_DUMP_TEX")) {
            // GP_DUMP_TEX_MAX: how many textures to write (default 24); GP_DUMP_TEX_AFTER: only once this many
            // frames have been presented (skips the textures of earlier scenes, such as a movie).
            static int dumped = 0;
            static const int max_dump = std::getenv("GP_DUMP_TEX_MAX") ? std::atoi(std::getenv("GP_DUMP_TEX_MAX")) : 24;
            static const long after = std::getenv("GP_DUMP_TEX_AFTER") ? std::atol(std::getenv("GP_DUMP_TEX_AFTER")) : 0;
            // A texture is written once per (address, format, size): a movie re-uploading into one buffer is dumped once.
            static std::unordered_map<uint64_t, bool> seen;
            const uint64_t key = (uint64_t(addr) << 32) ^ (uint64_t(fmt) << 28) ^ (uint64_t(w) << 14) ^ h;
            if (dumped < max_dump && long(g_frames) >= after && seen.emplace(key, true).second) {
                char path[512];
                std::snprintf(path, sizeof(path), "%s/tex_%02d_fmt%X_%ux%u_%06X.ppm", dir, dumped++, fmt, w, h, addr);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                    for (size_t i = 0; i < size_t(w) * h; ++i) {
                        const uint8_t a = rgba[i * 4 + 3];
                        const uint8_t px[3] = {uint8_t(rgba[i * 4] * a / 255 + 64 * (255 - a) / 255),
                                               uint8_t(rgba[i * 4 + 1] * a / 255 + 64 * (255 - a) / 255),
                                               uint8_t(rgba[i * 4 + 2] * a / 255 + 64 * (255 - a) / 255)};
                        std::fwrite(px, 1, 3, f);
                    }
                    std::fclose(f);
                }
            }
        }
        if (t.id == 0) glGenTextures(1, &t.id);
        edit_unit_texture(map, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        for (uint32_t level = 1; level < levels; ++level) {
            const std::vector<uint8_t> mip = decode_level(level);
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, mip_dim(w, level), mip_dim(h, level), 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, mip.data());
        }
        if (fmt == kTexC14X2) ++g_use.c14x2_textures;
        if (levels > 1) ++g_use.mip_textures;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(levels - 1));
        t.hash = hash;
        t.params_valid = false;
    }
    bind_unit_texture(map, t.id);
    apply_sampler(map, t.id, t.params, t.params_valid, wanted_sampler(mode0, mode1, levels));
    return t.id;
}

GLenum blend_factor(uint32_t f, bool is_src) {
    switch (f) {
        case 0: return GL_ZERO;
        case 1: return GL_ONE;
        case 2: return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
        case 3: return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
        case 4: return GL_SRC_ALPHA;
        case 5: return GL_ONE_MINUS_SRC_ALPHA;
        case 6: return GL_DST_ALPHA;
        default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

GLenum compare_func(uint32_t f) {
    static const GLenum table[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return table[f & 7];
}

// Uniform locations, looked up once after the link (glGetUniformLocation by name per draw was a large share of a frame).
struct UniformLocations {
    GLint nstages = -1, stage = -1, swap = -1, reg = -1, konst = -1, alpha_cmp = -1, fog_f = -1, fog_i = -1, fog_color = -1,
          const_alpha = -1;
} g_loc;  // the locations in the program in use (g_active)
UniformLocations g_locs[2];  // [0] g_program, [1] g_program_early

// A uniform's last uploaded words: the upload is skipped when the new value is bit-identical.
template <size_t N>
struct UniformShadow {
    bool valid = false;
    uint32_t w[N] = {};
    // Records `p` (N words); true when it differs from what the GL program holds.
    bool changed(const void* p) {
        if (valid && std::memcmp(w, p, N * sizeof(uint32_t)) == 0) return false;
        std::memcpy(w, p, N * sizeof(uint32_t));
        valid = true;
        return true;
    }
};

// What apply_state last told GL, so that a draw whose state equals the previous one's issues no GL call for it. Anything
// else that changes this state (EFB copies, the early-Z replay) must reset it: invalidate_shadow(). -1 / false: unknown.
struct Shadow {
    int scissor_on = -1;
    bool scissor_valid = false, viewport_valid = false, blend_func_valid = false, blend_eq_valid = false, logic_valid = false;
    GLint scissor[4] = {}, viewport[4] = {};
    int cull = -1;  // 0: culling off, else the GL face mode
    int depth_test = -1, depth_mask = -1;
    GLenum depth_func = 0;
    int blend_on = -1;
    GLenum blend_func[4] = {}, blend_eq[2] = {};
    int logic_on = -1;
    GLenum logic_op = 0;
    int color_mask = -1;
    UniformShadow<1> const_alpha, nstages;
    UniformShadow<64> stage;
    UniformShadow<16> swap, reg, konst;
    UniformShadow<2> alpha_cmp;
    UniformShadow<4> fog_f, fog_i;
    UniformShadow<3> fog_color;
} g_sh;

// State::gen the last apply_state sent to GL (~0u: unknown). The register state applied to GL is a function of the BP
// registers, the TEV colours and the XF viewport, all of which bump State::gen when they change (gp_parser.cpp), so
// a draw that follows another with nothing written in between skips the whole translation; the textures are still
// validated per draw, the guest may have rewritten one.
uint32_t g_applied_gen = ~0u;
bool g_used_maps[8] = {};  // maps the applied TEV stages sample

void invalidate_shadow() {
    g_sh = Shadow();
    g_applied_gen = ~0u;
}

template <size_t N>
bool same(const GLint (&a)[N], const GLint (&b)[N]) {
    return std::memcmp(a, b, sizeof a) == 0;
}

void apply_register_state(const State& s);

void apply_state(const State& s) {
    // GP_APPLY_ALWAYS=1: translate the register state for every draw (to compare pictures with and without the skip).
    static const bool always = std::getenv("GP_APPLY_ALWAYS") != nullptr;
    if (always || s.gen != g_applied_gen) apply_register_state(s);
    // Textures: bind the maps the stages use; the others get the white texture. (The samplers u_tex0..7 are set to
    // their units once, in init.)
    for (int m = 0; m < 8; ++m) {
        if (g_used_maps[m]) texture_for_map(s, m);
        else bind_unit_texture(m, g_white);
    }
}

// The draw's alpha-tested pixels that pass the depth test write depth even where the test rejects them, and this build has no early
// fragment tests: draw the draw again, depth only (flush_batch, draw_direct).
bool needs_replay(const State& s) { return !g_program_early && early_z_writes_rejected_pixels(s.bp); }

void apply_register_state(const State& s) {
    // The program: with early fragment tests for the draws that need them (see g_program_early).
    {
        const bool early = g_program_early && early_z_writes_rejected_pixels(s.bp);
        const GLuint want = early ? g_program_early : g_program;
        if (want != g_active) {
            flush_batch(kWhyProgram);  // queued draws belong to the other program
            g_active = want;
            glUseProgram(want);
            g_loc = g_locs[early ? 1 : 0];
            // The new program holds none of the uniform values sent to the other one.
            g_sh.const_alpha.valid = g_sh.nstages.valid = g_sh.stage.valid = g_sh.swap.valid = g_sh.reg.valid = g_sh.konst.valid = false;
            g_sh.alpha_cmp.valid = g_sh.fog_f.valid = g_sh.fog_i.valid = g_sh.fog_color.valid = false;
        }
    }
    // Viewport and scissor, in EFB coordinates (origin top-left) converted to GL (origin bottom-left).
    const float wd = s.xf_float(0x101A), ht = s.xf_float(0x101B), xo = s.xf_float(0x101D), yo = s.xf_float(0x101E);
    const float x_off = float((s.bp[0x59] & 0x3FF) * 2), y_off = float(((s.bp[0x59] >> 10) & 0x3FF) * 2);
    const float vx = xo - wd - x_off, vy = yo + ht - y_off, vw = 2.0f * wd, vh = -2.0f * ht;
    if (vw > 0 && vh > 0) {
        const GLint vp[4] = {int(vx), int(kEfbHeight - (vy + vh)), int(vw), int(vh)};
        if (!g_sh.viewport_valid || !same(g_sh.viewport, vp)) {
            glViewport(vp[0], vp[1], vp[2], vp[3]);
            std::memcpy(g_sh.viewport, vp, sizeof vp);
            g_sh.viewport_valid = true;
        }
    }

    const uint32_t tl = s.bp[0x20], br = s.bp[0x21];
    const int sx = int((tl >> 12) & 0x7FF) - 342, sy = int(tl & 0x7FF) - 342;
    const int sx2 = int((br >> 12) & 0x7FF) - 342, sy2 = int(br & 0x7FF) - 342;
    const int sw = sx2 - sx + 1, sh = sy2 - sy + 1;
    if (g_sh.scissor_on != 1) {
        glEnable(GL_SCISSOR_TEST);
        g_sh.scissor_on = 1;
    }
    if (sw > 0 && sh > 0) {
        const GLint sc[4] = {sx, kEfbHeight - (sy + sh), sw, sh};
        if (!g_sh.scissor_valid || !same(g_sh.scissor, sc)) {
            glScissor(sc[0], sc[1], sc[2], sc[3]);
            std::memcpy(g_sh.scissor, sc, sizeof sc);
            g_sh.scissor_valid = true;
        }
    }

    // GP_CULL=none: debugging aid, disables face culling.
    static const bool no_cull = std::getenv("GP_CULL") && !std::strcmp(std::getenv("GP_CULL"), "none");
    const uint32_t cull = no_cull ? 0 : (s.bp[0x00] >> 14) & 3;
    // GenMode cull field: 1 culls the faces GX calls back, 2 the front ones. GX's screen space has y down, OpenGL's
    // clip space y up, so a triangle GX sees counter-clockwise is clockwise here: the faces swap.
    const int cull_mode = cull == 0 ? 0 : int(cull == 1 ? GL_FRONT : cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
    if (g_sh.cull != cull_mode) {
        if (cull_mode == 0) {
            glDisable(GL_CULL_FACE);
        } else {
            if (g_sh.cull <= 0) glEnable(GL_CULL_FACE);
            glCullFace(GLenum(cull_mode));
        }
        g_sh.cull = cull_mode;
    }

    const uint32_t zmode = s.bp[0x40];
    const int depth_test = zmode & 1;
    if (g_sh.depth_test != depth_test) {
        if (depth_test) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        g_sh.depth_test = depth_test;
    }
    if (depth_test) {
        const GLenum func = compare_func((zmode >> 1) & 7);
        if (g_sh.depth_func != func) {
            glDepthFunc(func);
            g_sh.depth_func = func;
        }
    }
    const int depth_mask = (zmode & 0x10) ? 1 : 0;
    if (g_sh.depth_mask != depth_mask) {
        glDepthMask(depth_mask ? GL_TRUE : GL_FALSE);
        g_sh.depth_mask = depth_mask;
    }

    const BlendState blend = decode_blend(s.bp[0x41], s.bp[0x42]);
    // Constant alpha: o_color.a carries the constant, so the blend factors that use the source alpha read
    // the TEV alpha from the second shader output, and the alpha channel is written as it comes.
    const bool const_alpha = blend.const_alpha;
    auto factor = [&](uint32_t f, bool is_src) {
        if (const_alpha && f == 4) return GLenum(GL_SRC1_COLOR);
        if (const_alpha && f == 5) return GLenum(GL_ONE_MINUS_SRC1_COLOR);
        return blend_factor(f, is_src);
    };
    if (blend.blend) {
        if (g_sh.blend_on != 1) {
            glEnable(GL_BLEND);
            g_sh.blend_on = 1;
        }
        const GLenum src = factor(blend.src_factor, true), dst = factor(blend.dst_factor, false);
        const GLenum func[4] = {src, dst, const_alpha ? GLenum(GL_ONE) : src, const_alpha ? GLenum(GL_ZERO) : dst};
        if (!g_sh.blend_func_valid || std::memcmp(g_sh.blend_func, func, sizeof func) != 0) {
            glBlendFuncSeparate(func[0], func[1], func[2], func[3]);
            std::memcpy(g_sh.blend_func, func, sizeof func);
            g_sh.blend_func_valid = true;
        }
        const GLenum rgb_eq = blend.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        const GLenum eq[2] = {rgb_eq, const_alpha ? GLenum(GL_FUNC_ADD) : rgb_eq};
        if (!g_sh.blend_eq_valid || std::memcmp(g_sh.blend_eq, eq, sizeof eq) != 0) {
            glBlendEquationSeparate(eq[0], eq[1]);
            std::memcpy(g_sh.blend_eq, eq, sizeof eq);
            g_sh.blend_eq_valid = true;
        }
    } else if (g_sh.blend_on != 0) {
        glDisable(GL_BLEND);
        g_sh.blend_on = 0;
    }
    if (const_alpha) ++g_use.const_alpha_draws;
    if (blend.logic_op) {
        ++g_use.logic_op_draws;
        if (g_sh.logic_on != 1) {
            glEnable(GL_COLOR_LOGIC_OP);
            g_sh.logic_on = 1;
        }
        const GLenum op = GL_CLEAR + blend.logic_mode;  // GX_LO_* follows the GL enum order
        if (!g_sh.logic_valid || g_sh.logic_op != op) {
            glLogicOp(op);
            g_sh.logic_op = op;
            g_sh.logic_valid = true;
        }
    } else if (g_sh.logic_on != 0) {
        glDisable(GL_COLOR_LOGIC_OP);
        g_sh.logic_on = 0;
    }
    const float const_alpha_value = const_alpha ? float(blend.const_alpha_value) / 255.0f : -1.0f;
    if (g_sh.const_alpha.changed(&const_alpha_value)) glUniform1f(g_loc.const_alpha, const_alpha_value);
    const int color_mask = (blend.color_update ? 7 : 0) | (blend.alpha_update ? 8 : 0);
    if (g_sh.color_mask != color_mask) {
        glColorMask(blend.color_update, blend.color_update, blend.color_update, blend.alpha_update);
        g_sh.color_mask = color_mask;
    }

    // TEV stages.
    const int nstages = int(((s.bp[0x00] >> 10) & 0xF) + 1);
    uint32_t stage[16][4] = {};
    for (int i = 0; i < 16; ++i) {
        const uint32_t tref = s.bp[0x28 + i / 2];
        const uint32_t bits = (i & 1) ? (tref >> 12) : tref;
        const uint32_t map = bits & 7, coord = (bits >> 3) & 7, enable = (bits >> 6) & 1, chan = (bits >> 7) & 7;
        const uint32_t ksel = s.bp[0xF6 + i / 2];
        const uint32_t kc = (i & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        const uint32_t ka = (i & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        stage[i][0] = s.bp[0xC0 + 2 * i];
        stage[i][1] = s.bp[0xC1 + 2 * i];
        stage[i][2] = map | (coord << 3) | (enable << 6) | (chan << 7);
        stage[i][3] = kc | (ka << 5);
    }
    for (int i = 0; i < nstages; ++i) {
        if (((stage[i][0] >> 16) & 3) == 3 || ((stage[i][1] >> 16) & 3) == 3) {
            ++g_use.tev_compare_draws;
            break;
        }
    }
    if (g_sh.nstages.changed(&nstages)) glUniform1i(g_loc.nstages, nstages);
    if (g_sh.stage.changed(stage)) glUniform4uiv(g_loc.stage, 16, &stage[0][0]);

    int swap[4][4];
    for (int t = 0; t < 4; ++t) {
        const uint32_t lo = s.bp[0xF6 + 2 * t], hi = s.bp[0xF7 + 2 * t];
        swap[t][0] = int(lo & 3);
        swap[t][1] = int((lo >> 2) & 3);
        swap[t][2] = int(hi & 3);
        swap[t][3] = int((hi >> 2) & 3);
    }
    if (g_sh.swap.changed(swap)) glUniform4iv(g_loc.swap, 4, &swap[0][0]);
    if (g_sh.reg.changed(s.tev_reg)) glUniform4fv(g_loc.reg, 4, &s.tev_reg[0][0]);
    if (g_sh.konst.changed(s.tev_konst)) glUniform4fv(g_loc.konst, 4, &s.tev_konst[0][0]);

    const uint32_t ac = s.bp[0xF3];
    const uint32_t alpha_cmp[2] = {((ac >> 16) & 7) | (((ac >> 19) & 7) << 3) | (((ac >> 22) & 3) << 6),
                                   (ac & 0xFF) | (((ac >> 8) & 0xFF) << 8)};
    if (g_sh.alpha_cmp.changed(alpha_cmp)) glUniform2ui(g_loc.alpha_cmp, alpha_cmp[0], alpha_cmp[1]);

    const FogParams fog = decode_fog(s.bp);
    if (fog.type != kFogOff) ++g_use.fog_draws;
    const float fog_f[4] = {fog.a, fog.c, 0.0f, 0.0f};
    if (g_sh.fog_f.changed(fog_f)) glUniform4f(g_loc.fog_f, fog_f[0], fog_f[1], fog_f[2], fog_f[3]);
    const int fog_i[4] = {GLint(fog.b_mag), GLint(fog.b_shift), fog.ortho ? 1 : 0, GLint(fog.type)};
    if (g_sh.fog_i.changed(fog_i)) glUniform4i(g_loc.fog_i, fog_i[0], fog_i[1], fog_i[2], fog_i[3]);
    if (g_sh.fog_color.changed(fog.color)) glUniform3fv(g_loc.fog_color, 1, fog.color);

    for (int m = 0; m < 8; ++m) g_used_maps[m] = false;
    for (int i = 0; i < nstages; ++i)
        if ((stage[i][2] >> 6) & 1) g_used_maps[stage[i][2] & 7] = true;
    g_applied_gen = s.gen;
}

void print_feature_use() {
    if (g_use.filtered_copies) std::fprintf(stderr, "[GL] %llu EFB copies went through the vertical copy filter (GP_COPY_FILTER=on)\n", (unsigned long long)g_use.filtered_copies);
    std::fprintf(stderr, "[GL] feature use: C14X2 textures %llu, mipmapped textures %llu, fog draws %llu, constant-alpha draws %llu, logic-op draws %llu, early-Z passes %llu, TEV compare-mode draws %llu\n",
                 (unsigned long long)g_use.c14x2_textures, (unsigned long long)g_use.mip_textures, (unsigned long long)g_use.fog_draws,
                 (unsigned long long)g_use.const_alpha_draws, (unsigned long long)g_use.logic_op_draws, (unsigned long long)g_use.early_z_passes, (unsigned long long)g_use.tev_compare_draws);
    if (g_indirect_draws) std::fprintf(stderr, "[GL] %llu draw(s) used indirect texturing (not implemented)\n", (unsigned long long)g_indirect_draws);
}

// Reads the window back (the blit that precedes leaves the EFB bound for reading: read the window).
std::vector<uint8_t> read_window_rgb() {
    std::vector<uint8_t> px(size_t(g_window_w) * g_window_h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, g_window_w, g_window_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    // GL rows run bottom to top; the file and the signature want the top row first.
    for (int a = 0, b = g_window_h - 1; a < b; ++a, --b)
        std::swap_ranges(px.begin() + size_t(a) * g_window_w * 3, px.begin() + size_t(a + 1) * g_window_w * 3, px.begin() + size_t(b) * g_window_w * 3);
    return px;
}

void write_ppm(const char* path, const std::vector<uint8_t>& px) {
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", g_window_w, g_window_h);
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
    }
}

// A diagnostic bundle's picture of the EFB (colour, and alpha apart) over the source rectangle of the copy that is about to
// run: the stage of a screen-space effect built from copies. The XFB copy's picture is always kept, the texture copies' up
// to diag::kMaxEfbImages.
void diag_capture_efb(const CopyRequest& r, const State& s) {
    diag::Bundle& b = diag::g_bundle;
    if (!r.to_xfb && int(b.efb.size()) >= diag::kMaxEfbImages) {
        ++b.efb_dropped;
        return;
    }
    const int x = std::clamp(r.x, 0, kEfbWidth - 1), y = std::clamp(r.y, 0, kEfbHeight - 1);
    const int w = std::min(r.w, kEfbWidth - x), h = std::min(r.h, kEfbHeight - y);
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, kEfbHeight - (y + h), w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    diag::Image im;
    im.w = w;
    im.h = h;
    im.rgb.resize(size_t(w) * h * 3);
    im.alpha.resize(size_t(w) * h);
    for (int row = 0; row < h; ++row) {  // GL rows run bottom to top
        const uint8_t* src = &rgba[size_t(h - 1 - row) * w * 4];
        for (int col = 0; col < w; ++col) {
            uint8_t* rgb = &im.rgb[(size_t(row) * w + col) * 3];
            rgb[0] = src[col * 4];
            rgb[1] = src[col * 4 + 1];
            rgb[2] = src[col * 4 + 2];
            im.alpha[size_t(row) * w + col] = src[col * 4 + 3];
        }
    }
    if (r.to_xfb) {
        im.label = "xfb";
    } else {
        char label[64];
        std::snprintf(label, sizeof label, "copy%02llu-dest%06X-fmt%X", (unsigned long long)b.counts["efb copies to a texture"], (s.bp[0x4B] & 0xFFFFFF) << 5,
                      ((s.bp[0x52] >> 4) & 7) | (((s.bp[0x52] >> 3) & 1) << 3));
        im.label = label;
    }
    b.efb.push_back(std::move(im));
}

// Called after the frame is drawn and before it is swapped: periodic screenshots, the shots a script
// asked for, and the picture signature a script waits on.
void write_screenshot() {
    static uint64_t presented = 0;
    static const int every = [] {
        const char* v = std::getenv("GP_SHOT_EVERY");
        return v ? std::max(1, std::atoi(v)) : 1;
    }();
    const std::vector<std::string> requested = input::take_shot_requests();
    const bool periodic = presented++ % every == 0 && g_shot_dir && !(g_shot_max && g_shot_count >= g_shot_max);
    const bool sig = input::wants_signature();
    if (!periodic && requested.empty() && !sig) {
        input::notify_frame(nullptr);
        return;
    }
    const std::vector<uint8_t> px = read_window_rgb();
    if (sig) {
        const input::FrameSignature s = input::frame_signature(px.data(), g_window_w, g_window_h);
        input::notify_frame(&s);
    } else {
        input::notify_frame(nullptr);
    }
    char path[1024];
    for (const std::string& name : requested) {
        if (!g_shot_dir) {
            std::fprintf(stderr, "[script] shot %s: no screenshot directory (TOS_SCREENSHOT_DIR)\n", name.c_str());
            continue;
        }
        std::snprintf(path, sizeof(path), "%s/%s.ppm", g_shot_dir, name.c_str());
        write_ppm(path, px);
    }
    if (periodic) {
        std::snprintf(path, sizeof(path), "%s/frame_%04d.ppm", g_shot_dir, g_shot_count++);
        write_ppm(path, px);
    }
}

}  // namespace

bool init(int window_w, int window_h, const char* screenshot_dir, int max_screenshots) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[GL] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("tos-recomp", window_w, window_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        std::fprintf(stderr, "[GL] cannot create a window: %s\n", SDL_GetError());
        return false;
    }
    g_context = SDL_GL_CreateContext(g_window);
    g_gl_thread = std::this_thread::get_id();
    g_mem.read_guard_hook = &read_guard_hit;
    if (!g_context) {
        std::fprintf(stderr, "[GL] cannot create an OpenGL 3.3 context: %s\n", SDL_GetError());
        return false;
    }
    if (!gl_load_functions()) {
        std::fprintf(stderr, "[GL] the driver does not expose the required OpenGL 3.3 entry points\n");
        return false;
    }
    SDL_GL_SetSwapInterval(0);
    SDL_GetWindowSizeInPixels(g_window, &g_window_w, &g_window_h);
    g_shot_dir = screenshot_dir;
    g_shot_max = max_screenshots;
    diag::set_default_dir(screenshot_dir);
    std::fprintf(stderr, "[GL] %s, %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kEfbWidth, kEfbHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // The EFB colour texture is also sampled, by the pass that presents a filtered copy (copy_filter in present).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEfbWidth, kEfbHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[GL] EFB framebuffer is incomplete\n");
        return false;
    }
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;
    g_program = glCreateProgram();
    glAttachShader(g_program, vs);
    glAttachShader(g_program, fs);
    glLinkProgram(g_program);
    GLint linked = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048];
        glGetProgramInfoLog(g_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[GL] link failed:\n%s\n", log);
        return false;
    }
    auto locate = [](GLuint prog, UniformLocations& l) {
        glUseProgram(prog);
        l.nstages = glGetUniformLocation(prog, "u_nstages");
        l.stage = glGetUniformLocation(prog, "u_stage");
        l.swap = glGetUniformLocation(prog, "u_swap");
        l.reg = glGetUniformLocation(prog, "u_reg");
        l.konst = glGetUniformLocation(prog, "u_konst");
        l.alpha_cmp = glGetUniformLocation(prog, "u_alpha_cmp");
        l.fog_f = glGetUniformLocation(prog, "u_fog_f");
        l.fog_i = glGetUniformLocation(prog, "u_fog_i");
        l.fog_color = glGetUniformLocation(prog, "u_fog_color");
        l.const_alpha = glGetUniformLocation(prog, "u_const_alpha");
        for (int m = 0; m < 8; ++m) {
            char name[16];
            std::snprintf(name, sizeof(name), "u_tex%d", m);
            glUniform1i(glGetUniformLocation(prog, name), m);
        }
    };
    locate(g_program, g_locs[0]);
    // The early-fragment-test variant. GP_EARLY_TESTS=0 leaves it out (the depth replay is used then).
    const char* early_env = std::getenv("GP_EARLY_TESTS");
    const char* early_state = "disabled by GP_EARLY_TESTS=0";
    if (!early_env || std::atoi(early_env) != 0) {
        early_state = "not available (the program did not build)";
        std::string src = kFragmentShader;
        const size_t eol = src.find('\n');
        src.insert(eol + 1, "#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n");
        // A compile failure here only means the extension is missing: the log is not an error.
        const GLuint efs = compile(GL_FRAGMENT_SHADER, src.c_str());
        GLuint prog = efs ? glCreateProgram() : 0;
        GLint ok = 0;
        if (prog) {
            glAttachShader(prog, vs);
            glAttachShader(prog, efs);
            glLinkProgram(prog);
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        }
        if (ok) {
            g_program_early = prog;
            locate(prog, g_locs[1]);
            early_state = "active";
        } else {
            std::fprintf(stderr, "[GL] early fragment tests are not available: alpha-tested draws that write depth are drawn twice\n");
        }
    }
    g_active = g_program;
    g_loc = g_locs[0];
    glUseProgram(g_active);
    {
        bool ext = false;
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n && !ext; ++i) {
            const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
            ext = e && !std::strcmp(e, "GL_ARB_shader_image_load_store");
        }
        const char* merge_env = std::getenv("GP_DRAW_MERGE");
        std::fprintf(stderr, "[GL] draw batching %s; early fragment tests %s (GL_ARB_shader_image_load_store %s)\n",
                     !merge_env || std::atoi(merge_env) != 0 ? "on" : "off (GP_DRAW_MERGE=0)", early_state, ext ? "listed" : "not listed");
    }

    // The pass that presents an EFB copy through the vertical filter (a plain blit when the filter is the identity).
    {
        const GLuint pvs = compile(GL_VERTEX_SHADER, kPresentVertexShader);
        const GLuint pfs = compile(GL_FRAGMENT_SHADER, kPresentFragmentShader);
        GLint ok = 0;
        if (pvs && pfs) {
            g_present_program = glCreateProgram();
            glAttachShader(g_present_program, pvs);
            glAttachShader(g_present_program, pfs);
            glLinkProgram(g_present_program);
            glGetProgramiv(g_present_program, GL_LINK_STATUS, &ok);
        }
        if (!ok) {
            std::fprintf(stderr, "[GL] the copy filter pass did not build: presented frames are not filtered\n");
            g_present_program = 0;
        } else {
            g_present_loc.rect = glGetUniformLocation(g_present_program, "u_rect");
            g_present_loc.misc = glGetUniformLocation(g_present_program, "u_misc");
            g_present_loc.view = glGetUniformLocation(g_present_program, "u_view");
            g_present_loc.weights = glGetUniformLocation(g_present_program, "u_weights");
            glUseProgram(g_present_program);
            glUniform1i(glGetUniformLocation(g_present_program, "u_efb"), kEfbUnit);
            glUseProgram(g_active);
        }
        glGenVertexArrays(1, &g_present_vao);
    }

    // The pass that keeps an EFB copy as a texture.
    {
        const GLuint cfs = compile(GL_FRAGMENT_SHADER, kCopyFragmentShader);
        const GLuint cvs = compile(GL_VERTEX_SHADER, kPresentVertexShader);
        GLint ok = 0;
        if (cvs && cfs) {
            g_copy_program = glCreateProgram();
            glAttachShader(g_copy_program, cvs);
            glAttachShader(g_copy_program, cfs);
            glLinkProgram(g_copy_program);
            glGetProgramiv(g_copy_program, GL_LINK_STATUS, &ok);
        }
        if (!ok) {
            std::fprintf(stderr, "[GL] the EFB copy pass did not build: copies are read back at once\n");
            g_copy_program = 0;
        } else {
            g_copy_origin_loc = glGetUniformLocation(g_copy_program, "u_origin");
            g_copy_size_loc = glGetUniformLocation(g_copy_program, "u_size");
            g_copy_mode_loc = glGetUniformLocation(g_copy_program, "u_mode");
            glUseProgram(g_copy_program);
            glUniform1i(glGetUniformLocation(g_copy_program, "u_efb"), kEfbUnit);
            glUseProgram(g_active);
            glGenFramebuffers(1, &g_copy_fbo);
        }
    }

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    GLuint ibo = 0;
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);  // part of the VAO: the index list of a batch
    const GLsizei stride = kVertexFloats * sizeof(float);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    for (int i = 0; i < 8; ++i) glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, stride, (void*)((12 + 3 * i) * sizeof(float)));
    for (int i = 0; i < 11; ++i) glEnableVertexAttribArray(i);

    const uint8_t white[4] = {255, 255, 255, 255};
    glGenTextures(1, &g_white);
    glBindTexture(GL_TEXTURE_2D, g_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    return true;
}

void flush_copies() {
    // A sync point: write the copies the GPU has finished, and any that is two frames old. The others stay pending: a read of their
    // bytes by the CPU is caught by the read guard (Memory::read_guard_hook) and writes them then.
    for (size_t i = 0; i < g_pending.size();) {
        const GLenum r = g_frames - g_pending[i].frame >= 2 ? GL_ALREADY_SIGNALED : glClientWaitSync(g_pending[i].fence, 0, 0);
        if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) flush_pending_at(i);
        else ++i;
    }
}

void shutdown() {
    flush_batch();
    flush_all_copies();
    g_mem.read_guard_hook = nullptr;
    g_mem.guard_count = 0;
    print_feature_use();
    overlay_glue::stop();
    if (g_context) SDL_GL_DestroyContext(g_context);
    if (g_window) SDL_DestroyWindow(g_window);
    g_context = nullptr;
    g_window = nullptr;
    SDL_Quit();
}

namespace {

// ---- Draw batching --------------------------------------------------------------------------------------------------
// The vertices are transformed on the CPU, so consecutive draws that share all GL state (State::gen unchanged, the same
// textures bound: nothing below is called in between that changes GL state without flush_batch()) can be one GL draw: their
// vertices are collected and drawn through an index list. A strip or fan becomes the triangles GL would make of it, a quad
// two triangles (0 1 2) (0 2 3). Nothing here reorders anything: the triangles are drawn in the order they came.
struct Batch {
    bool open = false;
    uint32_t ndraws = 0;   // draws collected
    bool early_z = false;  // each draw needs the depth replay (below): the group must be pixel-disjoint
    uint32_t gen = 0;
    std::vector<std::array<float, 4>> boxes;  // clip-space boxes of the draws of an early-Z group
    std::vector<float> verts;
    std::vector<uint32_t> idx;
} g_batch;
size_t g_ring_used = 0;
bool g_in_draw = false;

void flush_batch(int why) {
    if (!g_batch.open) return;
    g_batch.open = false;
    if (g_batch.idx.empty()) {
        g_batch.verts.clear();
        g_batch.ndraws = 0;
        return;
    }
    bench::ScopedNs flush_timer(g_render_stats.flush_ns);
    ++g_render_stats.batches;
    ++g_render_stats.flush_why[why];
    static uint64_t nested_ns = 0;  // renderer::draw's own timer already counts a flush it makes
    bench::ScopedNs timer(g_in_draw ? nested_ns : g_render_stats.draw_ns);
    constexpr size_t kVertexBytes = kVertexFloats * sizeof(float);
    constexpr size_t kRingBytes = (16u << 20) / kVertexBytes * kVertexBytes;
    const size_t bytes = g_batch.verts.size() * sizeof(float);
    GLint first = 0;
    if (bytes > kRingBytes) {
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), g_batch.verts.data(), GL_STREAM_DRAW);
        g_ring_valid = false;
    } else {
        if (!g_ring_valid || g_ring_used + bytes > kRingBytes) {
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(kRingBytes), nullptr, GL_STREAM_DRAW);
            g_ring_valid = true;
            g_ring_used = 0;
        }
        // Unsynchronized: the range written is never one a queued draw still reads (the ring only moves forward).
        void* dst = glMapBufferRange(GL_ARRAY_BUFFER, GLsizeiptr(g_ring_used), GLsizeiptr(bytes),
                                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (dst) {
            std::memcpy(dst, g_batch.verts.data(), bytes);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, GLsizeiptr(g_ring_used), GLsizeiptr(bytes), g_batch.verts.data());
        }
        first = GLint(g_ring_used / kVertexBytes);
        g_ring_used += bytes;
    }
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(g_batch.idx.size() * sizeof(uint32_t)), g_batch.idx.data(), GL_STREAM_DRAW);
    glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(g_batch.idx.size()), GL_UNSIGNED_INT, nullptr, first);
    ++g_render_stats.gl_draws;
    if (g_batch.early_z) {
        // Z compare before texturing: pixels the alpha test rejected still update depth. Replay the group without colour and
        // without the alpha test; pixels that passed already wrote their depth and fail the depth test against themselves, the
        // rejected ones write it now. The draws of the group touch no common pixel (batch_add), so replaying them together
        // is replaying each after its own draw. The colour mask and the alpha test uniform are put back.
        ++g_use.early_z_passes;
        g_render_stats.draws_replayed += g_batch.ndraws;
        const int mask = g_sh.color_mask;
        const uint32_t cmp0 = g_sh.alpha_cmp.w[0], cmp1 = g_sh.alpha_cmp.w[1];
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUniform2ui(g_loc.alpha_cmp, 7u | (7u << 3), 0u);
        glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(g_batch.idx.size()), GL_UNSIGNED_INT, nullptr, first);
        if (mask >= 0) glColorMask((mask & 7) != 0, (mask & 7) != 0, (mask & 7) != 0, (mask & 8) != 0);
        else g_applied_gen = ~0u;
        glUniform2ui(g_loc.alpha_cmp, cmp0, cmp1);
        if (!g_sh.alpha_cmp.valid) g_applied_gen = ~0u;
    }
    g_batch.verts.clear();
    g_batch.idx.clear();
    g_batch.boxes.clear();
    g_batch.ndraws = 0;
}

void batch_add(const DrawCall& call, const State& s) {
    bench::ScopedNs add_timer(g_render_stats.add_ns);
    ++g_render_stats.draws_batched;
    std::vector<float>& verts = g_batch.verts;
    std::vector<uint32_t>& idx = g_batch.idx;
    if (g_batch.open && (verts.size() * sizeof(float) > (8u << 20) || (g_batch.early_z && g_batch.boxes.size() >= 64))) flush_batch(kWhySize);
    const bool early_z = needs_replay(s);
    if (early_z) {
        // The box of the draw in clip space (a vertex at or behind the eye plane: the whole screen), a little widened.
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (uint32_t v = 0; v < call.vertex_count; ++v) {
            const float* f = call.vertices + size_t(v) * kVertexFloats;
            if (!(f[3] > 0.0f)) {
                x0 = y0 = -1e30f;
                x1 = y1 = 1e30f;
                break;
            }
            const float nx = f[0] / f[3], ny = f[1] / f[3];
            x0 = std::min(x0, nx); x1 = std::max(x1, nx); y0 = std::min(y0, ny); y1 = std::max(y1, ny);
        }
        x0 -= 0.01f; y0 -= 0.01f; x1 += 0.01f; y1 += 0.01f;
        if (g_batch.open)
            for (const auto& b : g_batch.boxes)
                if (!(x1 < b[0] || x0 > b[2] || y1 < b[1] || y0 > b[3])) {
                    flush_batch(kWhyOverlap);  // pixels in common: the replay of the first must come before the second's draw
                    break;
                }
        if (!g_batch.open) {
            g_batch.open = true;
            g_batch.gen = s.gen;
        }
        g_batch.boxes.push_back({x0, y0, x1, y1});
    }
    if (!g_batch.open) {
        g_batch.open = true;
        g_batch.gen = s.gen;
    }
    g_batch.early_z = early_z;
    ++g_batch.ndraws;
    const uint32_t base = uint32_t(verts.size() / kVertexFloats), n = call.vertex_count;
    verts.insert(verts.end(), call.vertices, call.vertices + size_t(n) * kVertexFloats);
    switch (call.primitive) {
        case kTriangles:
            for (uint32_t i = 0; i + 2 < n; i += 3) {
                idx.push_back(base + i);
                idx.push_back(base + i + 1);
                idx.push_back(base + i + 2);
            }
            break;
        case kQuads:
            for (uint32_t q = 0; q + 3 < n; q += 4) {
                const uint32_t b = base + q;
                const uint32_t quad[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
                idx.insert(idx.end(), quad, quad + 6);
            }
            break;
        case kTriangleStrip:
            for (uint32_t i = 0; i + 2 < n; ++i) {
                // GL's own decomposition: every odd triangle swaps its first two vertices to keep the winding.
                idx.push_back(base + i + (i & 1));
                idx.push_back(base + i + 1 - (i & 1));
                idx.push_back(base + i + 2);
            }
            break;
        default:  // kTriangleFan
            for (uint32_t i = 0; i + 2 < n; ++i) {
                idx.push_back(base);
                idx.push_back(base + i + 1);
                idx.push_back(base + i + 2);
            }
            break;
    }
}

}  // namespace

// One draw at once, as GL's own primitive: lines, points, and the draws whose state needs a depth replay after each draw.
static void draw_direct(const DrawCall& call, const State& s) {
    const float* data = call.vertices;
    GLenum mode = GL_TRIANGLES;
    uint32_t count = call.vertex_count;
    const bool quads = call.primitive == kQuads;
    switch (call.primitive) {
        case kQuads: count = count / 4 * 6; break;  // two triangles per quad, written straight into the buffer below
        case kTriangles: mode = GL_TRIANGLES; break;
        case kTriangleStrip: mode = GL_TRIANGLE_STRIP; break;
        case kTriangleFan: mode = GL_TRIANGLE_FAN; break;
        case kLines: mode = GL_LINES; break;
        case kLineStrip: mode = GL_LINE_STRIP; break;
        case kPoints: mode = GL_POINTS; break;
        default: return;
    }
    if (count == 0) return;
    // Vertices go to the next free part of one big buffer (glBufferData per draw allocated a new buffer each time);
    // when it is full the buffer is orphaned and filling starts over.
    constexpr size_t kVertexBytes = kVertexFloats * sizeof(float);
    constexpr size_t kRingBytes = (16u << 20) / kVertexBytes * kVertexBytes;  // a whole number of vertices
    size_t& ring_used = g_ring_used;
    static std::vector<uint8_t> oversized;  // only for a draw larger than the ring
    const size_t bytes = size_t(count) * kVertexBytes;
    // Copies the vertices to `dst`; quads become the triangles (0 1 2) (0 2 3), one vertex copy each.
    auto fill = [&](uint8_t* dst) {
        if (!quads) {
            std::memcpy(dst, data, bytes);
            return;
        }
        static const int order[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t q = 0; q + 3 < call.vertex_count; q += 4)
            for (int k : order) {
                std::memcpy(dst, data + size_t(q + k) * kVertexFloats, kVertexBytes);
                dst += kVertexBytes;
            }
    };
    GLint first = 0;
    if (bytes > kRingBytes) {
        oversized.resize(bytes);
        fill(oversized.data());
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), oversized.data(), GL_STREAM_DRAW);
        g_ring_valid = false;
    } else {
        if (!g_ring_valid || ring_used + bytes > kRingBytes) {
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(kRingBytes), nullptr, GL_STREAM_DRAW);
            g_ring_valid = true;
            ring_used = 0;
        }
        // Unsynchronized: the range written is never one a queued draw still reads (the ring only moves forward).
        void* dst = glMapBufferRange(GL_ARRAY_BUFFER, GLsizeiptr(ring_used), GLsizeiptr(bytes),
                                     GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (dst) {
            fill(static_cast<uint8_t*>(dst));
            glUnmapBuffer(GL_ARRAY_BUFFER);
        } else {
            oversized.resize(bytes);
            fill(oversized.data());
            glBufferSubData(GL_ARRAY_BUFFER, GLsizeiptr(ring_used), GLsizeiptr(bytes), oversized.data());
        }
        first = GLint(ring_used / kVertexBytes);
        ring_used += bytes;
    }
    static const bool xhist = std::getenv("GP_XHIST") != nullptr;
    if (xhist) {
        static float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        static int frame_draws = 0;
        for (uint32_t v = 0; v < call.vertex_count; ++v) {
            const float w = data[size_t(v) * kVertexFloats + 3];
            const float cx = data[size_t(v) * kVertexFloats] / w, cy = data[size_t(v) * kVertexFloats + 1] / w;
            minx = std::min(minx, cx); maxx = std::max(maxx, cx); miny = std::min(miny, cy); maxy = std::max(maxy, cy);
        }
        ++frame_draws;
        if (frame_draws == 60) {
            std::fprintf(stderr, "[GL] clip bbox over 60 draws: x %g..%g y %g..%g\n", minx, maxx, miny, maxy);
            frame_draws = 0; minx = miny = 1e9f; maxx = maxy = -1e9f;
        }
    }
    glDrawArrays(mode, first, GLsizei(count));
    if (needs_replay(s)) {
        // Z compare before texturing: pixels the alpha test rejected still update depth. Replay the
        // draw without color and without the alpha test; pixels that passed already wrote their depth
        // and fail the depth test against themselves, the rejected ones write it now. apply_state
        // restores the color mask and the alpha test uniform for the next draw.
        ++g_use.early_z_passes;
        ++g_render_stats.draws_replayed;
        // The state this draw ran with, put back after the replay (a full translation of the state for every such draw was 3% of a boss frame).
        const int mask = g_sh.color_mask;
        const uint32_t cmp0 = g_sh.alpha_cmp.w[0], cmp1 = g_sh.alpha_cmp.w[1];
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glUniform2ui(g_loc.alpha_cmp, 7u | (7u << 3), 0u);
        glDrawArrays(mode, first, GLsizei(count));
        if (mask >= 0) glColorMask((mask & 7) != 0, (mask & 7) != 0, (mask & 7) != 0, (mask & 8) != 0);
        else g_applied_gen = ~0u;
        glUniform2ui(g_loc.alpha_cmp, cmp0, cmp1);
        if (!g_sh.alpha_cmp.valid) g_applied_gen = ~0u;
    }
}

void draw(const DrawCall& call, const State& s) {
    // GP_SKIP_DRAW=1: the draws are parsed and decoded but not given to GL (isolates the cost of the renderer and the GPU).
    static const bool skip_draw = std::getenv("GP_SKIP_DRAW") != nullptr;
    if (skip_draw) return;
    bench::ScopedNs timer(g_render_stats.draw_ns);
    struct InDraw {
        InDraw() { g_in_draw = true; }
        ~InDraw() { g_in_draw = false; }
    } in_draw;
    static uint64_t calls = 0;
    ++calls;
    static const bool gp_trace = std::getenv("GP_TRACE") != nullptr;
    if ((calls == 1 || calls == 5000 || calls == 30000 || calls == 100000) && gp_trace) {
        std::fprintf(stderr, "[GL] ---- draw #%llu\n", (unsigned long long)calls);
        const int nst = int(((s.bp[0x00] >> 10) & 0xF) + 1);
        std::fprintf(stderr, "[GL] draw prim=%u n=%u genmode=%06X ntev=%d zmode=%02X blend=%04X acmp=%06X\n", unsigned(call.primitive),
                     call.vertex_count, s.bp[0], nst, s.bp[0x40], s.bp[0x41], s.bp[0xF3]);
        std::fprintf(stderr, "[GL]   viewport wd=%g ht=%g xo=%g yo=%g zr=%g far=%g proj=%g %g %g %g %g %g ortho=%u\n",
                     s.xf_float(0x101A), s.xf_float(0x101B), s.xf_float(0x101D), s.xf_float(0x101E), s.xf_float(0x101C),
                     s.xf_float(0x101F), s.xf_float(0x1020), s.xf_float(0x1021), s.xf_float(0x1022), s.xf_float(0x1023),
                     s.xf_float(0x1024), s.xf_float(0x1025), s.xf[0x1026]);
        for (int i = 0; i < nst && i < 4; ++i)
            std::fprintf(stderr, "[GL]   stage %d: color=%06X alpha=%06X tref=%06X ksel=%06X\n", i, s.bp[0xC0 + 2 * i], s.bp[0xC1 + 2 * i],
                         s.bp[0x28 + i / 2], s.bp[0xF6 + i / 2]);
        for (uint32_t v = 0; v < call.vertex_count && v < 4; ++v) {
            const float* f = call.vertices + size_t(v) * kVertexFloats;
            std::fprintf(stderr, "[GL]   v%u clip=(%g %g %g %g) c0=(%g %g %g %g) uv0=(%g %g %g)\n", v, f[0], f[1], f[2], f[3], f[4], f[5],
                         f[6], f[7], f[12], f[13], f[14]);
        }
        std::fprintf(stderr, "[GL]   tex0 img0=%06X img3=%06X mode0=%06X\n", s.bp[0x88], s.bp[0x94], s.bp[0x80]);
    }
    if (const uint32_t ind = indirect_tev_stages(s.bp)) {
        // Indirect texturing is not implemented: the draw is rendered as if the stages were direct.
        // Say which games need it, once per distinct configuration.
        ++g_indirect_draws;
        static uint32_t seen[8];
        static int nseen = 0;
        const uint32_t sig = ind ^ (((s.bp[0x00] >> 16) & 7) << 16);
        bool known = false;
        for (int i = 0; i < nseen; ++i) known |= seen[i] == sig;
        if (!known && nseen < 8) {
            seen[nseen++] = sig;
            std::fprintf(stderr, "[GL] indirect texturing used (draw #%llu): %u indirect stage(s), TEV stages 0x%04X; not implemented, drawn as direct\n",
                         (unsigned long long)calls, (s.bp[0x00] >> 16) & 7, ind);
        }
    }
    if (!g_efb_bound) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        g_efb_bound = true;
    }
    {
        static uint32_t last_gen = ~0u;
        if (s.gen == last_gen) ++g_render_stats.draws_same_state;
        last_gen = s.gen;
    }
    if (g_batch.open && g_batch.gen != s.gen) flush_batch(kWhyState);  // the state changes below
    {
        bench::ScopedNs apply_timer(g_render_stats.apply_ns);
        apply_state(s);
    }
    // Why this draw does, or does not, need the depth write of alpha-rejected pixels (the first reason that applies).
    if (!((s.bp[0x43] >> 6) & 1)) ++g_render_stats.ez_late;
    else if (!(s.bp[0x40] & 1)) ++g_render_stats.ez_no_depth_test;
    else if (!((s.bp[0x40] >> 4) & 1)) ++g_render_stats.ez_no_depth_write;
    else if (!alpha_test_can_reject(s.bp[0xF3])) ++g_render_stats.ez_alpha_never_rejects;
    else if (g_program_early) ++g_render_stats.draws_early_program;

    // Triangles, quads, strips and fans of one state are collected and drawn as one indexed triangle list.
    static const bool merge = std::getenv("GP_DRAW_MERGE") == nullptr || std::atoi(std::getenv("GP_DRAW_MERGE")) != 0;
    const bool mergeable = merge && (call.primitive == kQuads || call.primitive == kTriangles || call.primitive == kTriangleStrip ||
                                     call.primitive == kTriangleFan);
    if (mergeable) {
        batch_add(call, s);
        return;
    }
    flush_batch(kWhyDirect);
    ++g_render_stats.draws_direct;
    draw_direct(call, s);
}

// The vertical copy filter (BP 0x53 / 0x54, efb_copy.h) of a copy; the identity unless GP_COPY_FILTER=on, and for depth.
static CopyFilter copy_filter_of(const State& s, bool depth) {
    static const bool on = [] {
        const char* v = std::getenv("GP_COPY_FILTER");
        return v && (!std::strcmp(v, "on") || !std::strcmp(v, "1"));
    }();
    return (depth || !on) ? CopyFilter() : make_copy_filter(s.bp[0x53], s.bp[0x54]);
}

// Presents the copied EFB region through the copy filter: each window pixel shows
//   (above * line(y-1) + centre * line(y) + below * line(y+1)) / 64
// of the EFB lines under it. The lines beyond the rectangle's first and last repeat those where the clamp bits (command bits 0
// and 1) say so, and are read from the EFB otherwise (copy_filter_margins).
static void present_filtered(const CopyRequest& r, const State& s, const CopyFilter& f) {
    EfbCopyParams p;
    p.y = r.y;
    p.h = r.h;
    p.clamp_top = s.bp[0x52] & 1;
    p.clamp_bottom = (s.bp[0x52] >> 1) & 1;
    int above = 0, below = 0;
    copy_filter_margins(p, kEfbHeight, &above, &below);
    glUseProgram(g_present_program);
    glBindVertexArray(g_present_vao);
    bind_unit_texture(kEfbUnit, g_color);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glUniform4f(g_present_loc.rect, float(r.x), float(kEfbHeight - (r.y + r.h)), float(r.w), float(r.h));
    glUniform4f(g_present_loc.misc, float(g_present_rect.w), float(g_present_rect.h), float(above), float(below));
    glUniform4f(g_present_loc.view, float(g_present_rect.x), float(g_present_rect.y), 0.0f, 0.0f);
    glUniform4f(g_present_loc.weights, float(f.above) / 64.0f, float(f.centre) / 64.0f, float(f.below) / 64.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    bind_unit_texture(kEfbUnit, g_white);  // the draws' sampler for this unit must not read the texture they render to
    glBindVertexArray(g_vao);
    glUseProgram(g_active);
}

// Copy a region of the EFB to main memory in the console's tiled layout (BP 0x52 with the XFB bit
// clear). Reads back from the GPU. The texture cache keys on a hash of the guest bytes, so entries
// overlapping the destination are re-decoded on their next use without explicit invalidation.
// True when the destination rows follow each other without a gap: the texture decoder reads the copy as one packed image.
static bool copy_is_packed(const EfbCopyParams& p) {
    int ow, oh, tw, th, tb;
    efb_copy_output_size(p, &ow, &oh);
    efb_copy_tile_geometry(p, &tw, &th, &tb);
    return p.stride == uint32_t((ow + tw - 1) / tw) * uint32_t(tb);
}

// Keeps the copy as a GL texture and starts its read-back; false when there is no slot or buffer to do it (the caller reads back
// at once).
static bool copy_to_gpu(const EfbCopyParams& p, uint32_t addr, uint32_t extent) {
    if (g_copy_tracker.pending_count() >= 6) flush_all_copies();
    GpuCopy fresh;
    fresh.addr = addr;
    fresh.extent = extent;
    int ow, oh;
    efb_copy_output_size(p, &ow, &oh);
    fresh.w = uint32_t(ow);
    fresh.h = uint32_t(oh);
    fresh.fmt = p.format;
    fresh.clock = g_mem.advance_clock();
    const int slot = g_copy_tracker.begin(fresh);
    if (slot < 0) return false;
    GpuCopyGl& gl = g_copy_gl[slot];
    if (!gl.tex) glGenTextures(1, &gl.tex);
    if (gl.tw != ow || gl.th != oh) {
        edit_unit_texture(kEfbUnit, gl.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.tw = ow;
        gl.th = oh;
        gl.params_valid = false;
        bind_unit_texture(kEfbUnit, g_white);
    }
    // The copy as a texture.
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.tex, 0);
    glViewport(0, 0, ow, oh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_copy_program);
    glBindVertexArray(g_present_vao);
    bind_unit_texture(kEfbUnit, g_color);
    glUniform2i(g_copy_origin_loc, p.x, kEfbHeight - 1 - p.y);
    glUniform2i(g_copy_size_loc, p.w, p.h);
    glUniform2i(g_copy_mode_loc, p.half_scale ? 1 : 0, p.format == uint8_t(EfbCopyFormat::RGBA8) ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    bind_unit_texture(kEfbUnit, g_white);
    glBindVertexArray(g_vao);
    glUseProgram(g_active);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    invalidate_shadow();  // viewport, scissor, blend, depth, cull and colour mask were changed here
    g_efb_bound = true;
    // The bytes: read back through a buffer, written when the game synchronises.
    GLuint pbo;
    if (!g_pbo_free.empty()) {
        pbo = g_pbo_free.back();
        g_pbo_free.pop_back();
    } else {
        glGenBuffers(1, &pbo);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, GLsizeiptr(size_t(p.w) * p.h * 4), nullptr, GL_STREAM_READ);
    glReadPixels(p.x, kEfbHeight - (p.y + p.h), p.w, p.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    g_pending.push_back({pbo, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), p, addr, slot, g_frames});
    update_read_guard();
    ++g_render_stats.efb_gpu_copies;
    return true;
}

static void copy_efb_to_memory(const State& s, uint32_t copy_cmd) {
    EfbCopyParams p = make_efb_copy_params(copy_cmd, s.bp[0x49], s.bp[0x4A], s.bp[0x4D], s.bp[0x4E],
                                           (s.bp[0x43] & 7) == 3);
    p.w = std::min(p.w, kEfbWidth - p.x);
    p.h = std::min(p.h, kEfbHeight - p.y);
    const uint32_t addr = (s.bp[0x4B] & 0xFFFFFF) << 5;
    if (p.w <= 0 || p.h <= 0) return;
    ++g_render_stats.efb_copies_memory;
    g_render_stats.efb_copy_pixels += size_t(p.w) * p.h;
    if (uint64_t(addr) + efb_copy_extent(p) > g_mem.ram_size) {
        std::fprintf(stderr, "[GL] EFB copy to 0x%06X (+%zu) is outside RAM, ignored\n", addr, efb_copy_extent(p));
        return;
    }
    // A filtered copy also reads the line above and below the rectangle (where the clamp bits and the EFB's edge allow), so
    // the readback covers `rows` lines and the rectangle's own are the middle ones.
    const CopyFilter filter = copy_filter_of(s, p.depth);
    int above = 0, below = 0;
    if (!filter.identity()) {
        copy_filter_margins(p, kEfbHeight, &above, &below);
        ++g_use.filtered_copies;
    }
    // GP_TRACE_COPY=1: every distinct kind of copy to memory, once (format, size, stride, scaling), and whether it stays on the GPU.
    static const bool trace_copy = std::getenv("GP_TRACE_COPY") != nullptr;
    if (trace_copy) {
        static std::vector<uint64_t> seen;
        const uint64_t sig = uint64_t(p.format) | uint64_t(p.half_scale) << 4 | uint64_t(p.y_scale) << 5 | uint64_t(p.depth) << 6 |
                             uint64_t(filter.identity()) << 7 | uint64_t(p.w) << 8 | uint64_t(p.h) << 20 | uint64_t(p.stride) << 32;
        if (std::find(seen.begin(), seen.end(), sig) == seen.end()) {
            seen.push_back(sig);
            std::fprintf(stderr, "[COPY] to memory: format %u %dx%d stride %u half=%d yscale=%d (reg %u) depth=%d filter identity=%d dest=0x%06X\n", p.format, p.w, p.h,
                         p.stride, p.half_scale, p.y_scale, p.y_scale_reg, p.depth, filter.identity(), addr);
        }
    }
    // What this copy overwrites must not be written later by an older pending copy, nor sampled from a GPU copy any more.
    const uint32_t extent = uint32_t(efb_copy_extent(p));
    flush_overlapping(addr, extent);
    g_copy_tracker.invalidate_overlapping(addr, extent);
    // The RGB565 and RGBA8 colour copies (full size or half scaled) written as the texture decoder reads them, stay on the GPU.
    if (!g_copy_sync && g_copy_program && !p.depth && (p.format == uint8_t(EfbCopyFormat::RGB565) || p.format == uint8_t(EfbCopyFormat::RGBA8)) &&
        !(p.y_scale && p.y_scale_reg != 0) && filter.identity() && copy_is_packed(p) && copy_to_gpu(p, addr, extent))
        return;
    const int rows = p.h + above + below;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const int gl_y = kEfbHeight - (p.y + p.h + below);
    // Read-back buffers kept between copies (a frame makes several): no allocation and no zero fill per copy.
    static std::vector<uint8_t> rgba;
    static std::vector<uint32_t> depth;
    if (p.depth) {
        depth.resize(size_t(p.w) * p.h);
        {
            bench::ScopedNs timer(g_render_stats.readback_ns);
            glReadPixels(p.x, gl_y, p.w, p.h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, depth.data());
        }
        for (uint32_t& d : depth) d >>= 8;
    } else {
        rgba.resize(size_t(p.w) * rows * 4);
        bench::ScopedNs timer(g_render_stats.readback_ns);
        glReadPixels(p.x, gl_y, p.w, rows, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    // GL rows run bottom to top.
    auto flip = [&](auto& v, size_t row_elems) {
        for (int a = 0, b = rows - 1; a < b; ++a, --b)
            std::swap_ranges(v.begin() + a * row_elems, v.begin() + (a + 1) * row_elems, v.begin() + b * row_elems);
    };
    if (p.depth) flip(depth, size_t(p.w)); else flip(rgba, size_t(p.w) * 4);
    uint8_t* image = rgba.data() + size_t(above) * p.w * 4;
    if (!filter.identity())
        apply_copy_filter(filter, image, p.w, p.h, above ? rgba.data() : nullptr, below ? image + size_t(p.h) * p.w * 4 : nullptr);
    encode_efb_copy(p, p.depth ? nullptr : image, p.depth ? depth.data() : nullptr, g_mem.translate(addr));
    g_mem.mark_written(addr | Memory::MAIN_RAM_BASE, uint32_t(efb_copy_extent(p)));  // the cached textures made from this memory are stale
}

void copy_efb(const CopyRequest& r, const State& s) {
    flush_batch();  // the copy reads what the queued draws make
    if (r.diag) diag_capture_efb(r, s);
    // An EFB copy changes framebuffer bindings, viewport, scissor, color and depth masks: forget what apply_state set.
    g_efb_bound = false;
    invalidate_shadow();
    static int traced = 0;
    static const bool gp_trace = std::getenv("GP_TRACE") != nullptr;
    if (traced < 6 && gp_trace) {
        ++traced;
        std::fprintf(stderr, "[GL] copy to_xfb=%d clear=%d src=(%d,%d) %dx%d bp4D=%06X bp4E=%06X window=%dx%d\n", r.to_xfb, r.clear, r.x, r.y,
                     r.w, r.h, s.bp[0x4D], s.bp[0x4E], g_window_w, g_window_h);
    }
    if (r.to_xfb) {
        static uint64_t presented = 0;
        ++g_render_stats.efb_copies_xfb;
        if (++presented % 600 == 0 && gp_trace) print_feature_use();
        // Blit the copied EFB region to the window and present.
        bench::ScopedNs timer(g_render_stats.swap_ns);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, g_window_w, g_window_h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        g_present_rect = present_rect_for(g_aspect, g_window_w, g_window_h);
        glViewport(g_present_rect.x, g_present_rect.y, g_present_rect.w, g_present_rect.h);
        const int sy0 = kEfbHeight - (r.y + r.h), sy1 = kEfbHeight - r.y;
        const CopyFilter filter = copy_filter_of(s, (s.bp[0x43] & 7) == 3);
        if (filter.identity() || !g_present_program) {
            glBlitFramebuffer(r.x, sy0, r.x + r.w, sy1, g_present_rect.x, g_present_rect.y, g_present_rect.x + g_present_rect.w,
                              g_present_rect.y + g_present_rect.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        } else {
            ++g_use.filtered_copies;
            present_filtered(r, s, filter);
        }
        write_screenshot();
        if (r.diag) {
            diag::Image& win = diag::g_bundle.window;
            win.w = g_window_w;
            win.h = g_window_h;
            win.rgb = read_window_rgb();
        }
        // The overlay (settings, frame rate) goes over the picture, after the screenshot and the diagnostic capture so
        // that those show the game alone. It restores the GL state it touches; the renderer's own state is
        // re-established by the next draw (the shadow was invalidated above).
        ++g_fps_frames;
        if (const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_fps_since).count(); dt >= 0.5) {
            g_fps = g_fps_frames / dt;
            g_fps_frames = 0;
            g_fps_since = std::chrono::steady_clock::now();
        }
        overlay_glue::draw(g_window_w, g_window_h, g_fps);
        SDL_GL_SwapWindow(g_window);
        SDL_GetWindowSizeInPixels(g_window, &g_window_w, &g_window_h);
    }
    if (!r.to_xfb) copy_efb_to_memory(s, s.bp[0x52]);
    if (r.clear) {
        const uint32_t ar = s.bp[0x4F], gb = s.bp[0x50], z = s.bp[0x51];
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(((ar >> 8) & 255) / 255.0f, ((gb >> 8) & 255) / 255.0f, (gb & 255) / 255.0f, (ar & 255) / 255.0f);
        glClearDepth((z & 0xFFFFFF) / 16777215.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
}

// Applies what the settings say to the window, the swap interval, the stick dead zone and, once the game has
// opened its audio output, the volume and the device.
static void apply_settings() {
    const settings::Values v = g_store->values();
    if (!g_applied_valid || v.video.fullscreen != g_applied.video.fullscreen) SDL_SetWindowFullscreen(g_window, v.video.fullscreen);
    // A benchmark run (GCRECOMP_BENCH) measures the renderer, not the display: it never waits for the refresh.
    if (!g_applied_valid || v.video.vsync != g_applied.video.vsync) SDL_GL_SetSwapInterval(v.video.vsync && !bench::g_timing ? 1 : 0);
    g_aspect = v.video.aspect;
    input::set_stick_deadzone(v.input.stick_deadzone);
    audio::AudioSink* sink = ai_hle_sink();
    if (sink && (sink != g_audio_target || !g_applied_valid || v.audio != g_applied.audio)) {
        sink->set_volume(v.audio.volume * v.audio.volume);  // the slider is perceptual: the gain is its square
        if (sink != g_audio_target || !g_applied_valid || v.audio.device != g_applied.audio.device) sink->set_device(v.audio.device);
        g_audio_target = sink;
    }
    g_applied = v;
    g_applied_valid = true;
}

void attach_settings(settings::Store& store, const char* manifest) {
    g_store = &store;
    overlay_glue::start(g_window, store, manifest ? manifest : "");
    apply_settings();
}

bool pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_closed = true;
        if (overlay_glue::process_event(e)) continue;  // the overlay is open: the game's input does not see it
        input::handle_event(e);
    }
    input::update();
    if (g_store) {
        const auto now = std::chrono::steady_clock::now();
        const bool visible = overlay_glue::visible();
        // The launcher may edit the files while the game runs: look again once a second, and when the overlay opens.
        if (now - g_last_reload >= std::chrono::seconds(1) || (visible && !g_overlay_was_visible)) {
            g_last_reload = now;
            g_store->reload();
            if (visible && !g_overlay_was_visible) input::reload_bindings();
        }
        g_overlay_was_visible = visible;
        // The audio sink appears when the game first uses it: apply its settings then, not only when they change.
        if (g_store->values() != g_applied || (ai_hle_sink() && ai_hle_sink() != g_audio_target)) apply_settings();
        if (overlay_glue::quit_requested()) g_closed = true;
    }
    return !g_closed;
}

}  // namespace gcrecomp::gp::renderer
