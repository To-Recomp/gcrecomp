// SPDX-License-Identifier: MIT
// Memory write stamps (gcrecomp/runtime.h): a cache of data derived from guest RAM learns which pages were written.
#include <cstdio>
#include <cstdlib>

#include "gcrecomp/runtime.h"

using gcrecomp::Memory;

static int g_failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

int main() {
    Memory m;
    CHECK(m.init(0x100000));
    const uint32_t base = Memory::MAIN_RAM_BASE;

    // A range nobody wrote after the clock was taken is clean.
    uint32_t clock = m.advance_clock();
    CHECK(!m.written_since(base + 0x4000, 0x3000, clock));

    // A store inside it makes it dirty; other ranges stay clean.
    m.write32(base + 0x5004, 0x11223344);
    CHECK(m.written_since(base + 0x4000, 0x3000, clock));
    CHECK(!m.written_since(base + 0x8000, 0x1000, clock));
    CHECK(!m.written_since(base + 0x4000, 0x1000, clock));

    // After a new epoch the same range is clean again, until the next store (any width, either mirror).
    clock = m.advance_clock();
    CHECK(!m.written_since(base + 0x4000, 0x3000, clock));
    m.write8(Memory::UNCACHED_BASE + 0x6000, 1);
    CHECK(m.written_since(base + 0x4000, 0x3000, clock));
    clock = m.advance_clock();
    m.write16(base + 0x7000, 2);
    CHECK(m.written_since(base + 0x7000, 2, clock));
    clock = m.advance_clock();

    // A store that straddles a page stamps both pages.
    m.write32(base + 0x9FFE, 0xAABBCCDD);
    CHECK(m.written_since(base + 0x9000, 0x1000, clock));
    CHECK(m.written_since(base + 0xA000, 0x1000, clock));
    clock = m.advance_clock();
    m.write16(base + 0xBFFF, 0xAABB);
    CHECK(m.written_since(base + 0xC000, 1, clock));

    // Reads through the const translate / read_ptr do not stamp; the non-const translate and mark_written do.
    clock = m.advance_clock();
    CHECK(m.read32(base + 0x2000) == 0);
    (void)m.read_ptr(base + 0x2000);
    CHECK(!m.written_since(base + 0x2000, 4, clock));
    *m.translate(base + 0x2010) = 1;
    CHECK(m.written_since(base + 0x2000, 0x1000, clock));
    clock = m.advance_clock();
    m.mark_written(base + 0x20000, 0x2800);
    CHECK(m.written_since(base + 0x20000, 1, clock));
    CHECK(m.written_since(base + 0x22000, 1, clock));
    CHECK(!m.written_since(base + 0x23000, 1, clock));

    // Ranges outside RAM are never trusted.
    CHECK(m.written_since(0xCC000000, 4, clock));
    CHECK(m.written_since(base + 0x100000 - 8, 16, clock));

    // The Memory is still correct.
    m.write32(base + 0x100, 0xCAFEBABE);
    CHECK(m.read32(base + 0x100) == 0xCAFEBABE);
    m.shutdown();
    if (g_failures == 0) std::printf("memory_stamp_test: ok\n");
    return g_failures ? 1 : 0;
}
