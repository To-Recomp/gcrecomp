// SPDX-License-Identifier: MIT
// The invalidation rules of GPU-resident EFB copies (no OpenGL).
#include <cstdio>

#include "efb_gpu_copy.h"

using namespace gcrecomp::gp;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static GpuCopy copy(uint32_t addr, uint32_t extent, uint32_t w, uint32_t h, uint8_t fmt = 4, uint32_t clock = 10) {
    GpuCopy c;
    c.addr = addr;
    c.extent = extent;
    c.w = w;
    c.h = h;
    c.fmt = fmt;
    c.clock = clock;
    return c;
}

int main() {
    // A new copy is found by its exact address, size and format, and is pending until written.
    {
        CopyTracker t;
        const int a = t.begin(copy(0x5C3240, 0x80000, 640, 448));
        CHECK(a >= 0);
        CHECK(t.at(a).pending && t.at(a).valid);
        CHECK(t.find(0x5C3240, 640, 448, 4) == a);
        CHECK(t.find(0x5C3240, 640, 447, 4) == -1);   // another size is another texture
        CHECK(t.find(0x5C3240, 640, 448, 5) == -1);   // another format too
        CHECK(t.find(0x5C3280, 640, 448, 4) == -1);
        CHECK(t.pending_count() == 1);
        t.mark_written(a, 42);
        CHECK(!t.at(a).pending && t.at(a).clock == 42 && t.find(0x5C3240, 640, 448, 4) == a);
        CHECK(t.pending_count() == 0);
    }
    // The same copy again reuses its slot; an overlapping other copy invalidates the old one.
    {
        CopyTracker t;
        const int a = t.begin(copy(0x1000, 0x1000, 32, 64));
        t.mark_written(a, 5);
        CHECK(t.begin(copy(0x1000, 0x1000, 32, 64)) == a);
        CHECK(t.at(a).pending);
        t.mark_written(a, 6);
        const int b = t.begin(copy(0x1800, 0x1000, 32, 64));
        CHECK(b != a);
        CHECK(!t.at(a).valid && t.at(b).valid);
        CHECK(t.find(0x1000, 32, 64, 4) == -1 && t.find(0x1800, 32, 64, 4) == b);
        // Disjoint ranges do not touch each other.
        const int c = t.begin(copy(0x9000, 0x100, 8, 8));
        CHECK(t.at(b).valid && t.at(c).valid);
    }
    // pending_overlapping lists exactly the unwritten copies that overlap the range.
    {
        CopyTracker t;
        const int a = t.begin(copy(0x1000, 0x1000, 32, 64));
        const int b = t.begin(copy(0x4000, 0x1000, 32, 64));
        t.mark_written(b, 3);
        int out[CopyTracker::kSlots];
        CHECK(t.pending_overlapping(0x1FFF, 1, out) == 1 && out[0] == a);
        CHECK(t.pending_overlapping(0x2000, 16, out) == 0);   // one byte past the copy
        CHECK(t.pending_overlapping(0x0FF0, 0x10, out) == 0);  // ends where the copy begins
        CHECK(t.pending_overlapping(0x0FF0, 0x11, out) == 1);
        CHECK(t.pending_overlapping(0x4000, 16, out) == 0);    // written already
    }
    // invalidate / invalidate_overlapping (a store by something else under the copy).
    {
        CopyTracker t;
        const int a = t.begin(copy(0x1000, 0x1000, 32, 64));
        t.mark_written(a, 3);
        t.invalidate_overlapping(0x3000, 16);
        CHECK(t.find(0x1000, 32, 64, 4) == a);
        t.invalidate_overlapping(0x1800, 4);
        CHECK(t.find(0x1000, 32, 64, 4) == -1);
        const int b = t.begin(copy(0x1000, 0x1000, 32, 64));
        CHECK(b == a && t.find(0x1000, 32, 64, 4) == a);
        t.invalidate(a);
        CHECK(t.find(0x1000, 32, 64, 4) == -1);
    }
    // Slots: pending ones are never recycled; the oldest written one is.
    {
        CopyTracker t;
        int slots[CopyTracker::kSlots];
        for (int i = 0; i < CopyTracker::kSlots; ++i) slots[i] = t.begin(copy(0x10000 * (i + 1), 0x100, 8, 8));
        CHECK(t.begin(copy(0xF00000, 0x100, 8, 8)) == -1);  // all pending
        t.mark_written(slots[3], 1);
        t.mark_written(slots[5], 1);
        const int r = t.begin(copy(0xF00000, 0x100, 8, 8));
        CHECK(r == slots[3]);  // the older of the two written ones
    }
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("efb_gpu_copy_test: ok");
    return 0;
}
