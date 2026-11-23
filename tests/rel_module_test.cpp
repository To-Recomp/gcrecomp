// SPDX-License-Identifier: MIT
//
// REL modules at run time (docs/REL_MODULES.md). The synthetic module of rel_synth.h was recompiled by
// gcrecomp_recompiler at build time and is linked into this test. The test plays the part of the SDK's OSLink (its own
// implementation of the relocation semantics, written from the format description, not the recompiler's) at two
// different sets of section addresses and runs every function through the function table, as the game does. Every
// result must be right at both: nothing in the generated code may depend on where the module was linked.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gcrecomp/runtime.h"
#include "rel_synth.h"

using namespace gcrecomp;

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        if ((long long)(a) != (long long)(b)) {                                                                     \
            std::printf("%s:%d: %s = 0x%llX, expected 0x%llX\n", __FILE__, __LINE__, #a, (long long)(a), (long long)(b)); \
            ++g_failures;                                                                                           \
        }                                                                                                           \
    } while (0)

static const relsynth::Module g_module = relsynth::build();

struct Layout {
    uint32_t info;                      // the OSModuleInfo of the module
    uint32_t base[relsynth::kNumSections];
};

// The other module the synthetic one imports from: only its section bases matter.
static constexpr uint32_t kOtherInfo = 0x80050000;
static constexpr uint32_t kOtherBase[3] = {0, 0x80060000, 0x80061000};

static const Layout kLayout1 = {0x80100000, {0, 0x80110000, 0x80118000, 0x80120000, 0x80140000}};
// A different order and different alignment: bss, text2, text, data. The data base makes the HA/LO halves of
// data + 0x8010 fall on the other side of the carry than in layout 1.
static const Layout kLayout2 = {0x80300000, {0, 0x80320000, 0x80318000, 0x8032C000, 0x80310020}};

constexpr uint32_t kListHead = OS_MODULE_INFO_HEAD;
constexpr uint32_t kListTail = OS_MODULE_INFO_HEAD + 4;

static void write_node(uint32_t info, uint32_t id, uint32_t num_sections, uint32_t table, uint32_t version) {
    g_mem.write32(info + 0x00, id);
    g_mem.write32(info + 0x04, 0);
    g_mem.write32(info + 0x08, 0);
    g_mem.write32(info + 0x0C, num_sections);
    g_mem.write32(info + 0x10, table);
    g_mem.write32(info + 0x1C, version);
}

static void append_node(uint32_t info) {
    const uint32_t tail = g_mem.read32(kListTail);
    g_mem.write32(info + 0x08, tail);
    g_mem.write32(info + 0x04, 0);
    if (tail) g_mem.write32(tail + 4, info); else g_mem.write32(kListHead, info);
    g_mem.write32(kListTail, info);
}

static void remove_node(uint32_t info) {
    const uint32_t next = g_mem.read32(info + 4), prev = g_mem.read32(info + 8);
    if (prev) g_mem.write32(prev + 4, next); else g_mem.write32(kListHead, next);
    if (next) g_mem.write32(next + 8, prev); else g_mem.write32(kListTail, prev);
}

// The SDK's Link + Relocate for this module: copy the sections to their addresses, rewrite the node, patch the places.
static void link_module(const Layout& l) {
    using namespace relsynth;
    for (uint32_t i = 1; i < kNumSections; i++) {
        for (uint32_t b = 0; b < g_module.section_size[i]; b++)   // data from the file, zeroes for BSS
            g_mem.write8(l.base[i] + b, g_module.section_data[i].empty() ? 0 : g_module.section_data[i][b]);
    }
    const uint32_t table = l.info + 0x100;
    write_node(l.info, kModuleId, kNumSections, table, 3);
    for (uint32_t i = 0; i < kNumSections; i++) {
        g_mem.write32(table + 8 * i, (l.base[i] ? l.base[i] : 0) | (g_module.section_exec[i] ? 1u : 0u));
        g_mem.write32(table + 8 * i + 4, g_module.section_size[i]);
    }
    append_node(l.info);

    for (const Reloc& r : g_module.relocs) {
        const uint32_t s = r.module == 0 ? 0 : r.module == kModuleId ? l.base[r.target_section] : kOtherBase[r.target_section];
        const uint32_t value = s + r.addend;
        const uint32_t place = l.base[r.section] + r.offset;
        switch (r.type) {
        case R_ADDR32: g_mem.write32(place, value); break;
        case R_ADDR24: g_mem.write32(place, (g_mem.read32(place) & ~0x03FFFFFCu) | (value & 0x03FFFFFCu)); break;
        case R_ADDR16: g_mem.write16(place, (uint16_t)value); break;
        case R_LO: g_mem.write16(place, (uint16_t)value); break;
        case R_HI: g_mem.write16(place, (uint16_t)(value >> 16)); break;
        case R_HA: g_mem.write16(place, (uint16_t)((value + 0x8000u) >> 16)); break;
        case R_ADDR14:
        case R_ADDR14_BRTAKEN:
        case R_ADDR14_BRNTAKEN: g_mem.write32(place, (g_mem.read32(place) & ~0xFFFCu) | (value & 0xFFFCu)); break;
        case R_REL24: g_mem.write32(place, (g_mem.read32(place) & ~0x03FFFFFCu) | ((value - place) & 0x03FFFFFCu)); break;
        case R_REL14: g_mem.write32(place, (g_mem.read32(place) & ~0xFFFCu) | ((value - place) & 0xFFFCu)); break;
        default: std::printf("bad relocation type %u\n", r.type); ++g_failures; break;
        }
    }
}

