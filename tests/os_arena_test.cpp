// SPDX-License-Identifier: MIT
//
// The arena's high bound (0x80000034, OSGetArenaHi). On a console the apploader puts the disc's FST at the top of RAM and
// OSInit starts the arena just below it. Tales of Symphonia creates its battle heap as OSCreateHeap(0x81200000,
// OSGetArenaHi()): with the former fixed 0x81700000 the heap was 1 MB smaller than on the console and the first boss ran out
// of memory (docs/DEBUGGING.md, "Heap exhaustion").
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>

#include "gcrecomp/hw/gc_hw.h"
#include "gcrecomp/runtime.h"
#include "test_env.h"

using namespace gcrecomp;

static int g_failures = 0;
#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        if ((long long)(a) != (long long)(b)) {                                                       \
            std::printf("%s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, (long long)(a), \
                        (long long)(b));                                                              \
            ++g_failures;                                                                             \
        }                                                                                             \
    } while (0)

int main() {
    if (!g_mem.init()) {
        std::printf("memory init failed\n");
        return 1;
    }
    unset_env("GCRECOMP_ARENA_HI");

    // Tales of Symphonia: FST max size 0x9EC4 -> 0x817F6120 (rounded down to 32 bytes).
    CHECK_EQ(arena_hi_for_fst(24 * 1024 * 1024, 0x9EC4), 0x817F6120);
    CHECK_EQ(arena_hi_for_fst(24 * 1024 * 1024, 0x1740), 0x817FE8C0);   // the usual retail value for a small FST
    CHECK_EQ(arena_hi_for_fst(48 * 1024 * 1024, 0x1740), 0x82FFE8C0);

    GameConfig config;
    std::memcpy(config.game_id, "GQSF", 4);
    init_low_memory(&g_mem, config);
    CHECK_EQ(g_mem.read32(hw::OS_ARENA_HI), 0x81700000);                // no FST size given: the configured bound

    config.fst_max_size = 0x9EC4;
    init_low_memory(&g_mem, config);
    CHECK_EQ(g_mem.read32(hw::OS_ARENA_HI), 0x817F6120);

    set_env("GCRECOMP_ARENA_HI", "0x81780000");
    init_low_memory(&g_mem, config);
    CHECK_EQ(g_mem.read32(hw::OS_ARENA_HI), 0x81780000);                // the experiment override wins
    unset_env("GCRECOMP_ARENA_HI");

    // boot.bin: the FST max size is the big-endian word at 0x42C.
    const std::string file = (std::filesystem::temp_directory_path() / "gcrecomp_os_arena_test.bin").string();
    {
        FILE* f = std::fopen(file.c_str(), "wb");
        if (!f) {
            std::printf("cannot write %s\n", file.c_str());
            return 1;
        }
        unsigned char buf[0x440] = {0};
        buf[0x42C] = 0x00; buf[0x42D] = 0x00; buf[0x42E] = 0x9E; buf[0x42F] = 0xC4;
        std::fwrite(buf, 1, sizeof buf, f);
        std::fclose(f);
    }
    CHECK_EQ(disc_fst_max_size(file.c_str()), 0x9EC4);
    std::remove(file.c_str());
    CHECK_EQ(disc_fst_max_size(file.c_str()), 0);                       // unreadable: 0

    if (g_failures == 0) std::printf("os_arena_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
