// SPDX-License-Identifier: MIT
// Pins the output of encode_efb_copy: pseudo-random images through every destination format, colour and depth, with and
// without half scale and Y scale, checksummed against values recorded from the implementation before it was
// specialised per layout (src/gp/efb_copy.cpp). A change of these checksums is a change of what the game reads back
// from an EFB copy.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "efb_copy.h"

using namespace gcrecomp::gp;

static uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static const uint64_t kExpected[128] = {
    0x1330aeff84fb51dbull,
    0x9aabb02bf36eb827ull,
    0x91f1397e81a42306ull,
    0x1e8ebba0a3dd30e3ull,
    0xd7ed250eaab13bc8ull,
    0x6baf0df7ac419066ull,
    0x911e64edd79b6017ull,
    0x0441d7b3f405e28dull,
    0xa8e6aeb4a4562681ull,
    0xa4a4f43f309be3cfull,
    0x2ce41275a345b97aull,
    0xc25157cabf237780ull,
    0x3a8416e12a57138bull,
    0xb8b411b807525eacull,
    0x094f9d27c3052a99ull,
    0x87b10c84ff47c18cull,
    0x2823376fa9b2783aull,
    0x412a9d93e5d53644ull,
    0x8167666b60d5a691ull,
    0xfdbd473e96d53613ull,
    0xe448aa942818e8bcull,
    0x4e80cca90475141eull,
    0xc04452d6d7557a79ull,
    0x6157a76651fa9f97ull,
    0x2a0a7614b3a5fb44ull,
    0xabb342eb6d45bedfull,
    0x6c499cb2cc4fcf3aull,
    0x95c54702ddf642b4ull,
    0x3b03f613c6b61050ull,
    0x08c7ab54fa351ec7ull,
    0x923dbe7a81b09eeaull,
    0x62bd21bea3e0bb88ull,
    0x30e8d26a7d2f40afull,
    0x7cd9fa78c54fe328ull,
    0x5c46fd4f42d85c5cull,
    0xb871796f12c206dbull,
    0x4e14ae36d2805cb9ull,
    0x74d126a71e071a21ull,
    0x02712479b3fabaccull,
    0x255638abca115202ull,
    0x1b41706f593d02fdull,
    0xc11eb24f8a7ab1c1ull,
    0x6bf23793291b7082ull,
    0x14d4c7f253fde569ull,
    0x37eec8ae30154ebaull,
    0xfadf7c0073f5c7bfull,
    0x238a3650e6485402ull,
    0xefb6fd9f6da9f240ull,
    0xcb38421d71a51a2full,
    0x4c334668ff5456f5ull,
    0x45c1067e724551a2ull,
    0x4439484852f3f37aull,
    0xa484f002ab5435d3ull,
    0xe23c5e3427b26053ull,
    0x1a358451490f76d3ull,
    0xdd3971258b71a553ull,
    0xfca40a88782b4cd3ull,
    0x65f6067348f8b8f3ull,
    0x73094b2209981ed3ull,
    0xa819d7bf07721e73ull,
    0xf6fa6eff5cfe75b3ull,
    0x67b2cd0919f53fd3ull,
    0xc5ffcf0ad64d51f3ull,
    0xb58b18013dae5893ull,
    0x321f2ce7342ad761ull,
    0x2b6feeecb101d815ull,
    0x5da3372fc8ec25a5ull,
    0x10d305956dc1cf70ull,
    0xe1d238f04f74b799ull,
    0xe8a3a3af26d005b8ull,
    0xda524654eb4c7f2aull,
    0x3a9282bfb617ecf7ull,
    0xe8dcc90f068eac73ull,
    0xbdd1040c11ac95f6ull,
    0x890c31a7db036769ull,
    0xf06f1100e2b3f3f1ull,
    0x0f03c2cae5a0a4d6ull,
    0xdcfb503104ac216bull,
    0xaeb92560532b6fe8ull,
    0x0c4e400c601cc6c9ull,
    0xddc68ef30a4e7a4eull,
    0x33917c21d69f706aull,
    0x955198290134bb34ull,
    0x3b2eaf79a8f3f242ull,
    0xf866212ee32e9bebull,
    0x385eb5bf0a33e74bull,
    0x9b60c33c461cf8ebull,
    0x9f4acefa4c5ad781ull,
    0x71fb3426db8f3600ull,
    0xea432f7c4ad8156aull,
    0x8a227bf00ad78d5bull,
    0xf5ca331eaae57a7eull,
    0xe4d7db04e1d49298ull,
    0xfc1df101f0e86f60ull,
    0xe66851d436d9904aull,
    0x16748713a6e8bbeaull,
    0xb21adb70839e3802ull,
    0x55cbb7c24da36607ull,
    0x000a276b8c7627b8ull,
    0x78f02971c5e9a0ccull,
    0x3c5bff92e614f01bull,
    0x38acf62cb327182cull,
    0x0ad08d1c9e79c20aull,
    0xdc0ed64ab847b6d3ull,
    0xa78c5568d085964full,
    0x9da7abdcbbc4a4fcull,
    0x8f8e53a42d135534ull,
    0xfe9ada296ab036f3ull,
    0x8c1545b5491226adull,
    0xa39bf6e667e41747ull,
    0x6aa3f654d27d5b7cull,
    0xf918bb030cc7c09bull,
    0xd8a0d180443e4a08ull,
    0x1f7daedf155509cfull,
    0xdb844969ef63fa11ull,
    0x5397a74375558d6dull,
    0x85396e2cc954cc54ull,
    0x02528e8351ca7c6full,
    0xfe9af419e1140d36ull,
    0xb240d3fc100f831eull,
    0xa4525e98b6494728ull,
    0xdefaea85786607e4ull,
    0x5fd5f0601302a1b7ull,
    0x483ceef840608f97ull,
    0xdafd513defca5cc9ull,
    0xff802e245b6184cdull,
    0xc711e0e5c40dfa18ull,
    0xadee0bfea814ff5cull};

