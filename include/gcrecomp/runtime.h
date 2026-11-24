#pragma once
// =============================================================================
// gcrecomp - GameCube Static Recompilation Runtime
//
// This header defines the core runtime types that recompiled GameCube code
// operates against. Every recompiled function receives a PPCContext* (the
// emulated CPU state) and a Memory* (the emulated address space), and
// reads/writes these structures to produce the same effects as the original
// PowerPC code.
//
// References:
//   - libogc (devkitPro, zlib license): Dolphin SDK function semantics
//   - Pureikyubu / Dolwin (CC0): OS HLE patterns and low-memory layout
//   - GameCubeRecompiled (CC0): Static recompilation architecture
//
// This is the game-agnostic toolkit layer. Game-specific projects include
// this header and link against the gcrecomp runtime library.
// =============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "gcrecomp/bench.h"

#if defined(__GNUC__) || defined(__clang__)
#define GCRECOMP_LIKELY(x) __builtin_expect(!!(x), 1)
#define GCRECOMP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define GCRECOMP_LIKELY(x) (x)
#define GCRECOMP_UNLIKELY(x) (x)
#endif
// The memory accessors are small inline functions with an out-of-line slow path. Recompiled code is usually built at -O1
// (compile time), where GCC does not inline small functions on its own: they stay calls, cheaper than before (no
// per-access hardware-register checks) but still calls. Defining GCRECOMP_FORCE_INLINE_MEMORY for the recompiled code
// inlines them at every access: about a quarter less CPU in guest-heavy code (the intro movie) for 2.4 times the compile
// time and twice the binary size (docs/BENCHMARK.md).
#if (defined(__GNUC__) || defined(__clang__)) && defined(GCRECOMP_FORCE_INLINE_MEMORY)
#define GCRECOMP_FORCE_INLINE inline __attribute__((always_inline))
#else
#define GCRECOMP_FORCE_INLINE inline
#endif