// OSUnlink and the heap reusing the memory.
static void unlink_module(const Layout& l) {
    remove_node(l.info);
    for (uint32_t i = 1; i < relsynth::kNumSections; i++)
        for (uint32_t b = 0; b < g_module.section_size[i]; b++) g_mem.write8(l.base[i] + b, 0xAA);
    for (uint32_t b = 0; b < 0x140; b++) g_mem.write8(l.info + b, 0xAA);
}

static uint32_t g_dol_lr = 0;
static void dol_double(PPCContext* ctx, Memory*) { g_dol_lr = ctx->lr; ctx->r[3] = ctx->r[3] * 2 + 5; }
static void dol_add1000(PPCContext* ctx, Memory*) { ctx->r[3] += 1000; }
static void dol_cond_a(PPCContext* ctx, Memory*) { ctx->r[3] = 1001; }
static void dol_cond_b(PPCContext* ctx, Memory*) { ctx->r[3] = 1002; }
static void dol_cond_c(PPCContext* ctx, Memory*) { ctx->r[3] = 1003; }

static PPCContext g_last;

static uint32_t call(uint32_t addr, uint32_t r3 = 0) {
    PPCContext ctx = {};
    ctx.r[1] = 0x80700000;
    ctx.r[3] = r3;
    ctx.lr = 0xDEAD0000;
    g_func_table.call(addr, &ctx, &g_mem);
    g_last = ctx;
    return ctx.r[3];
}

// Runs every function of the module linked at `l`; the values that do not depend on the addresses are appended to
// `portable` so the two layouts can be compared.
static void run_all(const Layout& l, std::vector<int64_t>& portable) {
    using namespace relsynth;
    const auto& off = g_module.off;
    const uint32_t text = l.base[1], text2 = l.base[2], data = l.base[3];
    auto at = [&](const char* name) { return text + off.at(name); };

    CHECK_EQ(call(at("addr_pair")), data + 0x8010);
    portable.push_back((int64_t)call(at("addr_pair")) - data);
    CHECK_EQ(call(at("load_global")), 0x12345678);
    call(at("store_global"), 0xCAFE);
    CHECK_EQ(g_mem.read32(data + 0x24), 0xCAFE);
    CHECK_EQ(call(at("hi_ori")), data + 0x30);
    CHECK_EQ(call(at("plain16")), (uint32_t)(int32_t)(int16_t)((data + 0xF0) & 0xFFFF));
    call(at("load_float"));
    CHECK(g_last.f[1] == 1.5);
    CHECK_EQ(call(at("import_pair")), kOtherBase[2] + 0x10);
    portable.push_back(call(at("import_pair")) - kOtherBase[2]);

    CHECK_EQ(call(at("call_cross"), 100), 42);
    portable.push_back(call(at("call_cross"), 100));

    g_dol_lr = 0;
    CHECK_EQ(call(at("call_dol")), 19);
    CHECK_EQ(g_dol_lr, text + g_module.call_dol_return);   // LR is the run-time address of the return point
    portable.push_back(g_dol_lr - text);
    CHECK_EQ(call(at("call_abs24")), 1003);
    CHECK_EQ(call(at("cond_dol"), 1), 1001);
    CHECK_EQ(call(at("cond_dol"), 2), 1002);
    CHECK_EQ(call(at("cond_dol"), 3), 1003);
    CHECK_EQ(call(at("cond_dol"), 4), 0);
    CHECK_EQ(call(at("cond_dol"), 0), 0);

    for (uint32_t i = 0; i < 3; i++) {
        CHECK_EQ(call(at("switch"), i), 10 * (i + 1));
        portable.push_back(call(at("switch"), i));
    }
    CHECK_EQ(call(at("switch"), 3), 0xFFFFFFFF);
    CHECK_EQ(call(at("switch"), 0x80000000), 0xFFFFFFFF);

    // The vtable the linker filled in: entries are run-time addresses of functions only the vtable names.
    const uint32_t slot0 = g_mem.read32(data + 0x40), slot1 = g_mem.read32(data + 0x44);
    CHECK_EQ(slot0, text + off.at("vslot0"));
    CHECK_EQ(slot1, text2 + off.at("vslot1"));
    CHECK_EQ(call(slot0), 100);
    CHECK_EQ(call(slot1), 200);
    portable.push_back(call(slot0));
    portable.push_back(call(slot1));

    CHECK_EQ(call(at("cond_x"), 5), 55);
    CHECK_EQ(call(at("cond_x"), 4), 0);
    CHECK_EQ(g_mem.read32(data + 0x90), kDolDataWord);
    CHECK_EQ(g_mem.read32(data + 0xA0), l.base[4] + 0x10);

    // The text of the second section is directly callable too (the function table knows it by its run-time address).
    CHECK_EQ(call(text2 + off.at("text2_const")), 41);
}

