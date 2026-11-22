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

    // The inline fast path (cached RAM) and the general path (uncached / physical mirrors) agree: big-endian layout, the
    // mirrors alias, every operation counts once for the interrupt poll (read64 / write64 as two).
    {
        using gcrecomp::bench::g_mem_ops;
        m.write32(base + 0x300, 0x11223344);
        CHECK(m.ram[0x300] == 0x11 && m.ram[0x301] == 0x22 && m.ram[0x302] == 0x33 && m.ram[0x303] == 0x44);
        CHECK(m.read16(base + 0x300) == 0x1122 && m.read16(base + 0x302) == 0x3344 && m.read8(base + 0x301) == 0x22);
        CHECK(m.read32(Memory::UNCACHED_BASE + 0x300) == 0x11223344 && m.read32(0x300) == 0x11223344);
        m.write16(Memory::UNCACHED_BASE + 0x304, 0xBEEF);
        CHECK(m.read16(base + 0x304) == 0xBEEF);
        m.write64(base + 0x310, 0x0102030405060708ull);
        CHECK(m.read64(base + 0x310) == 0x0102030405060708ull && m.read32(base + 0x314) == 0x05060708);
        m.writef32(base + 0x320, 1.0f);
        CHECK(m.read32(base + 0x320) == 0x3F800000 && m.readf32(base + 0x320) == 1.0f);
        m.writef64(base + 0x328, -2.5);
        CHECK(m.read64(base + 0x328) == 0xC004000000000000ull && m.readf64(base + 0x328) == -2.5);
        // Unaligned, across a page boundary.
        m.write32(base + 0x1FFE, 0xA1B2C3D4);
        CHECK(m.read32(base + 0x1FFE) == 0xA1B2C3D4 && m.read8(base + 0x2000) == 0xC3);

        uint64_t ops = g_mem_ops;
        m.read8(base + 0x10); m.read16(base + 0x10); m.read32(base + 0x10);
        CHECK(g_mem_ops == ops + 3);
        m.write8(base + 0x10, 1); m.write16(base + 0x10, 1); m.write32(base + 0x10, 1);
        CHECK(g_mem_ops == ops + 6);
        m.read32(Memory::UNCACHED_BASE + 0x10); m.write32(Memory::UNCACHED_BASE + 0x10, 1);
        CHECK(g_mem_ops == ops + 8);
        m.read64(base + 0x10); m.write64(base + 0x10, 1);
        CHECK(g_mem_ops == ops + 12);
    }

    // The Memory is still correct.
    m.write32(base + 0x100, 0xCAFEBABE);
    CHECK(m.read32(base + 0x100) == 0xCAFEBABE);
    m.shutdown();
    if (g_failures == 0) std::printf("memory_stamp_test: ok\n");
    return g_failures ? 1 : 0;
}