int main() {
    uint32_t rng = 12345;
    auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    int failures = 0, index = 0;
    for (int depth = 0; depth < 2; ++depth)
        for (int fmt = 0; fmt < 16; ++fmt)
            for (int mode = 0; mode < 4; ++mode, ++index) {  // none, half, yscale, half + yscale
                const int w = 37 + int(next() % 40), h = 21 + int(next() % 30);
                std::vector<uint8_t> rgba(size_t(w) * h * 4);
                for (auto& b : rgba) b = uint8_t(next());
                std::vector<uint32_t> z(size_t(w) * h);
                for (auto& v : z) v = next() * 3;
                const bool half = mode & 1, ys = mode & 2;
                const uint32_t cmd = (uint32_t(fmt & 7) << 4) | (uint32_t(fmt >> 3) << 3) | (uint32_t(next() & 1) << 15) |
                                     (uint32_t(half) << 9) | (uint32_t(ys) << 10);
                EfbCopyParams p = make_efb_copy_params(cmd, 0, uint32_t(w - 1) | (uint32_t(h - 1) << 10), 0,
                                                       ys ? 100 + next() % 300 : 0x100, depth);
                int ow, oh, tw, th, tb;
                efb_copy_output_size(p, &ow, &oh);
                efb_copy_tile_geometry(p, &tw, &th, &tb);
                p.stride = uint32_t((ow + tw - 1) / tw * tb) + (next() & 1) * 32;
                std::vector<uint8_t> out(efb_copy_extent(p) + 16, 0xAB);
                encode_efb_copy(p, rgba.data(), depth ? z.data() : nullptr, out.data());
                if (fnv(out.data(), out.size()) != kExpected[index]) {
                    std::fprintf(stderr, "FAIL depth=%d format=%d mode=%d\n", depth, fmt, mode);
                    ++failures;
                }
            }
    if (!failures) std::printf("efb_copy_golden_test: ok\n");
    return failures ? 1 : 0;
}
