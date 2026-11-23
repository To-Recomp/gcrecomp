// SPDX-License-Identifier: MIT
//
// OSTVFormat (0x800000CC): the IPL of a console writes the video standard of its region there, VIInit/VIConfigure read it.
// Left at zero, a PAL game that configures a PAL video mode trips the SDK's "Tried to change mode from (0) to (1), which is
// forbidden" panic (Tales of Symphonia PAL, three times a session).
#include <cstdio>
#include <cstring>

#include "gcrecomp/hw/gc_hw.h"
#include "gcrecomp/runtime.h"
#include "gcrecomp/vi.h"

using namespace gcrecomp;

static int g_failures = 0;
#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        if ((long long)(a) != (long long)(b)) {                                                       \
            std::printf("%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, (long long)(a),  \
                        (long long)(b));                                                              \
            ++g_failures;                                                                             \
        }                                                                                             \
    } while (0)

static uint32_t tv_format_after_init(const char* id) {
    GameConfig config;
    std::memcpy(config.game_id, id, 4);
    init_low_memory(&g_mem, config);
    return g_mem.read32(hw::OS_TV_FORMAT);
}

int main() {
    if (!g_mem.init()) {
        std::printf("memory init failed\n");
        return 1;
    }
    for (char r : {'E', 'J', 'K', 'W'}) CHECK_EQ(os_tv_format_for_region(r), hw::OS_TV_FORMAT_NTSC);
    for (char r : {'P', 'D', 'F', 'I', 'S', 'H', 'U', 'X', 'Y', 'Z', 'R'}) CHECK_EQ(os_tv_format_for_region(r), hw::OS_TV_FORMAT_PAL);
    CHECK_EQ(os_tv_format_for_region('?'), hw::OS_TV_FORMAT_NTSC);
    CHECK_EQ(tv_format_after_init("GQSF"), hw::OS_TV_FORMAT_PAL);  // Tales of Symphonia, PAL French
    CHECK_EQ(g_mem.read16(vi::kRegBase + vi::kRegDispConfig), 0x0101);  // VI enabled (else VIInit resets to NTSC), format PAL
    CHECK_EQ(tv_format_after_init("GQSE"), hw::OS_TV_FORMAT_NTSC);
    CHECK_EQ(g_mem.read16(vi::kRegBase + vi::kRegDispConfig), 0x0001);
    if (g_failures) return 1;
    std::printf("os tv format: ok\n");
    return 0;
}
