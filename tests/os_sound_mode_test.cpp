// SPDX-License-Identifier: MIT
//
// OSGetSoundMode / OSSetSoundMode HLE: the console is stereo unless the game (or GCRECOMP_SOUND_MODE=mono) says
// otherwise. With the SDK's own code the unloaded SRAM read as mono and Tales of Symphonia played its movie and
// music in mono.
#include <cstdio>
#include <cstdlib>

#include "gcrecomp/runtime.h"

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

static uint32_t call(const char* name, uint32_t arg) {
    RecompiledFunc f = lookup_os_func(name);
    if (!f) {
        std::printf("no replacement for %s\n", name);
        ++g_failures;
        return 12345;
    }
    PPCContext ctx = {};
    ctx.r[3] = arg;
    f(&ctx, &g_mem);
    return ctx.r[3];
}

int main() {
    unsetenv("GCRECOMP_SOUND_MODE");
    CHECK_EQ(call("OSGetSoundMode", 0), 1);  // stereo by default
    call("OSSetSoundMode", 0);
    CHECK_EQ(call("OSGetSoundMode", 0), 0);
    call("OSSetSoundMode", 1);
    CHECK_EQ(call("OSGetSoundMode", 0), 1);
    call("OSSetSoundMode", 7);  // anything but mono is stereo
    CHECK_EQ(call("OSGetSoundMode", 0), 1);
    if (g_failures) return 1;
    std::printf("os sound mode: ok\n");
    return 0;
}
