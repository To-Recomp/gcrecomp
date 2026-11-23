// SPDX-License-Identifier: MIT
//
// A synthetic REL module, built from scratch for the REL tests (no game data): a tiny assembler for the dozen
// PowerPC instructions it needs and a writer of the module format (docs/REL_MODULES.md). Shared by the generator
// that writes the file the recompiler reads at build time (rel_synth_gen.cpp), the recompiler analysis test and the
// run-time test, so all of them agree on where every function and data item is.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace relsynth {

// Relocation types (Dolphin SDK numbers).
enum : uint8_t {
    R_ADDR32 = 1, R_ADDR24 = 2, R_ADDR16 = 3, R_LO = 4, R_HI = 5, R_HA = 6, R_ADDR14 = 7, R_ADDR14_BRTAKEN = 8,
    R_ADDR14_BRNTAKEN = 9, R_REL24 = 10, R_REL14 = 11, R_NOP = 201, R_SECTION = 202, R_END = 203,
};

constexpr uint32_t kModuleId = 7;
constexpr uint32_t kOtherModuleId = 9;   // a module the synthetic one imports from (not recompiled)
constexpr uint32_t kNumSections = 5;     // 0 empty, 1 .text, 2 .text2, 3 .data, 4 .bss
constexpr uint32_t kDataSize = 0x9000;   // large enough that an offset has bit 15 set (the HA carry)
constexpr uint32_t kBssSize = 0x200;

// Guest addresses of the functions of the DOL the module calls (relocations against module 0 carry them).
constexpr uint32_t kDolDouble = 0x80004000;   // r3 = 2*r3 + 5            (REL24)
constexpr uint32_t kDolAdd1000 = 0x80004100;  // r3 += 1000               (ADDR24)
constexpr uint32_t kDolCondA = 0x80004200;    // r3 = 1001                (ADDR14)
constexpr uint32_t kDolCondB = 0x80004300;    // r3 = 1002                (ADDR14_BRTAKEN)
constexpr uint32_t kDolCondC = 0x80004400;    // r3 = 1003                (ADDR14_BRNTAKEN)
constexpr uint32_t kDolDataWord = 0x80004500; // stored in the data section by an ADDR32 against module 0

struct Reloc {
    uint32_t section;   // section holding the place
    uint32_t offset;    // the place: the immediate halfword for the 16-bit types
    uint8_t  type;
    uint32_t module;    // 0 = DOL
    uint32_t target_section;
    uint32_t addend;
};

struct Module {
    std::vector<std::vector<uint8_t>> section_data;   // empty for BSS
    std::vector<uint32_t>             section_size;
    std::vector<bool>                 section_exec;
    std::vector<Reloc>                relocs;
    std::vector<uint8_t>              file;

    // offsets of the functions (in .text, except where noted) and data items (in .data)
    std::map<std::string, uint32_t> off;
    uint32_t call_dol_return = 0;    // offset in .text of the instruction after the bl to kDolDouble
};

class Asm {
public:
    std::vector<uint32_t> w;
    uint32_t here() const { return (uint32_t)w.size() * 4; }
    void put(uint32_t v) { w.push_back(v); }
    static uint32_t d_form(uint32_t op, uint32_t rt, uint32_t ra, uint32_t imm) {
        return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF);
    }
    void lis(uint32_t rd, uint32_t imm = 0) { put(d_form(15, rd, 0, imm)); }
    void addi(uint32_t rd, uint32_t ra, int32_t imm = 0) { put(d_form(14, rd, ra, (uint32_t)imm)); }
    void li(uint32_t rd, int32_t imm) { addi(rd, 0, imm); }
    void ori(uint32_t ra, uint32_t rs, uint32_t imm = 0) { put(d_form(24, rs, ra, imm)); }
    void lwz(uint32_t rd, uint32_t ra, int32_t d = 0) { put(d_form(32, rd, ra, (uint32_t)d)); }
    void stw(uint32_t rs, uint32_t ra, int32_t d = 0) { put(d_form(36, rs, ra, (uint32_t)d)); }
    void lfs(uint32_t fd, uint32_t ra, int32_t d = 0) { put(d_form(48, fd, ra, (uint32_t)d)); }
    void cmpwi(uint32_t ra, int32_t imm) { put(d_form(11, 0, ra, (uint32_t)imm)); }
    void cmplwi(uint32_t ra, uint32_t imm) { put(d_form(10, 0, ra, imm)); }
    void blr() { put(0x4E800020); }
    void bctr() { put(0x4E800420); }
    void mflr_r0() { put(0x7C0802A6); }
    void mtlr_r0() { put(0x7C0803A6); }
    void mtctr_r0() { put(0x7C0903A6); }
    void stwu_r1(int32_t d) { put(d_form(37, 1, 1, (uint32_t)d)); }
    void rlwinm_r0_r3_2() { put(0x5460103A); }   // rlwinm r0,r3,2,0,29
    void lwzx_r0_r4_r0() { put(0x7C04002E); }
    // bl / b to an offset in this section (no relocation)
    void bl_to(uint32_t target) { put(0x48000001u | ((target - here()) & 0x03FFFFFCu)); }
    // a branch word whose displacement a relocation supplies
    void bl_reloc() { put(0x48000001u); }
    void bla_reloc() { put(0x48000003u); }
    void beq_reloc() { put(0x41820000u); }
    void bgt_placeholder() { put(0x41810000u); }
    void patch_bc(uint32_t at, uint32_t target) { w[at / 4] |= (target - at) & 0xFFFCu; }
    void prologue() { mflr_r0(); stw(0, 1, 4); stwu_r1(-16); }
    void epilogue() { lwz(0, 1, 20); addi(1, 1, 16); mtlr_r0(); blr(); }
};