namespace gcrecomp {

// Byte order of the guest (big endian) against the host: no-ops on a big-endian host.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline uint16_t big16(uint16_t v) { return v; }
inline uint32_t big32(uint32_t v) { return v; }
#elif defined(__GNUC__) || defined(__clang__)
inline uint16_t big16(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t big32(uint32_t v) { return __builtin_bswap32(v); }
#else
inline uint16_t big16(uint16_t v) { return uint16_t((v << 8) | (v >> 8)); }
inline uint32_t big32(uint32_t v) {
    return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}
#endif

// =============================================================================
// GameConfig - Configuration for game-specific parameters
//
// Passed to init_low_memory() so that the runtime can initialize the emulated
// boot state with the correct game ID, company code, and memory layout for
// the target game. This replaces hardcoded game-specific values.
// =============================================================================
struct GameConfig {
    // 4-character game ID (e.g. "GZLE" for Wind Waker US, "GGPE" for MKA Arcade GP)
    char game_id[5] = {0};

    // 2-character company/maker code (e.g. "01" for Nintendo)
    char company_code[3] = {0};

    // Disc number (0 for single-disc games, 0/1 for multi-disc)
    uint8_t disc_number = 0;

    // Disc version
    uint8_t disc_version = 0;

    // Physical RAM size in bytes. Defaults to standard GameCube (24 MB).
    // Override for variant hardware:
    //   - GameCube:  24 MB (0x01800000) — standard retail/devkit
    //   - Triforce:  48 MB (0x03000000) — arcade board (Sega DIMM expansion)
    //   - Wii MEM1:  24 MB (0x01800000) — Wii backward-compat mode
    uint32_t ram_size = 24 * 1024 * 1024;

    // Console type written to OS_CONSOLE_TYPE in low memory.
    // Affects how the game initializes video, I/O, and debug output.
    // Common values (from gc_hw.h):
    //   0x00000001 — Retail GameCube
    //   0x10000002 — Devkit (NPDP/GDEV)
    //   0x10000004 — Triforce arcade board
    uint32_t console_type = 0x00000001;  // OS_CONSOLE_RETAIL

    // Initial arena bounds. The game's OSInit will adjust these after DOL loading.
    // These defaults cover a typical GameCube layout; override per-game/platform.
    uint32_t arena_lo = 0x80400000;
    uint32_t arena_hi = 0x81700000;

    // Maximum size of the disc's file system table (boot.bin offset 0x42C, see disc_fst_max_size()). On a console the
    // apploader loads the FST at the very top of RAM and the SDK's OSInit takes the arena's high bound from it:
    // arena_hi = (RAM top - fst_max_size) rounded down to 32 bytes. When non-zero, init_low_memory() derives arena_hi
    // that way and `arena_hi` above is ignored. Games size their heaps from OSGetArenaHi(): a fixed value that is
    // too low (the 0x81700000 above) leaves them up to a megabyte short (docs/DEBUGGING.md, "Heap exhaustion").
    uint32_t fst_max_size = 0;
};

// =============================================================================
// PPCContext - PowerPC (Gekko) CPU Context
//
// Mirrors the state of the GameCube's Gekko processor. Recompiled code reads
// and writes fields of this struct instead of using actual PowerPC registers.
// The struct is aligned to 16 bytes for SIMD compatibility on the host.
//
// Register conventions (Metrowerks CodeWarrior / Dolphin SDK ABI):
//   r0       : volatile scratch
//   r1       : stack pointer (grows downward)
//   r2       : small data area (SDA2) base (read-only after init)
//   r3-r4    : return value (r3:r4 for 64-bit returns)
//   r3-r10   : integer arguments
//   r11-r12  : volatile scratch
//   r13      : small data area (SDA) base (read-only after init)
//   r14-r31  : non-volatile (callee-saved)
//   f0       : volatile scratch
//   f1       : float/double return value; first FP argument
//   f2-f8    : FP arguments
//   f9-f13   : volatile scratch
//   f14-f31  : non-volatile (callee-saved)
//
// Reference: PowerPC Microprocessor Family Programming Environments Manual,
//            Metrowerks CodeWarrior for Dolphin ABI specification
// =============================================================================
struct alignas(16) PPCContext {
    // General Purpose Registers (r0-r31) -- 32-bit integer registers
    uint32_t r[32];

    // Floating Point Registers (f0-f31) -- 64-bit double-precision.
    // f[i] is also ps0 of the paired-single view of register i: there is exactly one copy of it, so
    // scalar FP instructions, ps_* instructions and psq_l/psq_st all see the same value.
    double f[32];

    // ps1 of each register (the second slot of the Gekko's paired singles). Always a single-precision value.
    float ps1[32];

    // Condition Register (8 x 4-bit fields = 32 bits total)
    // CR0-CR7, each containing LT/GT/EQ/SO bits.
    // CR0 is set by integer compare/Rc=1 instructions.
    // CR1 is set by floating-point compare/Rc=1 instructions.
    uint32_t cr;

    // Link Register -- holds the return address after a bl (branch-and-link)
    uint32_t lr;

    // Count Register -- used as a loop counter (bdnz) or indirect branch target (bctr)
    uint32_t ctr;

    // Fixed-Point Exception Register
    // Bit 31: SO (Summary Overflow, sticky)
    // Bit 30: OV (Overflow, non-sticky)
    // Bit 29: CA (Carry, set by addc/subfc/etc.)
    // Bits 0-6: byte count (for lswx/stswx)
    uint32_t xer;

    // Floating Point Status and Control Register
    // Contains exception flags, rounding mode, and FP condition bits.
    uint32_t fpscr;

    // Graphics Quantization Registers (GQR0-GQR7)
    // Control the scale and type for paired-single load/store quantized
    // instructions (psq_l, psq_st). Each GQR specifies the data type
    // (u8/s8/u16/s16/float) and a scale factor for pack/unpack.
    uint32_t gqr[8];

    // Program counter -- tracks the current emulated address for debugging
    // and indirect branch resolution. Not a real PPC SPR; maintained by
    // the recompilation runtime.
    uint32_t pc;

    // ---- Helpers ----------------------------------------------------------

    // Get a 4-bit CR field (field 0 = bits 28-31, field 7 = bits 0-3)
    uint32_t get_cr_field(int field) const {
        return (cr >> (28 - field * 4)) & 0xF;
    }
    // Set a 4-bit CR field
    void set_cr_field(int field, uint32_t val) {
        uint32_t shift = 28 - field * 4;
        cr = (cr & ~(0xF << shift)) | ((val & 0xF) << shift);
    }

    // Get a single CR bit (bit 0 = MSB, bit 31 = LSB)
    bool get_cr_bit(int bit) const {
        return (cr >> (31 - bit)) & 1;
    }
    // Set a single CR bit
    void set_cr_bit(int bit, bool val) {
        if (val) cr |= (1 << (31 - bit));
        else     cr &= ~(1 << (31 - bit));
    }

    // XER flag accessors
    bool xer_so() const { return (xer >> 31) & 1; }  // Summary Overflow (sticky)
    bool xer_ov() const { return (xer >> 30) & 1; }  // Overflow
    bool xer_ca() const { return (xer >> 29) & 1; }  // Carry
    void set_xer_ca(bool v) { if (v) xer |= (1 << 29); else xer &= ~(1 << 29); }
    void set_xer_ov(bool v) {
        if (v) { xer |= (1 << 30) | (1 << 31); } // OV also sets SO (sticky)
        else   { xer &= ~(1 << 30); }
    }

    // Update CR0 after an integer operation with Rc=1
    // Sets LT/GT/EQ based on the signed result, copies SO from XER.
    void update_cr0(int32_t result) {
        uint32_t val = 0;
        if (result < 0)       val = 0x8; // LT
        else if (result > 0)  val = 0x4; // GT
        else                  val = 0x2; // EQ
        if (xer_so())         val |= 0x1; // SO
        set_cr_field(0, val);
    }

    // Initialize to power-on state (all registers zeroed).
    // The stack pointer (r1), SDA bases (r2, r13), and other registers
    // will be set by the DOL loader before the game's entry point is called.
    void reset() {
        memset(this, 0, sizeof(*this));
    }
};

// =============================================================================
// Memory - Big-endian emulated Gekko RAM
//
// Emulates the RAM accessible via two virtual address ranges:
//   0x80000000 - 0x80xxxxxx  (cached)
//   0xC0000000 - 0xC0xxxxxx  (uncached, same physical memory)
//
// Both ranges map to the same backing buffer. All reads and writes use
// big-endian byte order to match the PowerPC's native endianness.
//
// The RAM size is configurable at init() time to support different
// Gekko-based platforms:
//   - GameCube:  24 MB (default)
//   - Triforce:  48 MB (Sega DIMM board expansion)
//   - Wii MEM1:  24 MB
//
// Reference: libogc memory map, Pureikyubu memory subsystem
// =============================================================================
struct Memory {
    // Default RAM size (GameCube standard). Can be overridden via init().
    static constexpr uint32_t DEFAULT_RAM_SIZE = 24 * 1024 * 1024;  // 24 MB
    static constexpr uint32_t MAIN_RAM_BASE    = 0x80000000;        // Cached base
    static constexpr uint32_t UNCACHED_BASE    = 0xC0000000;        // Uncached mirror
    static constexpr uint32_t HW_REG_BASE     = 0xCC000000;        // Hardware registers
    static constexpr uint32_t HW_REG_SIZE     = 0x00010000;        // 64 KB of HW regs

    // Backward-compatible alias — existing code using Memory::MAIN_RAM_SIZE
    // continues to compile. For runtime-configured sizes, use the ram_size field.
    static constexpr uint32_t MAIN_RAM_SIZE    = DEFAULT_RAM_SIZE;

    // Actual RAM size and derived end address (set by init())
    uint32_t ram_size = DEFAULT_RAM_SIZE;
    uint32_t ram_end  = MAIN_RAM_BASE + DEFAULT_RAM_SIZE;

    // Host-side backing buffer for emulated RAM
    uint8_t* ram = nullptr;
    // Hardware register space (absorbs reads/writes to 0xCC000000-0xCC00FFFF)
    uint8_t hw_regs[HW_REG_SIZE] = {};

    // Allocate the backing buffer. Defaults to 24 MB (standard GameCube).
    // Pass a larger size for Triforce (48 MB) or other variants.
    bool init(uint32_t size = DEFAULT_RAM_SIZE);
    // Free the backing buffer.
    void shutdown();

    // Translate a GameCube virtual address to a host pointer.
    // Handles both cached (0x80xxxxxx) and uncached (0xC0xxxxxx) ranges.
    //
    // The non-const overload is the one that gets written through: it records the RAM page of `addr` as written
    // (see "Write stamps" below). Code that only reads through the pointer uses the const overload (read_ptr).
    uint8_t* translate(uint32_t addr);
    const uint8_t* translate(uint32_t addr) const;
    const uint8_t* read_ptr(uint32_t addr) const { return translate(addr); }

    // ---- Write stamps ---------------------------------------------------------------------------------------
    // Which 4 KB pages of RAM were written since a consumer last looked, so a cache of data derived from guest
    // memory (the GP's decoded textures) need not re-read all of it to find out that nothing changed.
    // Every store through write8/16/32/64 and every write through the non-const translate() stamps its page with
    // `write_clock`; a consumer that validated a range at clock C asks written_since(addr, len, C) later. Code that
    // writes guest RAM through a host pointer it keeps (memcpy into a buffer, DMA) must call mark_written() for the
    // whole range. A page is stamped with the clock's value at the store; advance_clock() starts a new epoch.
    static constexpr uint32_t PAGE_SHIFT = 12;
    uint32_t write_clock = 1;
    uint32_t* page_stamp = nullptr;  // one per page of RAM
    // Starts a new epoch and returns its clock: stores from now on compare >= the returned value.
    uint32_t advance_clock() { return ++write_clock; }
    // Stamps the pages of [addr, addr+len) (any guest address of RAM; others are ignored).
    void mark_written(uint32_t addr, uint32_t len);
    // True when a store may have touched [addr, addr+len) at or after `clock` (also when the range is not RAM).
    bool written_since(uint32_t addr, uint32_t len, uint32_t clock) const;

    // Big-endian accessors (match GameCube memory byte order).
    //
    // The common case, an address of main RAM through the cached mirror (0x80000000..), is handled inline: count the
    // operation (the 256K-op interrupt poll, see tick_slow), swap bytes, load or store. Everything else (the uncached
    // and physical mirrors, hardware registers, the DSP, the write-gather pipe, a watched range, an address outside RAM)
    // takes the out-of-line *_slow path, which is the general implementation.
    GCRECOMP_FORCE_INLINE uint8_t read8(uint32_t addr) const {
        const uint32_t off = addr - MAIN_RAM_BASE;
        if (GCRECOMP_LIKELY(off < ram_size)) {
            tick(addr);
            guard_read(off, 1);
            return ram[off];
        }
        return read8_slow(addr);
    }
    GCRECOMP_FORCE_INLINE uint16_t read16(uint32_t addr) const {
        const uint32_t off = addr - MAIN_RAM_BASE;
        if (GCRECOMP_LIKELY(off < ram_size)) {
            tick(addr);
            guard_read(off, 2);
            uint16_t v;
            std::memcpy(&v, ram + off, 2);
            return big16(v);
        }
        return read16_slow(addr);
    }
    GCRECOMP_FORCE_INLINE uint32_t read32(uint32_t addr) const {
        const uint32_t off = addr - MAIN_RAM_BASE;
        if (GCRECOMP_LIKELY(off < ram_size)) {
            tick(addr);
            guard_read(off, 4);
            uint32_t v;
            std::memcpy(&v, ram + off, 4);
            return big32(v);
        }
        return read32_slow(addr);
    }
    uint64_t read64(uint32_t addr) const { return (uint64_t(read32(addr)) << 32) | read32(addr + 4); }
    float readf32(uint32_t addr) const {
        const uint32_t v = read32(addr);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    double readf64(uint32_t addr) const {
        const uint64_t v = read64(addr);
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }

    GCRECOMP_FORCE_INLINE void write8(uint32_t addr, uint8_t val) {
        const uint32_t off = addr - MAIN_RAM_BASE;
        if (GCRECOMP_LIKELY(off < ram_size && watch_count == 0)) {
            tick(addr);
            page_stamp[off >> PAGE_SHIFT] = write_clock;
            ram[off] = val;
            return;
        }
        write8_slow(addr, val);
    }
    GCRECOMP_FORCE_INLINE void write16(uint32_t addr, uint16_t val) {
        const uint32_t off = addr - MAIN_RAM_BASE;
        if (GCRECOMP_LIKELY(off < ram_size && watch_count == 0)) {
            tick(addr);
            page_stamp[off >> PAGE_SHIFT] = write_clock;
            if (GCRECOMP_UNLIKELY((off & 0xFFF) == 0xFFF)) page_stamp[(off + 1) >> PAGE_SHIFT] = write_clock;
            const uint16_t v = big16(val);
            std::memcpy(ram + off, &v, 2);
            return;
        }
        write16_slow(addr, val);
    }
    GCRECOMP_FORCE_INLINE void write32(uint32_t addr, uint32_t val) {
        const uint32_t off = addr - MAIN_RAM_BASE;
        if (GCRECOMP_LIKELY(off < ram_size && watch_count == 0)) {
            tick(addr);
            page_stamp[off >> PAGE_SHIFT] = write_clock;
            if (GCRECOMP_UNLIKELY((off & 0xFFF) > 0xFFC)) page_stamp[(off + 3) >> PAGE_SHIFT] = write_clock;
            const uint32_t v = big32(val);
            std::memcpy(ram + off, &v, 4);
            return;
        }
        write32_slow(addr, val);
    }
    void write64(uint32_t addr, uint64_t val) {
        if (GCRECOMP_LIKELY(addr - MAIN_RAM_BASE < ram_size)) {
            write32(addr, uint32_t(val >> 32));
            write32(addr + 4, uint32_t(val));
            return;
        }
        write64_slow(addr, val);
    }
    void writef32(uint32_t addr, float val) {
        uint32_t v;
        std::memcpy(&v, &val, 4);
        write32(addr, v);
    }
    void writef64(uint32_t addr, double val) {
        uint64_t v;
        std::memcpy(&v, &val, 8);
        write64(addr, v);
    }

    // The general paths (see above); public only because the inline ones call them.
    uint8_t read8_slow(uint32_t addr) const;
    uint16_t read16_slow(uint32_t addr) const;
    uint32_t read32_slow(uint32_t addr) const;
    void write8_slow(uint32_t addr, uint8_t val);
    void write16_slow(uint32_t addr, uint16_t val);
    void write32_slow(uint32_t addr, uint32_t val);
    void write64_slow(uint32_t addr, uint64_t val);
    // Every 256K memory operations (counted by tick, and by translate on the slow path): the heartbeat line, and the
    // running guest thread takes the interrupts that have come due (alarms, the VI retrace, the DSP and the AI DMA), as a
    // game spinning on a flag one of their handlers sets never reaches an OS call. Cheap when nothing is due.
    void tick_slow(uint32_t addr) const;
    GCRECOMP_FORCE_INLINE void tick(uint32_t addr) const {
        if (GCRECOMP_UNLIKELY((++bench::g_mem_ops & 0x3FFFF) == 0)) tick_slow(addr);
    }

    // Read guard: up to 8 ranges of RAM (offsets from the start of RAM) whose bytes the GP module has not written yet (an EFB copy it
    // keeps on the GPU, see docs/BENCHMARK.md). A read that touches one calls read_guard_hook(offset, length) first; the hook makes
    // the bytes valid and updates the ranges with set_read_guard(). A store is not guarded: the write stamps tell the GP module
    // about it. Costs one test of guard_count per read when nothing is guarded.
    uint32_t guard_count = 0;
    uint32_t guard_lo[8] = {};
    uint32_t guard_hi[8] = {};  // exclusive
    void (*read_guard_hook)(uint32_t off, uint32_t len) = nullptr;
    void set_read_guard(const uint32_t* lo, const uint32_t* hi, uint32_t count) {
        for (uint32_t i = 0; i < count && i < 8; ++i) {
            guard_lo[i] = lo[i];
            guard_hi[i] = hi[i];
        }
        guard_count = count < 8 ? count : 8;
    }
    void guard_read_slow(uint32_t off, uint32_t len) const;
    GCRECOMP_FORCE_INLINE void guard_read(uint32_t off, uint32_t len) const {
        if (GCRECOMP_UNLIKELY(guard_count != 0)) guard_read_slow(off, len);
    }

    // Write watch (GCRECOMP_WATCH=0xADDR:LEN[,0xADDR:LEN...], read by init()): every store that overlaps one of the
    // ranges prints a line with the value and the host call chain (see tools/guest-watch.py). A store costs one
    // test of watch_count when nothing is watched.
    uint32_t watch_count = 0;
    uint32_t watch_lo[8] = {};
    uint32_t watch_hi[8] = {};  // exclusive
    void watch_hit(uint32_t addr, uint32_t len, uint64_t value);
private:
    uint8_t* translate_impl(uint32_t addr);
    void mark_host(const uint8_t* p) {
        const size_t off = size_t(p - ram);
        if (off < ram_size) page_stamp[off >> PAGE_SHIFT] = write_clock;
    }
public:
    void watch_check(uint32_t addr, uint32_t len, uint64_t value) {
        if (watch_count) watch_hit(addr, len, value);
    }
};

// =============================================================================
// FuncTable - Maps GameCube addresses to native recompiled functions
//
// When recompiled code encounters an indirect call (function pointer, virtual
// method dispatch, switch jump table), it looks up the target GameCube address
// in this table to find the corresponding native function. The recompiler
// populates this table at startup with every recompiled function's address.
//
// OS HLE functions are also registered here so that calls to SDK functions
// (e.g. OSAlloc, DVDOpen) are transparently redirected to native replacements.
// =============================================================================
using RecompiledFunc = void(*)(PPCContext* ctx, Memory* mem);

struct FuncTable {
    // Address-to-function mapping
    std::unordered_map<uint32_t, RecompiledFunc> table;

    // Register a recompiled function at the given GameCube address
    void register_func(uint32_t gc_addr, RecompiledFunc func);

    // Look up a function by GameCube address. Returns nullptr if not found.
    RecompiledFunc lookup(uint32_t gc_addr) const;

    // Call a function by its GameCube address. Logs an error if unresolved.
    void call(uint32_t gc_addr, PPCContext* ctx, Memory* mem) const;
};

// =============================================================================
// REL modules (see docs/REL_MODULES.md)
//
// Code that the game loads from the disc at run time (Dolphin SDK relocatable modules, OSLink) is recompiled to
// position-independent C++. The generated registration unit hands one RelModuleDesc per module to
// register_rel_module() from a static initialiser. When a call reaches an address that is in neither the DOL
// function table nor an active module, FuncTable::call asks rel_lookup(): it finds the module the guest linked
// there (the OSModuleInfo list at 0x800030C8), recognises it by its signature, reads its run-time section bases
// into `base` (the generated code adds them to every address it computes) and returns the function.
// =============================================================================
struct RelFunc {
    uint32_t       section;
    uint32_t       offset;     // inside the section
    RecompiledFunc fn;
};

struct RelModuleDesc {
    const char*     name;
    uint32_t        version;
    uint32_t        num_sections;
    const uint32_t* section_size;  // per section, as in the file (BSS included)
    uint32_t        exec_mask;     // bit i set when section i is executable
    uint32_t*       base;          // per section run-time address, written on activation, read by the generated code
    const RelFunc*  funcs;
    uint32_t        num_funcs;
};

// Guest address of __OSModuleInfoHead (the head of the OSModuleInfo list, tail at +4).
constexpr uint32_t OS_MODULE_INFO_HEAD = 0x800030C8;

void register_rel_module(const RelModuleDesc* desc);

// The function of a linked, recompiled module at `gc_addr`; nullptr when no such module is linked there or the address
// is not an entry the recompiler found. Activates the module on first use.
RecompiledFunc rel_lookup(uint32_t gc_addr);

// Run-time base of section `section` of the linked module with id `module_id` (0 when it is not linked). Generated code
// uses it for relocations against a module other than itself and the DOL.
uint32_t rel_import_base(uint32_t module_id, uint32_t section);

// Forget every active module overlapping [addr, addr+size): ICInvalidateRange, the guest rewrote that memory.
void rel_invalidate_range(uint32_t addr, uint32_t size);

// "inside module X section S +0xOFF" for an address in a linked module, empty otherwise (diagnostics).
std::string rel_describe(uint32_t gc_addr);

// Forgets every activation, keeping the registered modules (tests that relink a module).
void rel_reset_active();

// -----------------------------------------------------------------------------
// Loud failures (src/runtime/diagnostics.cpp, docs/DEBUGGING.md)
// -----------------------------------------------------------------------------
// Where the game's SDK keeps its OSAlloc state: offsets from r13 of `HeapArray`, `NumHeaps`, `__OSCurrHeap`.
struct GuestHeapLayout {
    int32_t array_off;
    int32_t num_off;
    int32_t curr_off;
};
void set_guest_heap_layout(const GuestHeapLayout& layout);  // or GCRECOMP_GUEST_HEAP=ARRAY_OFF:NUM_OFF:CURR_OFF
bool guest_heap_known();
// Prints every heap of the guest SDK: size, free and allocated cells and bytes (verbose: the biggest blocks).
void dump_guest_heaps(const char* why, const PPCContext* ctx, bool verbose);
// Prints LR and the saved return addresses up the guest stack.
void print_guest_backtrace(const PPCContext* ctx);
// Flushes the logs, prints "[FATAL] reason" and exits with status 70.
[[noreturn]] void fatal_stop(const char* reason);
// Stop after this many unresolved calls to one address (default 1000, env GCRECOMP_UNRESOLVED_LIMIT, 0 = never).
void set_unresolved_limit(unsigned limit);
// Tests: a handler called by fatal_stop before it exits (it may throw); nullptr restores the plain exit.
void set_fatal_handler(void (*handler)(const char* reason));
// Prints the guest module list, the recompiled modules and the activations (diagnostics).
void rel_dump_state();

// Last-dispatch probe, for diagnosing hangs.
//
// call() records every dispatch here. A watchdog can sample this: if the
// returned sequence number stops moving, the game is spinning inside one
// recompiled function and `addr` is the last one entered; if it keeps moving
// but `addr` repeats, it is looping over a call.
// Returns the dispatch sequence number; `addr`/`lr` may be null.
uint64_t last_dispatch(uint32_t* addr, uint32_t* lr);

// Debug facilities that are timed (GCRECOMP_WATCH, GCRECOMP_TRACE_CALLS) only act once GCRECOMP_DEBUG_AFTER seconds
// of wall-clock time have passed since the first call (default 0: always), to skip a boot phase or an earlier scene.
bool debug_armed();
// Appends " bt: OFF OFF ..." (the host call chain as offsets in the executable, innermost first) to `out`; no newline.
// tools/guest-watch.py names the offsets (guest functions of a recompiled game).
void print_call_chain(std::FILE* out);

// =============================================================================
// Global runtime state
//
// These globals are the single instances of the CPU context, memory, and
// function table used throughout the recompilation. They are initialized
// by runtime_init() and torn down by runtime_shutdown().
// =============================================================================
extern PPCContext g_ctx;
extern Memory     g_mem;

// Sink for stores to the write-gather pipe (0xCC008000), where the SDK's GX code writes its command
// stream. Installed by the GP module; stores are absorbed by the hardware register space otherwise.
// `size` is 1, 2, 4 or 8 bytes and `value` holds them big-endian.
extern void (*g_wgp_write)(uint32_t size, uint64_t value);
// Called by the GXDrawDone replacement: the GP module writes what it keeps on the GPU to guest memory (null: nothing to do).
extern void (*g_gp_sync)();

// Called at every VI retrace, on the thread that takes the interrupt (window events are pumped here).
extern void (*g_retrace_hook)();

// Delivers the audio interface's pending DMA interrupt (calls the game's AI callback) if it is due. Called at the
// points where the game could have been interrupted: re-enabling interrupts, clock reads.
void ai_poll(PPCContext* ctx, Memory* mem);

// Delivers the DSP block's pending ARAM / DSP interrupt (calls the handler the game registered with
// __OSSetInterruptHandler) at the same points as ai_poll. See hw/dsp.h.
void dsp_poll(PPCContext* ctx, Memory* mem);

// Drops all AI state and makes the AI replacements play into `sink` (not owned; null restores the default
// choice of SDL3 or the null sink). For tests.
namespace audio { class AudioSink; }
void ai_hle_reset(audio::AudioSink* sink);

// Per-function counters of calls that reached a native replacement, for diagnosing what a game is doing.
void hle_count(const char* name);
void hle_dump(size_t top);  // prints the most called replacements, most first

// State of a GameCube controller as the PAD replacements report it. Updated by the window layer.
struct PadState {
    uint16_t buttons = 0;  // PAD_BUTTON_* bits: LEFT 1, RIGHT 2, DOWN 4, UP 8, Z 0x10, R 0x20, L 0x40, A 0x100, B 0x200, X 0x400, Y 0x800, START 0x1000
    int8_t stick_x = 0, stick_y = 0, substick_x = 0, substick_y = 0;
    uint8_t trigger_l = 0, trigger_r = 0;
    bool connected = false;  // PADRead reports PAD_ERR_NO_CONTROLLER for the port when false
    bool rumble = false;     // set by PADControlMotor, read by the window layer to drive the motor
};
extern PadState g_pad[4];
extern FuncTable  g_func_table;

// =============================================================================
// Tracing — record the last N recompiled function entries
//
// When the recompiler is run with --trace, every generated function begins
// with TRACE_ENTER(0xADDR) which calls gcrecomp::trace_enter. The ring
// buffer is lock-free (atomic counter, 4096 entries, power-of-two mask).
// Disabled by default — flip with trace_set_enabled(true) before invoking
// recompiled code.
//
// trace_dump_recent(n) prints the last n entries in chronological order.
// trace_dump_summary() prints a histogram of the most-called functions in
// the ring, useful for diagnosing tight loops.
// =============================================================================
struct TraceRing {
    static constexpr size_t SIZE = 4096; // power of two — fast mask
    uint32_t entries[SIZE]{};
    std::atomic<uint64_t> pos{0};
};

extern TraceRing  g_trace_ring;
extern std::atomic<bool> g_trace_enabled;

// Out-of-line definition (not inline) so the compiler can't elide the call
// or fold trace_enter chains across recompiled-code TUs. The atomic load
// cost is dominated by the call overhead at this point.
void trace_enter(uint32_t addr);

void trace_set_enabled(bool on);
void trace_dump_recent(size_t n);
void trace_dump_summary();

// Initialize the runtime: allocate memory, reset CPU state, set up OS
// low-memory globals, and register OS HLE function replacements.
// Returns false on failure (e.g. memory allocation failed).
bool runtime_init();

// Shut down the runtime: free emulated memory.
void runtime_shutdown();

// =============================================================================
// OS HLE (High-Level Emulation) Interface
//
// These functions provide native implementations of Dolphin OS / SDK functions
// so that recompiled game code can call them transparently.
// =============================================================================

// Initialize Dolphin OS low-memory globals (game ID, clock speeds, arena
// bounds, console type, boot magic, main thread). Must be called before
// the game's OSInit or main entry point.
void init_low_memory(Memory* mem, const GameConfig& config);

// The arena's high bound of a console that boots a disc whose FST may be `fst_max_size` bytes (boot.bin 0x42C): the
// apploader puts the FST at the top of RAM and OSInit starts the arena just below it.
uint32_t arena_hi_for_fst(uint32_t ram_size, uint32_t fst_max_size);

// Reads the FST maximum size from a disc's boot.bin (sys/boot.bin of an extracted disc); 0 when the file cannot be read.
uint32_t disc_fst_max_size(const char* boot_bin_path);

// The video standard (hw::OS_TV_FORMAT_*) of a console that plays a disc of this region: the fourth character of the game
// ID (E USA, J Japan, K Korea, W Taiwan: NTSC; P, D, F, I, S, H, U, X, Y, Z, R: PAL). init_low_memory() stores it.
uint32_t os_tv_format_for_region(char region);

// Register all OS HLE function replacements into an internal name-to-function
// lookup table. Called during runtime_init().
void register_os_functions();

// Look up an OS HLE function by its SDK symbol name (e.g. "OSAlloc",
// "DVDOpen", "memcpy"). Returns nullptr if the name is not a known OS
// function. Used by the DOL/symbol-map loader to bind addresses.
RecompiledFunc lookup_os_func(const char* name);

// Set the root directory for game file access. DVD file operations
// (DVDOpen, DVDReadPrio, etc.) will resolve game paths relative to this
// directory on the host filesystem.
void set_game_root(const std::string& path);

// Moves the paths of the disc files the game opened (DVDOpen, DVDFastOpen; as the game wrote them)
// since the last call into `out`, oldest first. Lets a host program (the scripted input) wait for the
// game to open a file.
void take_opened_disc_files(std::vector<std::string>& out);

// Memory card in slot A. The card is reported only while `g_card_inserted` is true and an image path
// is set. `path` is a raw memory card image (no header; 0x80000, 0x100000 or 0x200000 bytes). A
// missing file starts as a blank, unformatted card of `size_bytes` (the game formats it) and is
// created on the first change. Saves are stored in the console's own format inside this image.
extern bool g_card_inserted;
void set_card_image(const std::string& path, size_t size_bytes = 0x200000);
// Run the callbacks of asynchronous CARD calls that completed since the last time. The SDK runs them
// from an interrupt after the call returned; here they run when the game polls the card or waits for
// a retrace (VIWaitForRetrace calls this).
void card_run_pending_callbacks(PPCContext* ctx, Memory* mem);
// The same for DVDReadAsync / DVDReadAsyncPrio: the data is in memory when the call returns, the callback runs
// afterwards (from the retrace interrupt, or when the game polls DVDGetDriveStatus / DVDGetCommandBlockStatus).
void dvd_run_pending_callbacks(PPCContext* ctx, Memory* mem);

// Mount a GameCube ISO disc image. Parses the disc header, loads the FST
// (File System Table) into emulated memory, and enables DVD reads directly
// from the ISO file. The FST is placed at the top of emulated RAM and
// OS_FST_ADDR (0x80000038) is set to point to it.
// Returns true on success, false if the ISO can't be opened or parsed.
bool mount_disc_image(const char* iso_path, Memory* mem);

// Read raw data from the mounted disc image at a given byte offset.
// Used internally by DVD HLE functions. Returns bytes read (0 on failure).
size_t disc_read(uint32_t disc_offset, void* dst, size_t length);

// Check if a disc image is currently mounted.
bool is_disc_mounted();

} // namespace gcrecomp
