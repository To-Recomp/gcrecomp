// =============================================================================
// Loud failures: guest heap dump, guest call chain, bounded stop (docs/DEBUGGING.md, "Failing loudly")
//
// A console halts in OSPanic; a recompiled game that carries on after a failed allocation reads files into guest
// address 0 (physical memory: the OS globals, the module list...) and ends up spinning in garbage. These helpers let the
// runtime say what happened and stop with a non-zero exit code.
//
// The Dolphin SDK heap (OSAlloc.c): `HeapArray` (pointer), `NumHeaps` and `__OSCurrHeap` live in the game's small data
// area; their offsets from r13 depend on the game, so the game project gives them with GCRECOMP_GUEST_HEAP=
// ARRAY_OFF:NUM_OFF:CURR_OFF (decimal or 0x, signed, r13-relative) or set_guest_heap_layout(). Heap descriptor (12
// bytes): size (-1 = unused), free list head, allocated list head. Cell (32 bytes header): prev, next, size
// (including the header), then the heap descriptor index / padding.
// =============================================================================

#include "gcrecomp/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace gcrecomp {

namespace {

GuestHeapLayout g_layout;
bool            g_layout_set = false;

bool ram_word(uint32_t a) { return a >= Memory::MAIN_RAM_BASE && a + 4 <= g_mem.ram_end && (a & 3) == 0; }
uint32_t rd(uint32_t a) { return ram_word(a) ? g_mem.read32(a) : 0; }

bool layout_from_env() {
    static bool tried = false;
    if (g_layout_set || tried) return g_layout_set;
    tried = true;
    const char* v = std::getenv("GCRECOMP_GUEST_HEAP");
    if (!v || !*v) return false;
    char* e = nullptr;
    const long a = std::strtol(v, &e, 0);
    if (*e != ':') return false;
    const long n = std::strtol(e + 1, &e, 0);
    if (*e != ':') return false;
    const long c = std::strtol(e + 1, &e, 0);
    g_layout = {static_cast<int32_t>(a), static_cast<int32_t>(n), static_cast<int32_t>(c)};
    g_layout_set = true;
    return true;
}

struct ListStats {
    uint32_t count = 0;
    uint64_t total = 0;
    uint32_t largest = 0;
    bool     broken = false;
    std::vector<std::pair<uint32_t, uint32_t>> biggest;  // (size, cell address)
};

ListStats walk(uint32_t head) {
    ListStats s;
    uint32_t p = head;
    for (uint32_t guard = 0; p != 0 && guard < 200000; ++guard) {
        if (!ram_word(p) || !ram_word(p + 8)) { s.broken = true; break; }
        const uint32_t size = rd(p + 8);
        ++s.count;
        s.total += size;
        s.largest = std::max(s.largest, size);
        s.biggest.emplace_back(size, p);
        p = rd(p + 4);
    }
    std::sort(s.biggest.begin(), s.biggest.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (s.biggest.size() > 16) s.biggest.resize(16);
    return s;
}

}  // namespace

void set_guest_heap_layout(const GuestHeapLayout& l) {
    g_layout = l;
    g_layout_set = true;
}

bool guest_heap_known() { return layout_from_env(); }

void dump_guest_heaps(const char* why, const PPCContext* ctx, bool verbose) {
    if (!layout_from_env()) {
        std::fprintf(stderr, "[heap] %s: layout unknown (set GCRECOMP_GUEST_HEAP=ARRAY_OFF:NUM_OFF:CURR_OFF, see docs/DEBUGGING.md)\n", why);
        return;
    }
    const uint32_t r13 = ctx->r[13];
    const uint32_t array = rd(r13 + g_layout.array_off);
    const int32_t  num = static_cast<int32_t>(rd(r13 + g_layout.num_off));
    const int32_t  curr = static_cast<int32_t>(rd(r13 + g_layout.curr_off));
    std::fprintf(stderr, "[heap] %s: HeapArray=0x%08X NumHeaps=%d current=%d\n", why, array, num, curr);
    if (!ram_word(array) || num <= 0 || num > 64) return;
    for (int32_t i = 0; i < num; ++i) {
        const uint32_t d = array + static_cast<uint32_t>(i) * 12;
        const int32_t size = static_cast<int32_t>(rd(d));
        if (size < 0) continue;
        const ListStats f = walk(rd(d + 4));
        const ListStats a = walk(rd(d + 8));
        std::fprintf(stderr,
                     "[heap]  #%d%s size=%d  free: %u cells, %llu bytes, largest %u%s  allocated: %u cells, %llu bytes, largest %u%s\n",
                     i, i == curr ? "*" : "", size, f.count, (unsigned long long)f.total, f.largest,
                     f.broken ? " (list broken)" : "", a.count, (unsigned long long)a.total, a.largest,
                     a.broken ? " (list broken)" : "");
        if (verbose) {
            for (auto& b : a.biggest)
                std::fprintf(stderr, "[heap]     allocated %9u bytes at 0x%08X (data 0x%08X)\n", b.first, b.second, b.second + 32);
        }
    }
    std::fflush(stderr);
}

void print_guest_backtrace(const PPCContext* ctx) {
    std::fprintf(stderr, "[stack] lr=0x%08X", ctx->lr);
    uint32_t sp = ctx->r[1];
    for (int i = 0; i < 24 && ram_word(sp); ++i) {
        const uint32_t back = rd(sp);
        if (back <= sp || !ram_word(back)) break;
        std::fprintf(stderr, " <- 0x%08X", rd(back + 4));
        sp = back;
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

namespace {
void (*g_fatal_handler)(const char*) = nullptr;
}

void set_fatal_handler(void (*handler)(const char* reason)) { g_fatal_handler = handler; }

void fatal_stop(const char* reason) {
    std::fflush(stdout);
    std::fprintf(stderr, "[FATAL] %s\n", reason);
    std::fflush(stderr);
    if (g_fatal_handler) g_fatal_handler(reason);  // tests: throws
    std::_Exit(70);
}

}  // namespace gcrecomp