inline void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}
inline void put_be32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = (uint8_t)(x >> 24); v[at + 1] = (uint8_t)(x >> 16); v[at + 2] = (uint8_t)(x >> 8); v[at + 3] = (uint8_t)x;
}

// Serialises the module: header (v3), section table, section data, import table, relocation streams.
inline void write_file(Module& m, uint32_t prolog, uint32_t epilog, uint32_t unresolved) {
    const uint32_t n = (uint32_t)m.section_size.size();
    std::vector<uint8_t>& f = m.file;
    f.assign(0x4C, 0);
    put_be32(f, 0x00, kModuleId);
    put_be32(f, 0x0C, n);
    put_be32(f, 0x10, 0x4C);
    put_be32(f, 0x1C, 3);
    put_be32(f, 0x20, kBssSize);
    f[0x30] = 1; f[0x31] = 1; f[0x32] = 1;
    put_be32(f, 0x34, prolog);
    put_be32(f, 0x38, epilog);
    put_be32(f, 0x3C, unresolved);
    put_be32(f, 0x40, 32);
    put_be32(f, 0x44, 32);
    f.resize(0x4C + 8 * n, 0);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t file_off = 0;
        if (!m.section_data[i].empty()) {
            while (f.size() % 32) f.push_back(0);
            file_off = (uint32_t)f.size();
            f.insert(f.end(), m.section_data[i].begin(), m.section_data[i].end());
        }
        put_be32(f, 0x4C + 8 * i, file_off | (m.section_exec[i] ? 1u : 0u));
        put_be32(f, 0x4C + 8 * i + 4, m.section_size[i]);
    }
    put_be32(f, 0x48, (uint32_t)f.size());   // fixSize

    // imports: the module itself, the DOL, the other module
    const uint32_t modules[3] = {kModuleId, 0, kOtherModuleId};
    while (f.size() % 8) f.push_back(0);
    const uint32_t imp_off = (uint32_t)f.size();
    f.resize(f.size() + 8 * 3, 0);
    const uint32_t rel_off = (uint32_t)f.size();
    for (int k = 0; k < 3; k++) {
        put_be32(f, imp_off + 8 * k, modules[k]);
        put_be32(f, imp_off + 8 * k + 4, (uint32_t)f.size());
        std::vector<Reloc> mine;
        for (const Reloc& r : m.relocs) {
            if (r.module == modules[k]) mine.push_back(r);
        }
        std::stable_sort(mine.begin(), mine.end(), [](const Reloc& a, const Reloc& b) {
            return a.section != b.section ? a.section < b.section : a.offset < b.offset;
        });
        auto entry = [&](uint32_t delta, uint8_t type, uint32_t section, uint32_t addend) {
            f.push_back((uint8_t)(delta >> 8)); f.push_back((uint8_t)delta); f.push_back(type); f.push_back((uint8_t)section);
            be32(f, addend);
        };
        int64_t cur_section = -1;
        uint32_t cur = 0;
        for (const Reloc& r : mine) {
            if ((int64_t)r.section != cur_section) {
                entry(0, R_SECTION, r.section, 0);
                cur_section = r.section;
                cur = 0;
            }
            uint32_t delta = r.offset - cur;
            while (delta > 0x40) {   // long gaps are crossed with NOP entries
                entry(0x20, R_NOP, 0, 0);
                delta -= 0x20;
            }
            entry(delta, r.type, r.target_section, r.addend);
            cur = r.offset;
        }
        entry(0, R_END, 0, 0);
    }
    put_be32(f, 0x24, rel_off);
    put_be32(f, 0x28, imp_off);
    put_be32(f, 0x2C, 8 * 3);
}