int main() {
    using namespace relsynth;
    CHECK(g_mem.init());
    g_func_table.register_func(kDolDouble, dol_double);
    g_func_table.register_func(kDolAdd1000, dol_add1000);
    g_func_table.register_func(kDolCondA, dol_cond_a);
    g_func_table.register_func(kDolCondB, dol_cond_b);
    g_func_table.register_func(kDolCondC, dol_cond_c);

    // The list head 0x800030C8 is where the guest keeps it; the other module sits in front of ours.
    g_mem.write32(kListHead, 0);
    g_mem.write32(kListTail, 0);
    write_node(kOtherInfo, kOtherModuleId, 3, kOtherInfo + 0x100, 3);
    for (uint32_t i = 0; i < 3; i++) {
        g_mem.write32(kOtherInfo + 0x100 + 8 * i, kOtherBase[i]);
        g_mem.write32(kOtherInfo + 0x100 + 8 * i + 4, i == 0 ? 0 : 0x100);
    }
    append_node(kOtherInfo);

    // Nothing linked yet: the address is simply unresolved.
    CHECK(rel_lookup(kLayout1.base[1]) == nullptr);

    // A module whose sections do not match the recompiled one is not mistaken for it.
    {
        const uint32_t info = 0x80500000, table = info + 0x100;
        write_node(info, 11, kNumSections, table, 3);
        for (uint32_t i = 0; i < kNumSections; i++) {
            g_mem.write32(table + 8 * i, (0x80510000 + 0x1000 * i) | (i == 1 || i == 2 ? 1u : 0u));
            g_mem.write32(table + 8 * i + 4, g_module.section_size[i] + 4);
        }
        append_node(info);
        CHECK(rel_lookup(0x80511010) == nullptr);
        CHECK(rel_describe(0x80511010).find("no recompiled module") != std::string::npos);
        remove_node(info);
    }

    std::vector<int64_t> portable1, portable2, portable3;

    link_module(kLayout1);
    CHECK(rel_describe(kLayout1.base[1] + 8).find("section 1") != std::string::npos);
    CHECK(rel_lookup(kLayout1.base[1] + 2) == nullptr);   // inside the module but not an entry
    run_all(kLayout1, portable1);

    // ICInvalidateRange over the module drops the activation; the next call finds the module again.
    {
        PPCContext ctx = {};
        ctx.r[3] = kLayout1.base[1];
        ctx.r[4] = 0x100;
        RecompiledFunc ic = lookup_os_func("ICInvalidateRange");
        CHECK(ic != nullptr);
        if (ic) ic(&ctx, &g_mem);
        CHECK_EQ(call(kLayout1.base[1] + g_module.off.at("leaf"), 1), 6);
    }

    // OSUnlink and the memory reused: no stale function is served at the old addresses.
    unlink_module(kLayout1);
    CHECK(rel_lookup(kLayout1.base[1] + g_module.off.at("leaf")) == nullptr);
    CHECK(rel_lookup(kLayout1.base[2] + g_module.off.at("vslot1")) == nullptr);

    // Linked again, somewhere else, in another order.
    link_module(kLayout2);
    run_all(kLayout2, portable2);
    CHECK(portable1 == portable2);
    CHECK(rel_lookup(kLayout1.base[1] + g_module.off.at("leaf")) == nullptr);   // the old address stays dead

    // And back at the first addresses.
    unlink_module(kLayout2);
    CHECK(rel_lookup(kLayout2.base[1] + g_module.off.at("leaf")) == nullptr);
    link_module(kLayout1);
    run_all(kLayout1, portable3);
    CHECK(portable1 == portable3);

    if (g_failures == 0) std::printf("rel_module_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
