// SPDX-License-Identifier: MIT
// Pins the arithmetic of light_vertex / transform_normal: a bitwise checksum over random XF lighting state and vertices,
// recorded from the implementation before the per-vertex light factors were shared between the channel controls
// (src/gp/lighting.cpp). Built with -ffp-contract=off so a fused multiply-add does not change the bits.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "lighting.h"

using namespace gcrecomp::gp;

int main() {
    uint32_t rng = 777;
    auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    auto fl = [&](float lo, float hi) { return lo + (hi - lo) * float(next() & 0xFFFF) / 65535.0f; };
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (int scene = 0; scene < 400; ++scene) {
        std::vector<uint32_t> xf(0x1100);
        for (auto& w : xf) w = next();
        auto put = [&](uint32_t a, float f) { std::memcpy(&xf[a], &f, 4); };
        for (int l = 0; l < 8; ++l)
            for (int k = 4; k <= 15; ++k) put(0x600 + l * 16 + k, fl(-3, 3));
        for (int i = 0; i < 0x60; ++i) put(0x400 + i, fl(-1, 1));
        for (int c = 0; c < 2; ++c) {
            uint32_t ctrl = next() & 0x7FFF;
            if (scene & 1) ctrl |= 2;
            xf[0x100E + c] = ctrl;
            xf[0x1010 + c] = (scene & 2) ? ctrl : (next() & 0x7FFF);
        }
        for (int v = 0; v < 8; ++v) {
            float vc[2][4], pos[3], n[3], out[2][4];
            for (auto& ch : vc)
                for (float& x : ch) x = fl(0, 1);
            for (float& x : pos) x = fl(-5, 5);
            for (float& x : n) x = fl(-1, 1);
            light_vertex(xf.data(), vc, pos, n, out);
            mix(out, sizeof out);
            float vn[3];
            transform_normal(xf.data(), next() & 31, n, vn);
            mix(vn, sizeof vn);
        }
    }
    const uint64_t expected = 0xb7621216ed859811ull;
    if (h != expected) {
        std::fprintf(stderr, "FAIL: checksum %016llx, expected %016llx\n", (unsigned long long)h, (unsigned long long)expected);
        return 1;
    }
    std::printf("gp_lighting_golden_test: ok\n");
    return 0;
}