inline Module build() {
    Module m;
    m.section_data.assign(kNumSections, {});
    m.section_size.assign(kNumSections, 0);
    m.section_exec.assign(kNumSections, false);

    // ---- .text (1) ----
    Asm t;
    auto reloc16 = [&](uint32_t insn_off, uint8_t type, uint32_t module, uint32_t tsec, uint32_t addend) {
        m.relocs.push_back(Reloc{1, insn_off + 2, type, module, tsec, addend});
    };
    auto fn = [&](const char* name) { m.off[name] = t.here(); };

    fn("addr_pair");          // r3 = &data[0x8010]: addis/addi with HA (carry: the low half is negative) and LO
    reloc16(t.here(), R_HA, kModuleId, 3, 0x8010); t.lis(3);
    reloc16(t.here(), R_LO, kModuleId, 3, 0x8010); t.addi(3, 3);
    t.blr();

    fn("leaf");               // r3 += 5
    t.addi(3, 3, 5);
    t.blr();

    fn("load_global");        // r3 = *(u32*)&data[0x20]
    reloc16(t.here(), R_HA, kModuleId, 3, 0x20); t.lis(4);
    reloc16(t.here(), R_LO, kModuleId, 3, 0x20); t.lwz(3, 4);
    t.blr();

    fn("store_global");       // *(u32*)&data[0x24] = r3
    reloc16(t.here(), R_HA, kModuleId, 3, 0x24); t.lis(4);
    reloc16(t.here(), R_LO, kModuleId, 3, 0x24); t.stw(3, 4);
    t.blr();

    fn("hi_ori");             // r3 = &data[0x30] with lis (HI) + ori (LO)
    reloc16(t.here(), R_HI, kModuleId, 3, 0x30); t.lis(3);
    reloc16(t.here(), R_LO, kModuleId, 3, 0x30); t.ori(3, 3);
    t.blr();

    fn("plain16");            // r3 = sign-extended low half of &data[0xF0] (ADDR16)
    reloc16(t.here(), R_ADDR16, kModuleId, 3, 0xF0); t.li(3, 0);
    t.blr();

    fn("load_float");         // f1 = *(float*)&data[0x30 + 4]
    reloc16(t.here(), R_HA, kModuleId, 3, 0x34); t.lis(4);
    reloc16(t.here(), R_LO, kModuleId, 3, 0x34); t.lfs(1, 4);
    t.blr();

    fn("import_pair");        // r3 = base of section 2 of the other module + 0x10
    reloc16(t.here(), R_HA, kOtherModuleId, 2, 0x10); t.lis(3);
    reloc16(t.here(), R_LO, kOtherModuleId, 2, 0x10); t.addi(3, 3);
    t.blr();

    fn("call_cross");         // r3 = leaf(r3) ... then text2_const() + 1 = 42: bl in the section, bl to another section
    t.prologue();
    t.bl_to(m.off["leaf"]);
    m.relocs.push_back(Reloc{1, t.here(), R_REL24, kModuleId, 2, 0});   // target offset patched below
    const uint32_t cross_reloc_index = (uint32_t)m.relocs.size() - 1;
    t.bl_reloc();
    t.addi(3, 3, 1);
    t.epilogue();

    fn("call_dol");           // r3 = kDolDouble(7) = 19, and the return address the DOL function sees
    t.prologue();
    t.li(3, 7);
    m.relocs.push_back(Reloc{1, t.here(), R_REL24, 0, 1, kDolDouble});
    t.bl_reloc();
    m.call_dol_return = t.here();
    t.epilogue();

    fn("call_abs24");         // r3 = kDolAdd1000(3) = 1003 through an absolute branch relocation
    t.prologue();
    t.li(3, 3);
    m.relocs.push_back(Reloc{1, t.here(), R_ADDR24, 0, 1, kDolAdd1000});
    t.bla_reloc();
    t.epilogue();

    fn("cond_dol");           // r3 = 1 -> kDolCondA, 2 -> kDolCondB, 3 -> kDolCondC (conditional branches out of the module), else 0
    const uint32_t cond_types[3] = {R_ADDR14, R_ADDR14_BRTAKEN, R_ADDR14_BRNTAKEN};
    const uint32_t cond_targets[3] = {kDolCondA, kDolCondB, kDolCondC};
    for (int i = 0; i < 3; i++) {
        t.cmpwi(3, i + 1);
        m.relocs.push_back(Reloc{1, t.here(), (uint8_t)cond_types[i], 0, 1, cond_targets[i]});
        t.beq_reloc();
    }
    t.li(3, 0);
    t.blr();

    fn("switch");             // r3 = 0,1,2 -> 10,20,30 through a jump table in .data, else -1
    t.cmplwi(3, 2);
    const uint32_t bgt_at = t.here();
    t.bgt_placeholder();
    reloc16(t.here(), R_HA, kModuleId, 3, 0x80); t.lis(4);
    reloc16(t.here(), R_LO, kModuleId, 3, 0x80); t.addi(4, 4);
    t.rlwinm_r0_r3_2();
    t.lwzx_r0_r4_r0();
    t.mtctr_r0();
    t.bctr();
    uint32_t cases[3];
    for (int i = 0; i < 3; i++) {
        cases[i] = t.here();
        t.li(3, 10 * (i + 1));
        t.blr();
    }
    const uint32_t default_at = t.here();
    t.li(3, -1);
    t.blr();
    t.patch_bc(bgt_at, default_at);

    fn("vslot0");             // reached only through the vtable in .data
    t.li(3, 100);
    t.blr();

    fn("cond_x");             // r3 == 5 -> the code of .text2 (a REL14 into another section), else 0
    t.cmpwi(3, 5);
    m.relocs.push_back(Reloc{1, t.here(), R_REL14, kModuleId, 2, 0});   // target offset patched below
    const uint32_t rel14_reloc_index = (uint32_t)m.relocs.size() - 1;
    t.beq_reloc();
    t.li(3, 0);
    t.blr();

    // ---- .text2 (2) ----
    Asm t2;
    m.off["text2_const"] = t2.here(); t2.li(3, 41); t2.blr();
    m.off["vslot1"] = t2.here();      t2.li(3, 200); t2.blr();
    m.off["alt55"] = t2.here();       t2.li(3, 55); t2.blr();
    m.relocs[cross_reloc_index].addend = m.off["text2_const"];
    m.relocs[rel14_reloc_index].addend = m.off["alt55"];

    // ---- .data (3) ----
    std::vector<uint8_t> data(kDataSize, 0);
    put_be32(data, 0x20, 0x12345678);
    put_be32(data, 0x34, 0x3FC00000);   // 1.5f
    m.off["d_addr"] = 0x8010;
    m.off["d_load"] = 0x20;
    m.off["d_store"] = 0x24;
    m.off["d_hi"] = 0x30;
    m.off["d_float"] = 0x34;
    m.off["d_vtable"] = 0x40;
    m.off["d_jump"] = 0x80;
    m.off["d_dol_word"] = 0x90;
    m.off["d_bss_ptr"] = 0xA0;
    m.relocs.push_back(Reloc{3, 0x40, R_ADDR32, kModuleId, 1, m.off["vslot0"]});
    m.relocs.push_back(Reloc{3, 0x44, R_ADDR32, kModuleId, 2, m.off["vslot1"]});
    for (int i = 0; i < 3; i++) m.relocs.push_back(Reloc{3, 0x80u + 4 * i, R_ADDR32, kModuleId, 1, cases[i]});
    m.relocs.push_back(Reloc{3, 0x90, R_ADDR32, 0, 0, kDolDataWord});
    m.relocs.push_back(Reloc{3, 0xA0, R_ADDR32, kModuleId, 4, 0x10});

    auto bytes_of = [](const Asm& a) {
        std::vector<uint8_t> v;
        for (uint32_t x : a.w) be32(v, x);
        return v;
    };
    m.section_data[1] = bytes_of(t);
    m.section_size[1] = (uint32_t)m.section_data[1].size();
    m.section_exec[1] = true;
    m.section_data[2] = bytes_of(t2);
    m.section_size[2] = (uint32_t)m.section_data[2].size();
    m.section_exec[2] = true;
    m.section_data[3] = data;
    m.section_size[3] = kDataSize;
    m.section_size[4] = kBssSize;   // BSS: a length and no data

    write_file(m, m.off["addr_pair"], m.off["leaf"], m.off["vslot0"]);
    return m;
}

}  // namespace relsynth
