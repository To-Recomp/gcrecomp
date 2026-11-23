// SPDX-License-Identifier: MIT
#include "gp_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gcrecomp/gx/gx.h"

namespace gcrecomp::gx {
// Defined in src/gx/gx_texture.cpp.
std::vector<uint8_t> decode_texture(const uint8_t* src, uint32_t width, uint32_t height, GXTexFmt fmt);
}  // namespace gcrecomp::gx

namespace gcrecomp::gp {

size_t texture_bytes(uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t bw, bh, bytes;
    switch (fmt) {
        case 0: case 8: case 14: bw = 8; bh = 8; bytes = 32; break;   // I4, C4, CMPR
        case 1: case 2: case 9: bw = 8; bh = 4; bytes = 32; break;    // I8, IA4, C8
        case 6: bw = 4; bh = 4; bytes = 64; break;                    // RGBA8
        default: bw = 4; bh = 4; bytes = 32; break;                   // 16-bit formats, C14X2
    }
    return size_t((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

void tlut_entry_to_rgba(uint32_t e, uint32_t pal_fmt, uint8_t out[4]) {
    uint8_t r, g, b, a;
    if (pal_fmt == 0) {  // IA8: alpha in the high byte, intensity in the low byte
        a = uint8_t(e >> 8);
        r = g = b = uint8_t(e);
    } else if (pal_fmt == 1) {  // RGB565
        r = uint8_t(((e >> 11) & 31) * 255 / 31);
        g = uint8_t(((e >> 5) & 63) * 255 / 63);
        b = uint8_t((e & 31) * 255 / 31);
        a = 255;
    } else if (e & 0x8000) {  // RGB5A3, opaque: RGB555
        r = uint8_t(((e >> 10) & 31) * 255 / 31);
        g = uint8_t(((e >> 5) & 31) * 255 / 31);
        b = uint8_t((e & 31) * 255 / 31);
        a = 255;
    } else {  // RGB5A3, translucent: A3 R4 G4 B4
        a = uint8_t(((e >> 12) & 7) * 255 / 7);
        r = uint8_t(((e >> 8) & 15) * 17);
        g = uint8_t(((e >> 4) & 15) * 17);
        b = uint8_t((e & 15) * 17);
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

namespace {

// The TLUT as RGBA texels, one 32-bit word per entry (the bytes of an output pixel). An entry the TLUT memory does not hold
// stays transparent black, as a texel pointing past the end of the TLUT always was.
void build_palette(const uint8_t* tlut, size_t tlut_size, size_t base, uint32_t pal_fmt, size_t entries, uint32_t* pal) {
    for (size_t i = 0; i < entries; ++i) {
        const size_t off = base + i * 2;
        uint8_t px[4] = {0, 0, 0, 0};
        if (off + 2 <= tlut_size) tlut_entry_to_rgba(uint32_t(tlut[off] << 8) | tlut[off + 1], pal_fmt, px);
        std::memcpy(&pal[i], px, 4);
    }
}

// Walks the tiles of a palette format (`tw` x `th` texels per tile, 32 bytes each, tiles in rows left to right) and calls
// put(x, y, tile, column, row) for each texel inside the image.
template <typename Put>
void for_each_texel(uint32_t w, uint32_t h, uint32_t tw, uint32_t th, const uint8_t* src, Put put) {
    const uint32_t tiles_x = (w + tw - 1) / tw, tiles_y = (h + th - 1) / th;
    for (uint32_t ty = 0; ty < tiles_y; ++ty) {
        const uint32_t rows = std::min(th, h - ty * th);
        for (uint32_t tx = 0; tx < tiles_x; ++tx) {
            const uint8_t* tile = src + (size_t(ty) * tiles_x + tx) * 32;
            const uint32_t cols = std::min(tw, w - tx * tw);
            for (uint32_t r = 0; r < rows; ++r)
                for (uint32_t c = 0; c < cols; ++c) put(tx * tw + c, ty * th + r, tile, c, r);
        }
    }
}

}  // namespace

std::vector<uint8_t> decode_indexed(const uint8_t* tlut, size_t tlut_size, const uint8_t* src, uint32_t w,
                                    uint32_t h, uint32_t fmt, uint32_t settlut) {
    const size_t texels = size_t(w) * h;
    const size_t base = size_t(settlut & 0x3FF) * 512;
    const uint32_t pal_fmt = (settlut >> 10) & 3;
    std::vector<uint8_t> out(texels * 4, 0);
    uint8_t* const dst = out.data();
    auto store = [&](uint32_t x, uint32_t y, uint32_t rgba) { std::memcpy(dst + (size_t(y) * w + x) * 4, &rgba, 4); };

    if (fmt == kTexC14X2) {
        // One 16-bit big-endian entry per texel in 4x4 tiles; the low 14 bits are the index. The whole palette is converted
        // first when the image has about as many texels as it has entries, else only the entries that are used.
        const bool table = texels >= 8192;
        std::vector<uint32_t> pal(table ? 16384 : 0);
        if (table) build_palette(tlut, tlut_size, base, pal_fmt, pal.size(), pal.data());
        for_each_texel(w, h, 4, 4, src, [&](uint32_t x, uint32_t y, const uint8_t* tile, uint32_t c, uint32_t r) {
            const uint32_t idx = ((tile[(r * 4 + c) * 2] << 8) | tile[(r * 4 + c) * 2 + 1]) & 0x3FFF;
            if (table) {
                store(x, y, pal[idx]);
            } else {
                uint32_t px;
                build_palette(tlut, tlut_size, base + size_t(idx) * 2, pal_fmt, 1, &px);
                store(x, y, px);
            }
        });
    } else if (fmt == kTexC4) {
        // The I4 tile layout: 8x8 texels, two per byte, the high nibble first.
        uint32_t pal[16];
        build_palette(tlut, tlut_size, base, pal_fmt, 16, pal);
        for_each_texel(w, h, 8, 8, src, [&](uint32_t x, uint32_t y, const uint8_t* tile, uint32_t c, uint32_t r) {
            const uint8_t byte = tile[r * 4 + c / 2];
            store(x, y, pal[(c & 1) ? byte & 15 : byte >> 4]);
        });
    } else {
        // C8, the I8 tile layout: 8x4 texels, one byte each.
        uint32_t pal[256];
        build_palette(tlut, tlut_size, base, pal_fmt, 256, pal);
        for_each_texel(w, h, 8, 4, src, [&](uint32_t x, uint32_t y, const uint8_t* tile, uint32_t c, uint32_t r) {
            store(x, y, pal[tile[r * 8 + c]]);
        });
    }
    return out;
}

MinFilter min_filter_of(uint32_t mode0) {
    switch ((mode0 >> 5) & 7) {
        case 1: return MinFilter::NearestMipNearest;
        case 2: return MinFilter::NearestMipLinear;
        case 4: case 7: return MinFilter::Linear;
        case 5: return MinFilter::LinearMipNearest;
        case 6: return MinFilter::LinearMipLinear;
        default: return MinFilter::Nearest;
    }
}

uint32_t mip_level_count(uint32_t w, uint32_t h, uint32_t mode0, uint32_t mode1) {
    const MinFilter f = min_filter_of(mode0);
    if (f == MinFilter::Nearest || f == MinFilter::Linear) return 1;
    uint32_t max_levels = 1;
    for (uint32_t d = w > h ? w : h; d > 1; d >>= 1) ++max_levels;
    const uint32_t max_lod = (mode1 >> 8) & 0xFF;               // 4.4 fixed point
    const uint32_t wanted = (max_lod + 15) / 16 + 1;           // round the fraction up
    return wanted < max_levels ? wanted : max_levels;
}

MinFilter effective_min_filter(uint32_t mode0, uint32_t levels) {
    const MinFilter f = min_filter_of(mode0);
    if (levels > 1) return f;
    return (f == MinFilter::Nearest || f == MinFilter::NearestMipNearest || f == MinFilter::NearestMipLinear)
               ? MinFilter::Nearest
               : MinFilter::Linear;
}

size_t mip_level_offset(uint32_t w, uint32_t h, uint32_t fmt, uint32_t level) {
    size_t off = 0;
    for (uint32_t l = 0; l < level; ++l) off += texture_bytes(mip_dim(w, l), mip_dim(h, l), fmt);
    return off;
}

size_t mip_chain_bytes(uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels) {
    return mip_level_offset(w, h, fmt, levels);
}

float lod_bias_of(uint32_t mode0) { return float(int8_t((mode0 >> 9) & 0xFF)) / 32.0f; }

namespace {
// A and C of the fog registers: the top 20 bits of an IEEE-754 single.
float fog_float(uint32_t field) {
    const uint32_t bits = (field & 0xFFFFF) << 12;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
}  // namespace

FogParams decode_fog(const uint32_t* bp) {
    FogParams p;
    p.a = fog_float(bp[0xEE]);
    p.b_mag = bp[0xEF] & 0xFFFFFF;
    p.b_shift = bp[0xF0] & 0x1F;
    p.c = fog_float(bp[0xF1]);
    p.ortho = (bp[0xF1] >> 20) & 1;
    p.type = (bp[0xF1] >> 21) & 7;
    p.color[0] = float((bp[0xF2] >> 16) & 0xFF) / 255.0f;
    p.color[1] = float((bp[0xF2] >> 8) & 0xFF) / 255.0f;
    p.color[2] = float(bp[0xF2] & 0xFF) / 255.0f;
    return p;
}

float fog_factor(const FogParams& p, uint32_t z24) {
    if (p.type == kFogOff) return 0.0f;
    float ze;
    if (p.ortho) {
        ze = p.a * float(z24) / 16777216.0f;
    } else {
        // ze = A / (B - (Zs >> B_SHIFT)), with A scaled by 2^24 for the integer depth.
        const int32_t denom = int32_t(p.b_mag) - int32_t(z24 >> p.b_shift);
        ze = denom != 0 ? p.a * 16777216.0f / float(denom) : 1e30f;
    }
    float f = ze - p.c;
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    switch (p.type) {
        case kFogExp: return 1.0f - std::exp2(-8.0f * f);
        case kFogExp2: return 1.0f - std::exp2(-8.0f * f * f);
        case kFogBackwardsExp: return std::exp2(-8.0f * (1.0f - f));
        case kFogBackwardsExp2: f = 1.0f - f; return std::exp2(-8.0f * f * f);
        default: return f;  // linear
    }
}

uint32_t indirect_tev_stages(const uint32_t* bp) {
    if (((bp[0x00] >> 16) & 7) == 0) return 0;
    const uint32_t nstages = ((bp[0x00] >> 10) & 0xF) + 1;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < nstages; ++i)
        if (bp[0x10 + i] & 0x1FFFFF) mask |= 1u << i;
    return mask;
}

namespace {
enum class Tri { Never, Always, Unknown };

// What one alpha comparison (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) does for every alpha
// value 0..255 against `ref`.
Tri alpha_compare_result(uint32_t func, uint32_t ref) {
    switch (func) {
        case 0: return Tri::Never;
        case 1: return ref == 0 ? Tri::Never : Tri::Unknown;
        case 3: return ref == 255 ? Tri::Always : Tri::Unknown;
        case 4: return ref == 255 ? Tri::Never : Tri::Unknown;
        case 6: return ref == 0 ? Tri::Always : Tri::Unknown;
        case 7: return Tri::Always;
        default: return Tri::Unknown;
    }
}
}  // namespace

bool alpha_test_can_reject(uint32_t ac) {
    const Tri a = alpha_compare_result((ac >> 16) & 7, ac & 0xFF);
    const Tri b = alpha_compare_result((ac >> 19) & 7, (ac >> 8) & 0xFF);
    Tri r = Tri::Unknown;
    switch ((ac >> 22) & 3) {
        case 0:  // AND
            if (a == Tri::Never || b == Tri::Never) r = Tri::Never;
            else if (a == Tri::Always && b == Tri::Always) r = Tri::Always;
            break;
        case 1:  // OR
            if (a == Tri::Always || b == Tri::Always) r = Tri::Always;
            else if (a == Tri::Never && b == Tri::Never) r = Tri::Never;
            break;
        default:  // XOR (2), XNOR (3)
            if (a != Tri::Unknown && b != Tri::Unknown) {
                const bool same = a == b;
                r = ((((ac >> 22) & 3) == 3) == same) ? Tri::Always : Tri::Never;
            }
            break;
    }
    return r != Tri::Always;
}

bool early_z_writes_rejected_pixels(const uint32_t* bp) {
    const bool early = (bp[0x43] >> 6) & 1;
    const bool depth_test = bp[0x40] & 1, depth_write = (bp[0x40] >> 4) & 1;
    return early && depth_test && depth_write && alpha_test_can_reject(bp[0xF3]);
}

BlendState decode_blend(uint32_t cmode0, uint32_t cmode1) {
    BlendState b;
    b.blend = cmode0 & 1;
    b.subtract = (cmode0 >> 11) & 1;
    b.logic_mode = (cmode0 >> 12) & 15;
    // The hardware ignores the logic op while blending is on; the copy op is a plain write.
    b.logic_op = ((cmode0 >> 1) & 1) && !b.blend && b.logic_mode != 3;
    b.color_update = (cmode0 >> 3) & 1;
    b.alpha_update = (cmode0 >> 4) & 1;
    b.dst_factor = (cmode0 >> 5) & 7;
    b.src_factor = (cmode0 >> 8) & 7;
    b.const_alpha = (cmode1 >> 8) & 1;
    b.const_alpha_value = uint8_t(cmode1);
    return b;
}

}  // namespace gcrecomp::gp
