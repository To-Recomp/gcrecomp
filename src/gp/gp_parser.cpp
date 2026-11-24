// SPDX-License-Identifier: MIT
//
// GP command stream parser and vertex decoder: parses what the SDK's GX functions write to the write-gather pipe, keeps the
// CP/XF/BP register state, decodes vertices and hands primitives to the renderer.
//
// Register layouts follow the public GameCube documentation (YAGCD) and Dolphin's
// documentation of the same hardware. No Dolphin code is used.
#include "gcrecomp/gp/gp.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "gcrecomp/bench.h"
#include "gcrecomp/runtime.h"
#include "gp_internal.h"
#include "diag.h"
#include "efb_copy.h"
#include "gp_decode.h"
#include "gp_record.h"
#include "lighting.h"

namespace gcrecomp::gp {

float State::xf_float(uint32_t addr) const {
    float f;
    std::memcpy(&f, &xf[addr], 4);
    return f;
}

Stats g_stats;
RenderStats g_render_stats;
State g_state;
uint64_t g_frames = 0;
std::atomic<long> g_trace_request{-1};  // request_trace_frame(): the frame to trace, overrides GP_TRACE_FRAME
bool g_graphics = false;

namespace {

// GP_TRACE: asked once, not per command (getenv is a linear scan of the environment).
bool gp_trace_enabled() {
    static const bool on = std::getenv("GP_TRACE") != nullptr;
    return on;
}

// True while the frame being drawn is the one GP_TRACE_FRAME or request_trace_frame() names.
bool tracing_frame() {
    static const long env_trace_frame = std::getenv("GP_TRACE_FRAME") ? std::atol(std::getenv("GP_TRACE_FRAME")) : -1;
    const long requested = g_trace_request.load(std::memory_order_relaxed);
    const long trace_frame = requested >= 0 ? requested : env_trace_frame;
    return trace_frame >= 0 && uint64_t(trace_frame) == g_frames;
}

// One line of the trace of the frame being drawn: GP_TRACE_FRAME sends it to stderr, a diagnostic bundle (diag.h) keeps it
// for diag.txt. `to_stderr` and `to_diag` say which of the two are on for this frame.
void trace_emit(bool to_stderr, bool to_diag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
void trace_emit(bool to_stderr, bool to_diag, const char* fmt, ...) {
    std::string line;
    va_list ap;
    va_start(ap, fmt);
    diag::vappendf(line, fmt, ap);
    va_end(ap);
    if (to_stderr) std::fputs(line.c_str(), stderr);
    if (to_diag) diag::g_bundle.trace += line;
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

enum AttrMode : uint32_t { kNone = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3 };

// Where each vertex attribute comes from, and its size inside the vertex, for one VCD/VAT pair.
struct Layout {
    bool pmidx = false;
    bool texmidx[8] = {};
    AttrMode pos = kNone, nrm = kNone, col[2] = {kNone, kNone}, tex[8] = {};
    uint32_t pos_cnt = 3, pos_fmt = 4, pos_shift = 0;
    uint32_t nrm_cnt = 3, nrm_fmt = 4;
    uint32_t col_cnt[2] = {}, col_fmt[2] = {};
    uint32_t tex_cnt[8] = {}, tex_fmt[8] = {}, tex_shift[8] = {};
    uint32_t size = 0;
    uint32_t fmt = 0;
};

uint32_t comp_size(uint32_t fmt) {
    switch (fmt) {
        case 0: case 1: return 1;
        case 2: case 3: return 2;
        default: return 4;
    }
}

uint32_t color_size(uint32_t fmt) {
    switch (fmt) {
        case 0: return 2;  // RGB565
        case 1: return 3;  // RGB888
        case 2: return 4;  // RGB888x
        case 3: return 2;  // RGBA4444
        case 4: return 3;  // RGBA6666
        default: return 4; // RGBA8888
    }
}

uint32_t attr_size(AttrMode m, uint32_t direct_size) {
    switch (m) {
        case kDirect: return direct_size;
        case kIndex8: return 1;
        case kIndex16: return 2;
        default: return 0;
    }
}

Layout make_layout(const State& s, uint32_t fmt) {
    Layout l;
    l.fmt = fmt;
    const uint32_t lo = s.vcd_lo, hi = s.vcd_hi;
    const uint32_t a = s.vat[0][fmt], b = s.vat[1][fmt], c = s.vat[2][fmt];
    l.pmidx = lo & 1;
    for (int i = 0; i < 8; ++i) l.texmidx[i] = (lo >> (1 + i)) & 1;
    l.pos = AttrMode((lo >> 9) & 3);
    l.nrm = AttrMode((lo >> 11) & 3);
    l.col[0] = AttrMode((lo >> 13) & 3);
    l.col[1] = AttrMode((lo >> 15) & 3);
    for (int i = 0; i < 8; ++i) l.tex[i] = AttrMode((hi >> (2 * i)) & 3);

    l.pos_cnt = (a & 1) ? 3 : 2;
    l.pos_fmt = (a >> 1) & 7;
    l.pos_shift = (a >> 4) & 0x1F;
    l.nrm_cnt = ((a >> 9) & 1) ? 9 : 3;
    l.nrm_fmt = (a >> 10) & 7;
    l.col_cnt[0] = (a >> 13) & 1;
    l.col_fmt[0] = (a >> 14) & 7;
    l.col_cnt[1] = (a >> 17) & 1;
    l.col_fmt[1] = (a >> 18) & 7;
    l.tex_cnt[0] = (a >> 21) & 1;
    l.tex_fmt[0] = (a >> 22) & 7;
    l.tex_shift[0] = (a >> 25) & 0x1F;
    l.tex_cnt[1] = b & 1;
    l.tex_fmt[1] = (b >> 1) & 7;
    l.tex_shift[1] = (b >> 4) & 0x1F;
    l.tex_cnt[2] = (b >> 9) & 1;
    l.tex_fmt[2] = (b >> 10) & 7;
    l.tex_shift[2] = (b >> 13) & 0x1F;
    l.tex_cnt[3] = (b >> 18) & 1;
    l.tex_fmt[3] = (b >> 19) & 7;
    l.tex_shift[3] = (b >> 22) & 0x1F;
    l.tex_cnt[4] = (b >> 27) & 1;
    l.tex_fmt[4] = (b >> 28) & 7;
    l.tex_shift[4] = ((b >> 31) & 1) | ((c & 0xF) << 1);
    l.tex_cnt[5] = (c >> 4) & 1;
    l.tex_fmt[5] = (c >> 5) & 7;
    l.tex_shift[5] = (c >> 8) & 0x1F;
    l.tex_cnt[6] = (c >> 13) & 1;
    l.tex_fmt[6] = (c >> 14) & 7;
    l.tex_shift[6] = (c >> 17) & 0x1F;
    l.tex_cnt[7] = (c >> 22) & 1;
    l.tex_fmt[7] = (c >> 23) & 7;
    l.tex_shift[7] = (c >> 26) & 0x1F;

    uint32_t size = l.pmidx ? 1 : 0;
    for (int i = 0; i < 8; ++i) size += l.texmidx[i] ? 1 : 0;
    size += attr_size(l.pos, l.pos_cnt * comp_size(l.pos_fmt));
    size += attr_size(l.nrm, l.nrm_cnt * comp_size(l.nrm_fmt));
    for (int i = 0; i < 2; ++i) size += attr_size(l.col[i], color_size(l.col_fmt[i]));
    for (int i = 0; i < 8; ++i) size += attr_size(l.tex[i], (l.tex_cnt[i] + 1) * comp_size(l.tex_fmt[i]));
    l.size = size;
    return l;
}

// 2^-n for n = 0..31, exact: v * 2^-n equals v / 2^n.
struct InvPow2 {
    float v[32];
    constexpr InvPow2() : v() {
        float f = 1.0f;
        for (int i = 0; i < 32; ++i, f *= 0.5f) v[i] = f;
    }
    constexpr float operator[](uint32_t i) const { return v[i]; }
};
constexpr InvPow2 kInvPow2;

float read_component(const uint8_t* p, uint32_t fmt, uint32_t shift) {
    float v;
    switch (fmt) {
        case 0: v = float(p[0]); break;
        case 1: v = float(int8_t(p[0])); break;
        case 2: v = float(be16(p)); break;
        case 3: v = float(int16_t(be16(p))); break;
        default: {
            uint32_t bits = be32(p);
            std::memcpy(&v, &bits, 4);
            return v;
        }
    }
    return shift ? v * kInvPow2[shift & 31] : v;
}

void read_color(const uint8_t* p, uint32_t fmt, float out[4]) {
    uint32_t r = 255, g = 255, b = 255, a = 255;
    switch (fmt) {
        case 0: {  // RGB565
            const uint32_t v = be16(p);
            r = ((v >> 11) & 31) * 255 / 31;
            g = ((v >> 5) & 63) * 255 / 63;
            b = (v & 31) * 255 / 31;
            break;
        }
        case 1: case 2: r = p[0]; g = p[1]; b = p[2]; break;
        case 3: {  // RGBA4444
            const uint32_t v = be16(p);
            r = ((v >> 12) & 15) * 17;
            g = ((v >> 8) & 15) * 17;
            b = ((v >> 4) & 15) * 17;
            a = (v & 15) * 17;
            break;
        }
        case 4: {  // RGBA6666
            const uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            r = ((v >> 18) & 63) * 255 / 63;
            g = ((v >> 12) & 63) * 255 / 63;
            b = ((v >> 6) & 63) * 255 / 63;
            a = (v & 63) * 255 / 63;
            break;
        }
        default: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

void unpack_rgba8(uint32_t packed, float out[4]) {
    out[0] = ((packed >> 24) & 255) / 255.0f;
    out[1] = ((packed >> 16) & 255) / 255.0f;
    out[2] = ((packed >> 8) & 255) / 255.0f;
    out[3] = (packed & 255) / 255.0f;
}

class Parser {
public:
    bool idle() const { return buf_.empty(); }
    void feed(const uint8_t* data, size_t n) {
        buf_.insert(buf_.end(), data, data + n);
        for (size_t i = 0; i < n; ++i) raw_[raw_pos_++ & 255] = data[i];
        // A vertex command or XF load arrives a store at a time: do not decode its layout again until the bytes
        // the last attempt said it needs are there.
        if (buf_.size() < want_) return;
        size_t pos = 0;
        while (pos < buf_.size()) {
            const size_t used = parse_one(buf_.data() + pos, buf_.size() - pos);
            if (used == 0) break;
            want_ = 0;
            remember(buf_[pos], used);
            pos += used;
        }
        if (pos) buf_.erase(buf_.begin(), buf_.begin() + pos);
    }

private:
    std::vector<uint8_t> buf_;
    size_t want_ = 0;  // bytes the command at the head of buf_ needs (set by an incomplete parse_one), 0 = unknown
    std::vector<float> vertices_;
    int dl_depth_ = 0;

    // The layout of the last draw, reused while the registers it is made from are unchanged (a draw command follows
    // thousands of others with the same vertex format).
    Layout layout_cache_;
    uint32_t layout_key_[5] = {};
    bool layout_valid_ = false;
    const Layout& cached_layout(uint32_t fmt) {
        const State& st = g_state;
        const uint32_t key[5] = {st.vcd_lo, st.vcd_hi, st.vat[0][fmt], st.vat[1][fmt], st.vat[2][fmt]};
        if (!layout_valid_ || layout_cache_.fmt != fmt || std::memcmp(key, layout_key_, sizeof key) != 0) {
            layout_cache_ = make_layout(st, fmt);
            std::memcpy(layout_key_, key, sizeof key);
            layout_valid_ = true;
        }
        return layout_cache_;
    }

    // Returns the number of bytes consumed, or 0 when the command is not complete yet.
    size_t parse_one(const uint8_t* p, size_t n) {
        const uint8_t cmd = p[0];
        switch (cmd) {
            case 0x00: ++g_stats.commands; return 1;
            case 0x44: case 0x48: ++g_stats.commands; return 1;  // metric, invalidate vertex cache
            case 0x08:
                if (n < 6) return 0;
                ++g_stats.commands;
                cp_write(p[1], be32(p + 2));
                return 6;
            case 0x10: {
                if (n < 5) return 0;
                const uint32_t header = be32(p + 1);
                const uint32_t count = (header >> 16) + 1, addr = header & 0xFFFF;
                if (count > 1000 && !huge_reported_) {
                    huge_reported_ = true;
                    dump_raw("huge XF load header");
                }
                if (n < 5 + 4 * size_t(count)) {
                    want_ = 5 + 4 * size_t(count);
                    return 0;
                }
                ++g_stats.commands;
                if (count > 64 && gp_trace_enabled()) {
                    std::fprintf(stderr, "[GP] XF load addr=0x%04X count=%u; bytes:", addr, count);
                    for (int i = 0; i < 24; ++i) std::fprintf(stderr, " %02X", p[i]);
                    std::fprintf(stderr, "\n");
                }
                for (uint32_t i = 0; i < count; ++i) {
                    if (addr + i < 0x1100) g_state.xf[addr + i] = be32(p + 5 + 4 * i);
                }
                g_stats.xf_writes += count;
                if (addr < 0x1020 && addr + count > 0x101A) ++g_state.gen;  // viewport
                if ((addr == 0x101A || addr == 0x1020) && trace_xf_ < 10 && gp_trace_enabled()) {
                    ++trace_xf_;
                    std::fprintf(stderr, "[GP] XF 0x%04X count=%u lr=0x%08X:", addr, count, g_ctx.lr);
                    for (uint32_t i = 0; i < count && i < 8; ++i) {
                        const uint32_t w = be32(p + 5 + 4 * i);
                        float f;
                        std::memcpy(&f, &w, 4);
                        std::fprintf(stderr, " %08X(%g)", w, f);
                    }
                    std::fprintf(stderr, "\n");
                }
                return 5 + 4 * size_t(count);
            }
            case 0x20: case 0x28: case 0x30: case 0x38: {
                if (n < 5) return 0;
                ++g_stats.commands;
                xf_indexed_load((cmd >> 3) & 3, be32(p + 1));
                return 5;
            }
            case 0x40: {
                if (n < 9) return 0;
                ++g_stats.commands;
                call_display_list(be32(p + 1), be32(p + 5));
                return 9;
            }
            case 0x61:
                if (n < 5) return 0;
                ++g_stats.commands;
                bp_write(be32(p + 1));
                return 5;
            default: break;
        }
        if (cmd >= 0x80) {
            if (n < 3) return 0;
            const uint32_t count = be16(p + 1);
            const Layout& layout = cached_layout(cmd & 7);
            const size_t total = 3 + size_t(count) * layout.size;
            if (n < total) {
                want_ = total;
                return 0;
            }
            ++g_stats.commands;
            if (trace_draws_ < 12 && gp_trace_enabled()) {
                ++trace_draws_;
                std::fprintf(stderr, "[GP] draw cmd=0x%02X prim=%u count=%u fmt=%u vsize=%u vcd=%06X/%04X vat=%08X/%08X/%08X\n", cmd,
                             (cmd >> 3) & 7, count, cmd & 7, layout.size, g_state.vcd_lo, g_state.vcd_hi, g_state.vat[0][cmd & 7],
                             g_state.vat[1][cmd & 7], g_state.vat[2][cmd & 7]);
            }
            draw(Primitive((cmd >> 3) & 7), count, layout, p + 3);
            return total;
        }
        ++g_stats.unknown_commands;
        if (g_stats.unknown_commands == 1) {
            dump_raw("first unknown command");
            std::fprintf(stderr, "[GP] unknown command 0x%02X; previous commands (opcode:bytes), oldest first:", cmd);
            for (int i = 0; i < 24; ++i) {
                const auto& h = history_[(history_pos_ + i) % 24];
                std::fprintf(stderr, " %02X:%zu", h.first, h.second);
            }
            std::fprintf(stderr, "\n");
        }
        if (g_stats.unknown_commands <= 8) std::fprintf(stderr, "[GP] unknown command 0x%02X\n", cmd);
        return 1;
    }

    bool huge_reported_ = false;
    int trace_xf_ = 0;
    uint8_t raw_[256] = {};
    size_t raw_pos_ = 0;
    void dump_raw(const char* why) {
        std::fprintf(stderr, "[GP] %s; last 160 stream bytes:", why);
        for (size_t i = raw_pos_ - 160; i < raw_pos_; ++i) {
            if ((i - (raw_pos_ - 160)) % 32 == 0) std::fprintf(stderr, "\n   ");
            std::fprintf(stderr, " %02X", raw_[i & 255]);
        }
        std::fprintf(stderr, "\n");
    }
    std::pair<uint8_t, size_t> history_[24] = {};
    int history_pos_ = 0;
    int trace_draws_ = 0;
    void remember(uint8_t cmd, size_t size) {
        history_[history_pos_] = {cmd, size};
        history_pos_ = (history_pos_ + 1) % 24;
    }

    void call_display_list(uint32_t addr, uint32_t size) {
        if (dl_depth_ >= 4) return;
        const uint8_t* mem = g_mem.read_ptr(addr);
        ++dl_depth_;
        size_t pos = 0;
        while (pos < size) {
            const size_t used = parse_one(mem + pos, size - pos);
            if (used == 0) break;
            pos += used;
        }
        --dl_depth_;
    }

    void xf_indexed_load(uint32_t which, uint32_t header) {
        // which: 0 = A (position matrices), 1 = B (normal matrices), 2 = C (post matrices), 3 = D (lights)
        const uint32_t index = header >> 16, len = ((header >> 12) & 0xF) + 1, addr = header & 0xFFF;
        const uint32_t base = g_state.array_base[12 + which], stride = g_state.array_stride[12 + which];
        const uint8_t* src = g_mem.read_ptr(base + index * stride);
        for (uint32_t i = 0; i < len; ++i) {
            if (addr + i < 0x1100) g_state.xf[addr + i] = be32(src + 4 * i);
        }
        g_stats.xf_writes += len;
        if (addr < 0x1020 && addr + len > 0x101A) ++g_state.gen;
    }

    void cp_write(uint8_t reg, uint32_t value) {
        ++g_stats.cp_writes;
        State& s = g_state;
        if (reg == 0x30) s.matidx_a = value;
        else if (reg == 0x40) s.matidx_b = value;
        else if (reg == 0x50) s.vcd_lo = value;
        else if (reg == 0x60) s.vcd_hi = value;
        else if (reg >= 0x70 && reg < 0x78) s.vat[0][reg - 0x70] = value;
        else if (reg >= 0x80 && reg < 0x88) s.vat[1][reg - 0x80] = value;
        else if (reg >= 0x90 && reg < 0x98) s.vat[2][reg - 0x90] = value;
        else if (reg >= 0xA0 && reg < 0xB0) s.array_base[reg - 0xA0] = value & 0x03FFFFFF;
        else if (reg >= 0xB0) s.array_stride[reg - 0xB0] = value & 0xFF;
    }

    void bp_write(uint32_t word) {
        ++g_stats.bp_writes;
        const uint32_t reg = word >> 24;
        const uint32_t data = word & 0xFFFFFF;
        State& s = g_state;
        if (reg == 0xFE) {
            s.bp[0xFE] = data;
            return;
        }
        const uint32_t mask = s.bp[0xFE] & 0xFFFFFF;
        const uint32_t before = s.bp[reg];
        s.bp[reg] = (s.bp[reg] & ~mask) | (data & mask);
        s.bp[0xFE] = 0xFFFFFF;
        const uint32_t value = s.bp[reg];
        if (value != before) ++s.gen;

        if (reg >= 0xE0 && reg <= 0xE7) {
            const uint32_t slot = (reg - 0xE0) / 2;
            const bool konst = (value >> 23) & 1;
            auto s10 = [](uint32_t v) {
                v &= 0x7FF;
                return (v & 0x400 ? int(v) - 0x800 : int(v)) / 255.0f;
            };
            float* dst = konst ? s.tev_konst[slot] : s.tev_reg[slot];
            if ((reg & 1) == 0) {
                dst[0] = s10(value);
                dst[3] = s10(value >> 12);
            } else {
                dst[2] = s10(value);
                dst[1] = s10(value >> 12);
            }
        }
        if (reg == 0x65) {
            // LOADTLUT1: copy `count` 32-byte lines from the address given by LOADTLUT0 into TLUT memory.
            const uint32_t src = (s.bp[0x64] & 0x1FFFFF) << 5;
            const uint32_t offset = (value & 0x3FF) * 512;
            const uint32_t bytes = ((value >> 10) & 0x7FF) * 32;
            if (offset + bytes <= sizeof(s.tlut)) {
                std::memcpy(s.tlut + offset, g_mem.read_ptr(src), bytes);
                ++s.tlut_gen;
            ++s.gen;
            }
        }
        if (reg == 0x47 || reg == 0x48) {
            // Draw-sync token: GXReadDrawSync reads it from the PE register at 0xCC00100E.
            uint8_t* pe = g_mem.hw_regs + (0xCC00100E - Memory::HW_REG_BASE);
            pe[0] = uint8_t(value >> 8);
            pe[1] = uint8_t(value);
            if (g_graphics) renderer::flush_copies();
        } else if (reg == 0x45) {
            if (g_graphics) renderer::flush_copies();  // GXSetDrawDone
        } else if (reg == 0x52) {
            efb_copy(value);
        }
    }

    // The EFB copy and the registers it reads, for GP_TRACE_FRAME and the diagnostic bundle: the copy command's
    // fields (BP 0x52), the source rectangle, destination, stride, Y scale, the vertical filter taps (BP 0x53/0x54),
    // the pixel format and the clear values.
    std::string copy_text(const CopyRequest& r, uint32_t value) const {
        const State& s = g_state;
        std::string t;
        using diag::appendf;
        appendf(t, "[COPY] to_xfb=%d clear=%d src=(%d,%d) %dx%d cmd=%06X dest=0x%06X stride=%u pe_control=%06X (pixel format %u, z format %u) clear AR=%06X GB=%06X z=%06X\n",
                r.to_xfb, r.clear, r.x, r.y, r.w, r.h, value, (s.bp[0x4B] & 0xFFFFFF) << 5, s.bp[0x4D] & 0x3FF, s.bp[0x43], s.bp[0x43] & 7,
                (s.bp[0x43] >> 3) & 7, s.bp[0x4F], s.bp[0x50], s.bp[0x51]);
        const uint32_t fmt = ((value >> 4) & 7) | (((value >> 3) & 1) << 3);
        const bool depth = (s.bp[0x43] & 7) == 3;
        const uint32_t yreg = s.bp[0x4E] & 0x1FF;
        int ow = r.w, oh = r.h;
        if ((value >> 9) & 1) ow = std::max(ow / 2, 1), oh = std::max(oh / 2, 1);
        if (((value >> 10) & 1) && yreg) oh = std::max(int((uint64_t(oh) * 256 + yreg / 2) / yreg), 1);
        appendf(t, "[COPY]   %s format=%X%s clamp(top=%u bottom=%u) yuv=%u gamma=%u half_scale/mipmap=%u y_scale=%u (bp4E=%03X: x%g) frame_to_field=%u intensity=%u bit16=%u -> %dx%d texels\n",
                depth ? "depth" : "colour", fmt, ((value >> 15) & 1) ? " (intensity: luma)" : "", value & 1, (value >> 1) & 1, (value >> 2) & 1,
                (value >> 7) & 3, (value >> 9) & 1, (value >> 10) & 1, yreg, yreg ? 256.0 / yreg : 0.0, (value >> 12) & 3, (value >> 15) & 1,
                (value >> 16) & 1, ow, oh);
        const uint32_t f53 = s.bp[0x53], f54 = s.bp[0x54];
        const int c[7] = {int(f53 & 63), int((f53 >> 6) & 63), int((f53 >> 12) & 63), int((f53 >> 18) & 63), int(f54 & 63), int((f54 >> 6) & 63), int((f54 >> 12) & 63)};
        const CopyFilter flt = make_copy_filter(f53, f54);
        appendf(t, "[COPY]   vertical filter bp53=%06X bp54=%06X taps %d %d %d %d %d %d %d: lines above/this/below = %d/%d/%d of 64 (%s; there is no enable bit, the SDK always writes the taps; 0 0 21 22 21 0 0 = unfiltered, all zero = never written = unfiltered)\n",
                f53, f54, c[0], c[1], c[2], c[3], c[4], c[5], c[6], flt.above, flt.centre, flt.below, flt.identity() ? "identity" : "ACTIVE");
        appendf(t, "[COPY]   bp47/48 token=%06X sample pattern bp01..04=%06X %06X %06X %06X genmode=%06X (msaa %u)\n", s.bp[0x48], s.bp[1], s.bp[2], s.bp[3], s.bp[4], s.bp[0],
                (s.bp[0] >> 9) & 1);
        return t;
    }

    void efb_copy(uint32_t value) {
        const State& s = g_state;
        CopyRequest r;
        r.to_xfb = (value >> 14) & 1;
        r.clear = (value >> 11) & 1;
        const uint32_t tl = s.bp[0x49], br = s.bp[0x4A];
        r.x = int(tl & 0x3FF);
        r.y = int((tl >> 10) & 0x3FF);
        r.w = int(br & 0x3FF) + 1;
        r.h = int((br >> 10) & 0x3FF) + 1;
        // A diagnostic bundle (diag.h) covers one whole frame: it starts after the XFB copy that follows the request and
        // ends with the next one, which is recorded as well. The copy that starts it belongs to the frame before.
        const diag::Trigger::Edge edge = r.to_xfb ? diag::g_trigger.frame_presented() : diag::Trigger::Edge::None;
        if (edge == diag::Trigger::Edge::Begin) diag::g_bundle.reset();
        r.diag = diag::g_trigger.capturing() && edge != diag::Trigger::Edge::Begin;
        const bool trace_stderr = tracing_frame();
        if (trace_stderr || r.diag) {
            const std::string text = copy_text(r, value);
            if (trace_stderr) std::fputs(text.c_str(), stderr);
            if (r.diag) {
                diag::Bundle& b = diag::g_bundle;
                b.trace += text;
                b.count(r.to_xfb ? "efb copies to the XFB" : "efb copies to a texture");
                diag::note_copy_features(b, value, s.bp[0x53], s.bp[0x54], (s.bp[0x43] & 7) == 3);
            }
        }
        if (g_graphics) renderer::copy_efb(r, g_state);
        if (r.diag && edge == diag::Trigger::Edge::Finish) diag::finish(g_frames);
        if (r.to_xfb) {
            ++g_frames;
            record::on_frame_end();
        }
    }

    // The register state a draw runs with, one [STATE] line per unit: what GP_TRACE_STATE prints and a diagnostic bundle
    // records for every draw whose state differs from the previous one's.
    std::string state_text() const {
        const State& s = g_state;
        std::string t;
        using diag::appendf;
        const uint32_t zm = s.bp[0x40], cm0 = s.bp[0x41], cm1 = s.bp[0x42], ac = s.bp[0xF3];
        appendf(t, "[STATE] gen=%06X zmode=%02X blend=%04X/%04X acmp=%06X scissor=%06X/%06X cullmode=%u\n", s.bp[0], zm, cm0, cm1, ac, s.bp[0x20], s.bp[0x21], (s.bp[0] >> 14) & 3);
        const BlendState bl = decode_blend(cm0, cm1);
        appendf(t, "[STATE]   z: test=%u func=%u write=%u early=%u | blend=%u logic=%u(op %u) src=%u dst=%u subtract=%u colour_update=%u alpha_update=%u dither=%u dst_alpha=%u(%u) | alpha test: c0=%u ref0=%u logic=%u c1=%u ref1=%u\n",
                zm & 1, (zm >> 1) & 7, (zm >> 4) & 1, (s.bp[0x43] >> 6) & 1, bl.blend, bl.logic_op, bl.logic_mode, bl.src_factor, bl.dst_factor, bl.subtract, bl.color_update,
                bl.alpha_update, (cm0 >> 2) & 1, bl.const_alpha, bl.const_alpha_value, (ac >> 16) & 7, ac & 0xFF, (ac >> 22) & 3, (ac >> 19) & 7, (ac >> 8) & 0xFF);
        const uint32_t stl = s.bp[0x20], str = s.bp[0x21];
        appendf(t, "[STATE]   viewport wd=%g ht=%g xo=%g yo=%g zr=%g far=%g efb_offset=%06X | scissor (%d,%d)-(%d,%d) | projection %g %g %g %g %g %g %s\n",
                s.xf_float(0x101A), s.xf_float(0x101B), s.xf_float(0x101D), s.xf_float(0x101E), s.xf_float(0x101C), s.xf_float(0x101F), s.bp[0x59],
                int((stl >> 12) & 0x7FF) - 342, int(stl & 0x7FF) - 342, int((str >> 12) & 0x7FF) - 342, int(str & 0x7FF) - 342, s.xf_float(0x1020), s.xf_float(0x1021),
                s.xf_float(0x1022), s.xf_float(0x1023), s.xf_float(0x1024), s.xf_float(0x1025), s.xf[0x1026] ? "orthographic" : "perspective");
        static const char* const cin[16] = {"CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2", "TEXC", "TEXA", "RASC", "RASA", "1", "0.5", "KONST", "0"};
        static const char* const ain[8] = {"APREV", "A0", "A1", "A2", "TEXA", "RASA", "KONST", "0"};
        static const char* const bias[4] = {"+0", "+0.5", "-0.5", "COMPARE"};
        static const char* const scale[4] = {"x1", "x2", "x4", "x0.5"};
        static const char* const dest[4] = {"PREV", "C0", "C1", "C2"};
        const int nst = int(((s.bp[0] >> 10) & 0xF) + 1);
        bool used[8] = {};
        for (int i = 0; i < nst; ++i) {
            const uint32_t ce = s.bp[0xC0 + 2 * i], ae = s.bp[0xC1 + 2 * i], tref = s.bp[0x28 + i / 2], ksel = s.bp[0xF6 + i / 2];
            appendf(t, "[STATE] stage%d color=%06X alpha=%06X tref=%06X ksel=%06X\n", i, ce, ae, tref, ksel);
            const uint32_t tb = (i & 1) ? (tref >> 12) : tref;
            if ((tb >> 6) & 1) used[tb & 7] = true;
            appendf(t, "[STATE]   stage%d: rgb %s = (%s %s lerp(%s,%s,%s) %s) %s clamp=%u | a %s = (%s %s lerp(%s,%s,%s) %s) %s clamp=%u | tex map%u coord%u %s ras chan%u | kcolor sel %u kalpha sel %u\n",
                    i, dest[(ce >> 22) & 3], cin[ce & 15], (ce >> 18) & 1 ? "-" : "+", cin[(ce >> 12) & 15], cin[(ce >> 8) & 15], cin[(ce >> 4) & 15], bias[(ce >> 16) & 3],
                    scale[(ce >> 20) & 3], (ce >> 19) & 1, dest[(ae >> 22) & 3], ain[(ae >> 4) & 7], (ae >> 18) & 1 ? "-" : "+", ain[(ae >> 13) & 7], ain[(ae >> 10) & 7],
                    ain[(ae >> 7) & 7], bias[(ae >> 16) & 3], scale[(ae >> 20) & 3], (ae >> 19) & 1, tb & 7, (tb >> 3) & 7, (tb >> 6) & 1 ? "on" : "off", (tb >> 7) & 7,
                    (i & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31, (i & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31);
        }
        for (int i = 0; i < 4; ++i)
            appendf(t, "[STATE] reg%d=(%g %g %g %g) konst%d=(%g %g %g %g)\n", i, s.tev_reg[i][0], s.tev_reg[i][1], s.tev_reg[i][2], s.tev_reg[i][3], i,
                    s.tev_konst[i][0], s.tev_konst[i][1], s.tev_konst[i][2], s.tev_konst[i][3]);
        for (int i = 0; i < 8; ++i)
            appendf(t, "[STATE] texgen%d info=%05X  map%d mode0=%06X img0=%06X img3=%06X\n", i, s.xf[0x1040 + i], i, s.bp[0x80 + (i < 4 ? i : i + 0x1C)],
                    s.bp[0x88 + (i < 4 ? i : i + 0x1C)], s.bp[0x94 + (i < 4 ? i : i + 0x1C)]);
        static const char* const wrap[4] = {"clamp", "repeat", "mirror", "?"};
        static const char* const minf[8] = {"nearest", "nearest/mip-nearest", "nearest/mip-linear", "?", "linear", "linear/mip-nearest", "linear/mip-linear", "?"};
        for (int m = 0; m < 8; ++m) {
            if (!used[m]) continue;
            const uint32_t mode0 = s.bp[m < 4 ? 0x80 + m : 0xA0 + (m - 4)], mode1 = s.bp[m < 4 ? 0x84 + m : 0xA4 + (m - 4)];
            const uint32_t img0 = s.bp[m < 4 ? 0x88 + m : 0xA8 + (m - 4)], img3 = s.bp[m < 4 ? 0x94 + m : 0xB4 + (m - 4)];
            const uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
            appendf(t, "[STATE]   map%d: %ux%u fmt=%X addr=%06X wrap=%s/%s mag=%s min=%s lod_bias=%g lod=[%g,%g] levels=%u (mode0=%06X mode1=%06X)\n", m, w, h, fmt,
                    (img3 & 0xFFFFFF) << 5, wrap[mode0 & 3], wrap[(mode0 >> 2) & 3], (mode0 & 0x10) ? "linear" : "nearest", minf[(mode0 >> 5) & 7], lod_bias_of(mode0),
                    min_lod_of(mode1), max_lod_of(mode1), mip_level_count(w, h, mode0, mode1), mode0, mode1);
        }
        const FogParams fog = decode_fog(s.bp);
        appendf(t, "[STATE] fog=%06X/%06X/%06X/%06X colour=%06X range=%06X/%06X ind=%06X genmode_ind=%u\n", s.bp[0xEE], s.bp[0xEF], s.bp[0xF0], s.bp[0xF1],
                s.bp[0xF2], s.bp[0xE8], s.bp[0xE9], s.bp[0x10], (s.bp[0] >> 16) & 7);
        const bool range_adjust = (s.bp[0xE8] >> 10) & 1;
        const uint32_t indirect = indirect_tev_stages(s.bp);
        appendf(t, "[STATE]   fog type=%u%s ortho=%u | range adjust %s (centre %u)%s | indirect TEV stages mask=%X%s\n", fog.type, fog.type == kFogOff ? " (off)" : "", fog.ortho,
                range_adjust ? "ON" : "off", s.bp[0xE8] & 0x3FF, range_adjust ? " NOT IMPLEMENTED" : "", indirect, indirect ? " NOT IMPLEMENTED" : "");
        return t;
    }

    void dump_state() const { std::fputs(state_text().c_str(), stderr); }

    void draw(Primitive prim, uint32_t count, const Layout& l, const uint8_t* data) {
        if (count == 0) return;
        ++g_stats.draws;
        g_stats.vertices += count;
        if (!g_graphics) return;
        bench::ScopedNs timer(g_render_stats.parse_ns);

        const State& s = g_state;
        vertices_.resize(size_t(count) * kVertexFloats);

        // Projection (XF 0x1020..0x1026).
        const float p0 = s.xf_float(0x1020), p1 = s.xf_float(0x1021), p2 = s.xf_float(0x1022),
                    p3 = s.xf_float(0x1023), p4 = s.xf_float(0x1024), p5 = s.xf_float(0x1025);
        const bool ortho = s.xf[0x1026] != 0;
        // Viewport depth mapping (XF 0x101C zRange, 0x101F far).
        const float z_range = s.xf_float(0x101C), far_z = s.xf_float(0x101F);

        float mat_color[2][4];
        unpack_rgba8(s.xf[0x100C], mat_color[0]);
        unpack_rgba8(s.xf[0x100D], mat_color[1]);

        const uint32_t num_texgens = s.bp[0x00] & 0xF;
        const bool trace_stderr = tracing_frame();
        const bool trace_diag = diag::g_trigger.capturing();
        const bool trace = trace_stderr || trace_diag;
        static int trace_n = 0;
        // Several frames can be traced one after the other (the script command `trace-frame` repeated): the draws are
        // numbered from 1 in each, and trace_ordinal counts the traced frames (0 for the first). A diagnostic bundle
        // numbers its draws the same way but does not take part in the count of traced frames.
        static uint64_t trace_last_frame = ~uint64_t(0), ordinal_last_frame = ~uint64_t(0);
        static int trace_ordinal = -1;
        static std::string last_state;  // the state dump of the previous draw of a bundle: repeated dumps are not written
        static int last_state_draw = 0;
        if (trace && g_frames != trace_last_frame) {
            trace_last_frame = g_frames;
            trace_n = 0;
            last_state.clear();
        }
        if (trace_stderr && g_frames != ordinal_last_frame) {
            ordinal_last_frame = g_frames;
            ++trace_ordinal;
        }
        if (trace) {
            ++trace_n;
            // GP_TRACE_STATE=n[,m...]: full TEV / texture / raster state at those draws of the traced frame.
            if (trace_stderr) {
                if (const char* list = std::getenv("GP_TRACE_STATE")) {
                    for (const char* q = list; *q;) {
                        char* end;
                        const long n = std::strtol(q, &end, 10);
                        if (n == trace_n) dump_state();
                        q = *end ? end + 1 : end;
                    }
                }
            }
            trace_emit(trace_stderr, trace_diag,
                       "[DRAW] #%d prim=%u n=%u vsize=%u vcd=%06X/%04X gen=%06X tev=%u z=%02X blend=%04X acmp=%06X cc0=%06X ca0=%06X tref0=%06X img0=%06X/%06X ntexg=%u proj=%g %g %g %g %g %g o=%u vp=%g %g %g %g\n",
                       trace_n, unsigned(prim), count, l.size, s.vcd_lo, s.vcd_hi, s.bp[0], ((s.bp[0] >> 10) & 0xF) + 1, s.bp[0x40], s.bp[0x41], s.bp[0xF3],
                       s.bp[0xC0], s.bp[0xC1], s.bp[0x28], s.bp[0x88], s.bp[0x94], num_texgens, p0, p1, p2, p3, p4, p5, ortho, s.xf_float(0x101A),
                       s.xf_float(0x101B), s.xf_float(0x101D), s.xf_float(0x101E));
            if (trace_diag) {
                diag::Bundle& b = diag::g_bundle;
                b.count("draws");
                b.count("vertices", count);
                static const char* const kPrimitive[8] = {"draws: quads", "draws: 1?", "draws: triangles", "draws: triangle strips", "draws: triangle fans", "draws: lines",
                                                          "draws: line strips", "draws: points"};
                b.count(kPrimitive[unsigned(prim) & 7]);
                diag::note_draw_features(b, s.bp, s.xf);
                // Every draw is followed by the state it ran with, written in full when it differs from the previous draw's.
                std::string state = state_text();
                if (state != last_state) {
                    b.trace += "[DRAW] state of draw #" + std::to_string(trace_n) + ":\n" + state;
                    last_state = std::move(state);
                    last_state_draw = trace_n;
                } else {
                    b.trace += "[DRAW] state of draw #" + std::to_string(trace_n) + " = state of draw #" + std::to_string(last_state_draw) + "\n";
                }
            }
            // GP_TRACE_BT=first-last (or `ortho`): the guest call chain that fed those draws, as `[bt] #N bt: ...`
            // lines for tools/guest-watch.py (which guest code draws the fade quad, the HUD...).
            static int bt_first = -1, bt_last = -1;
            static bool bt_ortho = false;
            static const bool bt_parsed = [] {
                if (const char* e = std::getenv("GP_TRACE_BT")) {
                    if (!std::strcmp(e, "ortho")) bt_ortho = true;
                    else if (std::sscanf(e, "%d-%d", &bt_first, &bt_last) == 1) bt_last = bt_first;
                }
                return true;
            }();
            (void)bt_parsed;
            if (trace_stderr && ((trace_n >= bt_first && trace_n <= bt_last) || (bt_ortho && ortho))) {
                std::fprintf(stderr, "[bt] #%d", trace_n);
                print_call_chain(stderr);
                std::fprintf(stderr, "\n");
            }
        }
        // Matrix indices of the draw; a vertex may override them with the indices stored in it.
        const uint32_t base_pos_mtx = s.matidx_a & 0x3F;
        uint32_t base_tex_mtx[8];
        for (int i = 0; i < 8; ++i)
            base_tex_mtx[i] = i < 4 ? (s.matidx_a >> (6 * (i + 1))) & 0x3F : (s.matidx_b >> (6 * (i - 4))) & 0x3F;
        // The normal only matters when one of the four channel controls (XF 0x100E..0x1011) has lighting enabled.
        const bool lit = ((s.xf[0x100E] | s.xf[0x100F] | s.xf[0x1010] | s.xf[0x1011]) & 2) != 0;
        // Indexed attribute arrays: the host address of each array once per draw (guest RAM is contiguous), not a
        // translation per vertex and attribute.
        const uint8_t* array_ptr[16] = {};
        auto resolve = [&](AttrMode mode, uint32_t array) {
            if (mode == kIndex8 || mode == kIndex16) array_ptr[array] = g_mem.read_ptr(s.array_base[array]);
        };
        resolve(l.pos, 0);
        resolve(l.nrm, 1);
        for (int c = 0; c < 2; ++c) resolve(l.col[c], 2 + c);
        for (int t = 0; t < 8; ++t) resolve(l.tex[t], 4 + t);
        // Texture coordinate generators: source row and projectivity of each, decoded once.
        struct TexGen { uint32_t row; bool projective; uint32_t info; };
        TexGen texgens[8] = {};
        for (uint32_t i = 0; i < num_texgens && i < 8; ++i) {
            const uint32_t info = s.xf[0x1040 + i];
            texgens[i] = {(info >> 7) & 0x1F, bool((info >> 1) & 1), info};
        }
        const uint8_t* p = data;
        for (uint32_t v = 0; v < count; ++v) {
            float* out = &vertices_[size_t(v) * kVertexFloats];
            uint32_t pos_mtx = base_pos_mtx;
            uint32_t tex_mtx[8];
            std::memcpy(tex_mtx, base_tex_mtx, sizeof tex_mtx);
            if (l.pmidx) pos_mtx = *p++ & 0x3F;
            for (int i = 0; i < 8; ++i)
                if (l.texmidx[i]) tex_mtx[i] = *p++ & 0x3F;

            auto fetch = [&](AttrMode mode, uint32_t array, uint32_t direct_size) -> const uint8_t* {
                if (mode == kDirect) {
                    const uint8_t* q = p;
                    p += direct_size;
                    return q;
                }
                uint32_t index;
                if (mode == kIndex8) index = *p++;
                else {
                    index = be16(p);
                    p += 2;
                }
                return array_ptr[array] + size_t(index) * s.array_stride[array];
            };

            float pos[3] = {0, 0, 0};
            if (l.pos != kNone) {
                const uint32_t csize = comp_size(l.pos_fmt);
                const uint8_t* q = fetch(l.pos, 0, l.pos_cnt * csize);
                for (uint32_t i = 0; i < l.pos_cnt; ++i) pos[i] = read_component(q + i * csize, l.pos_fmt, l.pos_shift);
            }
            // Normal (the first three components; binormal and tangent of NBT are not used).
            float nrm[3] = {0, 0, 0};
            if (l.nrm != kNone) {
                const uint32_t csize = comp_size(l.nrm_fmt);
                const uint8_t* q = fetch(l.nrm, 1, l.nrm_cnt * csize);
                const uint32_t shift = l.nrm_fmt >= 4 ? 0 : (csize == 1 ? 6 : 14);
                for (uint32_t i = 0; i < 3; ++i) nrm[i] = read_component(q + i * csize, l.nrm_fmt, shift);
            }

            // Vertex colors; the material registers stand in where the vertex has none.
            float color[2][4];
            for (int c = 0; c < 2; ++c) {
                std::memcpy(color[c], mat_color[c], sizeof(color[c]));
                if (l.col[c] != kNone) {
                    const uint8_t* q = fetch(l.col[c], 2 + c, color_size(l.col_fmt[c]));
                    read_color(q, l.col_fmt[c], color[c]);
                }
            }

            float st[8][2] = {};
            bool has_tex[8] = {};
            for (int t = 0; t < 8; ++t) {
                if (l.tex[t] == kNone) continue;
                const uint32_t csize = comp_size(l.tex_fmt[t]);
                const uint32_t comps = l.tex_cnt[t] + 1;
                const uint8_t* q = fetch(l.tex[t], 4 + t, comps * csize);
                for (uint32_t i = 0; i < comps; ++i) st[t][i] = read_component(q + i * csize, l.tex_fmt[t], l.tex_shift[t]);
                has_tex[t] = true;
            }

            // Position: model-view (3x4 at XF row index pos_mtx), then projection.
            const uint32_t m = pos_mtx * 4;
            float view[3];
            for (int r = 0; r < 3; ++r) {
                view[r] = s.xf_float(m + r * 4 + 0) * pos[0] + s.xf_float(m + r * 4 + 1) * pos[1] +
                          s.xf_float(m + r * 4 + 2) * pos[2] + s.xf_float(m + r * 4 + 3);
            }
            float cx, cy, cz, cw;
            if (ortho) {
                cx = p0 * view[0] + p1;
                cy = p2 * view[1] + p3;
                cz = p4 * view[2] + p5;
                cw = 1.0f;
            } else {
                cx = p0 * view[0] + p1 * view[2];
                cy = p2 * view[1] + p3 * view[2];
                cz = p4 * view[2] + p5;
                cw = -view[2];
            }
            const float zgc = cw != 0.0f ? cz / cw : 0.0f;
            const float depth = (far_z + zgc * z_range) / 16777215.0f;
            // Channel colors: material and ambient sources, lights (XF color/alpha channels).
            float shaded[2][4];
            float view_nrm[3] = {0, 0, 0};
            if (lit || trace) transform_normal(s.xf, pos_mtx, nrm, view_nrm);
            if (lit) {
                light_vertex(s.xf, color, view, view_nrm, shaded);
            } else {
                // No channel lights: each component is its material source (what light_vertex returns for such a control).
                for (int c = 0; c < 2; ++c) {
                    const uint32_t cc = s.xf[0x100E + c], ac = s.xf[0x1010 + c];
                    for (int i = 0; i < 3; ++i) shaded[c][i] = (cc & 1) ? color[c][i] : mat_color[c][i];
                    shaded[c][3] = (ac & 1) ? color[c][3] : mat_color[c][3];
                }
            }
            if (trace && trace_n <= 60 && v == 0) {
                trace_emit(trace_stderr, trace_diag, "[DRAW]   ndc=(%g %g %g) w=%g lit0=(%g %g %g %g) nrm=(%g %g %g) view_nrm=(%g %g %g) ctrl0=%X ctrl1=%X amb0=%08X mat0=%08X L0col=%08X L1col=%08X L0pos=(%g %g %g) L0dir=(%g %g %g) vcol=(%g %g %g %g)\n",
                           cx / cw, cy / cw, zgc, cw, shaded[0][0], shaded[0][1], shaded[0][2], shaded[0][3], nrm[0], nrm[1], nrm[2], view_nrm[0], view_nrm[1], view_nrm[2], s.xf[0x100E], s.xf[0x1010],
                           s.xf[0x100A], s.xf[0x100C], s.xf[0x603 + 0], s.xf[0x603 + 16], s.xf_float(0x60A), s.xf_float(0x60B), s.xf_float(0x60C),
                           s.xf_float(0x60D), s.xf_float(0x60E), s.xf_float(0x60F), color[0][0], color[0][1], color[0][2], color[0][3]);
            }
            if (trace_stderr) {
                // GP_TRACE_VERTS=first-last: every vertex of those draws of the traced frame (position, vertex
                // colour 0, lit channel 0, first texture coordinates).
                static int verts_first = -1, verts_last = -1;
                static bool verts_ortho = false;
                static const bool verts_parsed = [] {
                    if (const char* e = std::getenv("GP_TRACE_VERTS")) {
                        if (!std::strcmp(e, "ortho")) verts_ortho = true;
                        else if (std::sscanf(e, "%d-%d", &verts_first, &verts_last) == 1) verts_last = verts_first;
                    }
                    return true;
                }();
                (void)verts_parsed;
                if ((trace_n >= verts_first && trace_n <= verts_last) || (verts_ortho && ortho))
                    std::fprintf(stderr, "[VERT] #%d v%u pos=(%g %g %g) col0=(%g %g %g %g) lit0=(%g %g %g %g) st0=(%g %g)\n", trace_n, v, pos[0], pos[1], pos[2],
                                 color[0][0], color[0][1], color[0][2], color[0][3], shaded[0][0], shaded[0][1], shaded[0][2], shaded[0][3], st[0][0], st[0][1]);
            }
            // A bundle keeps the vertices of small draws (the quads of screen-space passes and 2D overlays): where they land
            // (normalised device coordinates), their colour and texture coordinates.
            if (trace_diag && count <= 8)
                trace_emit(false, true, "[VERT] #%d v%u pos=(%g %g %g) ndc=(%g %g %g) col0=(%g %g %g %g) lit0=(%g %g %g %g) st0=(%g %g)\n", trace_n, v, pos[0], pos[1], pos[2],
                           cx / cw, cy / cw, zgc, color[0][0], color[0][1], color[0][2], color[0][3], shaded[0][0], shaded[0][1], shaded[0][2], shaded[0][3], st[0][0], st[0][1]);
            if (trace && trace_n <= 3 && v < 4) {
                trace_emit(trace_stderr, trace_diag, "[DRAW]   v%u mtx=%u pos=(%g %g %g) view=(%g %g %g) clip=(%g %g %g %g) rows: %g %g %g %g %g %g %g %g %g %g %g %g\n", v, pos_mtx, pos[0], pos[1], pos[2],
                           view[0], view[1], view[2], cx, cy, cz, cw, s.xf_float(m), s.xf_float(m + 1), s.xf_float(m + 2), s.xf_float(m + 3), s.xf_float(m + 4), s.xf_float(m + 5),
                           s.xf_float(m + 6), s.xf_float(m + 7), s.xf_float(m + 8), s.xf_float(m + 9), s.xf_float(m + 10), s.xf_float(m + 11));
            }
            out[0] = cx;
            out[1] = cy;
            out[2] = (2.0f * depth - 1.0f) * cw;
            out[3] = cw;
            std::memcpy(out + 4, shaded[0], sizeof(float) * 4);
            std::memcpy(out + 8, shaded[1], sizeof(float) * 4);

            // Texture coordinates: texgen i reads its source row and applies a 2x4 or 3x4 matrix.
            for (uint32_t i = 0; i < 8; ++i) {
                float* uv = out + 12 + i * 3;
                uv[0] = uv[1] = 0.0f;
                uv[2] = 1.0f;
                if (i >= num_texgens) continue;
                const uint32_t info = texgens[i].info;
                const bool projective = texgens[i].projective;
                const uint32_t row = texgens[i].row;
                float src[4] = {0, 0, 1, 1};
                if (row == 0) {
                    src[0] = pos[0]; src[1] = pos[1]; src[2] = pos[2]; src[3] = 1;
                } else if (row >= 5 && row < 13) {
                    src[0] = st[row - 5][0]; src[1] = st[row - 5][1]; src[2] = 1; src[3] = 1;
                }
                const uint32_t tm = tex_mtx[i] * 4;
                float res[3];
                for (int r = 0; r < 3; ++r) {
                    res[r] = s.xf_float(tm + r * 4 + 0) * src[0] + s.xf_float(tm + r * 4 + 1) * src[1] +
                             s.xf_float(tm + r * 4 + 2) * src[2] + s.xf_float(tm + r * 4 + 3) * src[3];
                }
                uv[0] = res[0];
                uv[1] = res[1];
                uv[2] = projective ? res[2] : 1.0f;
                if (trace && trace_n <= 40 && v < 3 && i == 0)
                    trace_emit(trace_stderr, trace_diag, "[DRAW]   uv0 v%u=(%g %g %g) texgen0=%05X mtx=%u rows %g %g %g %g / %g %g %g %g\n", v, uv[0], uv[1], uv[2], info,
                               tex_mtx[0], s.xf_float(tm), s.xf_float(tm + 1), s.xf_float(tm + 2), s.xf_float(tm + 3), s.xf_float(tm + 4),
                               s.xf_float(tm + 5), s.xf_float(tm + 6), s.xf_float(tm + 7));
            }
        }
        // GP_TRACE_DRAWS=first-last[,first-last...]: in the traced frame, only draws numbered first..last (as printed) are
        // rendered, to see what a group of draws contributes. With several ranges, the k-th traced frame (script command
        // `trace-frame` repeated, one `shot` after each) renders the k-th range: one run shows several groups of draws.
        if (trace_stderr) {
            struct Range { unsigned lo, hi; };
            static const std::vector<Range> ranges = [] {
                std::vector<Range> v;
                if (const char* e = std::getenv("GP_TRACE_DRAWS")) {
                    for (const char* q = e; *q;) {
                        Range r;
                        int used = 0;
                        if (std::sscanf(q, "%u-%u%n", &r.lo, &r.hi, &used) != 2) break;
                        v.push_back(r);
                        q += used;
                        if (*q == ',') ++q;
                    }
                }
                return v;
            }();
            if (!ranges.empty()) {
                const Range& r = ranges[std::min(size_t(trace_ordinal), ranges.size() - 1)];
                if (unsigned(trace_n) < r.lo || unsigned(trace_n) > r.hi) return;
            }
        }
        DrawCall call{prim, count, vertices_.data()};
        renderer::draw(call, g_state);
    }
};

Parser g_parser;

}  // namespace

void reset() {
    std::memset(&g_state, 0, sizeof(g_state));
    g_state.bp[0xFE] = 0xFFFFFF;
    g_stats = Stats{};
    g_frames = 0;
    g_parser = Parser{};
}

void feed(const uint8_t* data, size_t n) {
    record::on_feed(data, n);
    g_parser.feed(data, n);
    record::on_feed_done(g_parser.idle());
}

// Where a write-gather store goes depends on the FIFO registers. The CPU FIFO (PI registers at
// 0xCC00300C base, 0xCC003010 end, 0xCC003014 write pointer) is where the hardware stores the
// gathered data in main memory. When it is the same FIFO the graphics processor reads (CP registers
// 0xCC000020/22 base), the data is executed; otherwise the game is recording a display list and
// the data only goes to memory, to be called later.
void wgp_write(uint32_t size, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < size; ++i) bytes[i] = uint8_t(value >> (8 * (size - 1 - i)));

    uint8_t* hw = g_mem.hw_regs;
    auto reg32 = [&](uint32_t addr) { return be32(hw + (addr - Memory::HW_REG_BASE)); };
    const uint32_t pi_base = reg32(0xCC00300C) & 0x03FFFFFF;
    const uint32_t gp_base = ((uint32_t(be16(hw + 0x22)) << 16) | be16(hw + 0x20)) & 0x03FFFFFF;
    if (pi_base == 0 || gp_base == 0 || pi_base == gp_base) {
        feed(bytes, size);
        return;
    }

    const uint32_t end = reg32(0xCC003010) & 0x03FFFFFF;
    uint32_t wr = reg32(0xCC003014);
    uint32_t phys = wr & 0x03FFFFFF;
    uint8_t* dst = g_mem.translate(0x80000000u | phys);
    std::memcpy(dst, bytes, size);
    g_mem.mark_written(0x80000000u | phys, uint32_t(size));
    phys += size;
    uint32_t wrap = wr & 0x04000000;
    if (phys > end) {  // `end` is the address of the last byte of the FIFO
        phys = pi_base;
        wrap ^= 0x04000000;
    }
    wr = wrap | phys;
    uint8_t* p = hw + (0xCC003014 - Memory::HW_REG_BASE);
    p[0] = uint8_t(wr >> 24);
    p[1] = uint8_t(wr >> 16);
    p[2] = uint8_t(wr >> 8);
    p[3] = uint8_t(wr);
}

uint64_t frames_presented() { return g_frames; }
void request_trace_frame(uint64_t n) { g_trace_request = long(g_frames + (n ? n - 1 : 0)); }
const Stats& stats() { return g_stats; }
const RenderStats& render_stats() { return g_render_stats; }

}  // namespace gcrecomp::gp
