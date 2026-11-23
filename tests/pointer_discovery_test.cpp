// SPDX-License-Identifier: MIT
//
// Pointer discovery on a synthetic DOL: one text section of hand-assembled code and one data
// section holding a callback table. Checks which targets are promoted under each policy and why
// the others are rejected (mid-function label, unaligned, undecodable code, fall-through from code
// above) or only kept by aggressive (frame teardown, fall-through into another function).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gcrecomp/cfg.h"
#include "gcrecomp/dol.h"
#include "gcrecomp/pointer_discovery.h"
#include "gcrecomp/ppc.h"

using namespace gcrecomp;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// ---- a tiny assembler ----
static uint32_t lis(int rd, uint32_t imm) { return 0x3C000000u | (rd << 21) | (imm & 0xFFFF); }
static uint32_t addi(int rd, int ra, uint32_t imm) { return 0x38000000u | (rd << 21) | (ra << 16) | (imm & 0xFFFF); }
static uint32_t ori(int ra, int rs, uint32_t imm) { return 0x60000000u | (rs << 21) | (ra << 16) | (imm & 0xFFFF); }
static uint32_t stw(int rs, int ra, int d) { return 0x90000000u | (rs << 21) | (ra << 16) | ((uint32_t)d & 0xFFFF); }
static uint32_t lwz(int rd, int ra, int d) { return 0x80000000u | (rd << 21) | (ra << 16) | ((uint32_t)d & 0xFFFF); }
static uint32_t b(int32_t off) { return 0x48000000u | ((uint32_t)off & 0x03FFFFFC); }
static uint32_t bl(int32_t off) { return b(off) | 1; }
static const uint32_t BLR = 0x4E800020, MFLR_R0 = 0x7C0802A6, MTLR_R0 = 0x7C0803A6, NOP = 0x60000000;
static const uint32_t ADDI_R1_16 = 0x38210010;
static const uint32_t STWU_R1_M16 = 0x9421FFF0, CMPWI_R3_0 = 0x2C030000, BEQ_8 = 0x41820008;
static const uint32_t GARBAGE = 0x04000000;  // primary opcode 1: not a Gekko instruction

constexpr uint32_t kText = 0x80003100, kData = 0x80010000;

// Word indexes in the text section.
enum : uint32_t {
    F0 = 0, MID = 15, CB_A = 19, CB_B = 22, CB_C = 23, BAD = 25, H = 27, K = 28, G = 29, STUB = 32, J = 33,
    PAD = 35, CB_D = 37, NWORDS = 39,
};
static uint32_t at(uint32_t w) { return kText + w * 4; }

static void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

// f0 (entry), then a series of small functions reached only through pointers.
static bool build_dol(DOLFile& dol) {
    std::vector<uint32_t> t(NWORDS, NOP);
    t[0] = STWU_R1_M16; t[1] = MFLR_R0; t[2] = stw(0, 1, 20);
    t[3] = lis(3, at(CB_A) >> 16);              // lis  r3,hi(cbA)
    t[4] = addi(5, 0, 0);                       // li   r5,0      (between the halves)
    t[5] = addi(4, 3, at(CB_A) & 0xFFFF);       // addi r4,r3,lo  (different destination register)
    t[6] = lis(6, at(MID) >> 16);               // pair onto a label inside f0
    t[7] = addi(6, 6, at(MID) & 0xFFFF);
    t[8] = lis(7, (at(CB_A) + 1) >> 16);        // pair onto an unaligned address
    t[9] = ori(7, 7, (at(CB_A) + 1) & 0xFFFF);
    t[10] = lis(8, at(CB_D) >> 16);             // lis/ori pair onto a callback behind padding
    t[11] = ori(8, 8, at(CB_D) & 0xFFFF);
    t[12] = CMPWI_R3_0;
    t[13] = BEQ_8;                              // beq mid
    t[14] = bl((int32_t)((K - 14) * 4));        // bl k
    t[MID] = lwz(0, 1, 20);
    t[16] = MTLR_R0;
    t[17] = ADDI_R1_16;
    t[18] = BLR;
    t[CB_A] = addi(0, 0, 1); t[CB_A + 1] = stw(0, 13, -0x7bc0); t[CB_A + 2] = BLR;  // li r0,1; stw; blr
    t[CB_B] = BLR;
    t[CB_C] = ADDI_R1_16; t[CB_C + 1] = BLR;    // an epilogue, not an entry
    t[BAD] = GARBAGE; t[BAD + 1] = BLR;
    t[H] = addi(3, 0, 0);                       // no terminator: runs straight into k
    t[K] = BLR;
    t[G] = addi(3, 0, 1); t[G + 1] = addi(4, 0, 2); t[G + 2] = BLR;
    t[STUB] = b((int32_t)((K - STUB) * 4));     // b k
    t[J] = addi(3, 0, 5); t[J + 1] = BLR;
    t[PAD] = NOP; t[PAD + 1] = NOP;
    t[CB_D] = addi(0, 0, 2); t[CB_D + 1] = BLR;

    std::vector<uint8_t> text, data;
    for (uint32_t w : t) be32(text, w);
    for (uint32_t w : {at(CB_B), at(CB_C), at(BAD), at(H), at(G + 1), at(J), at(K)}) be32(data, w);
    be32(data, 0x80003101);  // a lone unaligned "pointer"

    std::vector<uint8_t> file(0x100, 0);
    auto put = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; i++) file[off + i] = (uint8_t)(v >> (24 - 8 * i));
    };
    put(0x00, 0x100);                           put(0x48, kText);  put(0x90, (uint32_t)text.size());   // text0
    put(0x1C, 0x100 + (uint32_t)text.size());   put(0x64, kData);  put(0xAC, (uint32_t)data.size());   // data0
    put(0xE0, kText);                                                                                   // entry
    file.insert(file.end(), text.begin(), text.end());
    file.insert(file.end(), data.begin(), data.end());

    FILE* f = std::tmpfile();
    std::fwrite(file.data(), 1, file.size(), f);
    std::rewind(f);
    const bool ok = dol.load(f);
    std::fclose(f);
    return ok;
}

static const PointerCandidate* find(const PointerDiscoveryResult& r, uint32_t target) {
    for (const auto& c : r.candidates)
        if (c.target == target) return &c;
    return nullptr;
}

static bool reason_has(const PointerCandidate* c, const char* needle) {
    return c && c->reason.find(needle) != std::string::npos;
}

static void check_tiers(const PointerDiscoveryResult& r) {
    const PointerCandidate* c;
    c = find(r, at(CB_A));  CHECK(c && c->tier == PointerTier::Safe);   // lis + addi with another register and a gap
    c = find(r, at(CB_B));  CHECK(c && c->tier == PointerTier::Safe);   // data table entry
    c = find(r, at(CB_D));  CHECK(c && c->tier == PointerTier::Safe);   // lis + ori, behind padding
    c = find(r, at(J));     CHECK(c && c->tier == PointerTier::Aggressive);  // follows a `b`, not a `blr`
    c = find(r, at(MID));   CHECK(c && c->tier == PointerTier::Rejected && reason_has(c, "mid-function"));
    c = find(r, at(CB_A) + 1); CHECK(c && c->tier == PointerTier::Rejected && reason_has(c, "unaligned"));
    // Doubtful shapes: not safe, kept by aggressive at a penalty.
    c = find(r, at(CB_C));  CHECK(c && c->tier == PointerTier::Aggressive && reason_has(c, "tears down"));
    c = find(r, at(BAD));   CHECK(c && c->tier == PointerTier::Rejected && reason_has(c, "undecodable"));
    c = find(r, at(H));     CHECK(c && c->tier == PointerTier::Aggressive && reason_has(c, "falls through into a known function"));
    c = find(r, at(G + 1)); CHECK(c && c->tier == PointerTier::Rejected && reason_has(c, "not preceded"));
    // The known entries are reported as such and never promoted.
    c = find(r, at(K));     CHECK(c && c->tier == PointerTier::Known && !c->promoted);
}

static size_t promoted(const PointerDiscoveryResult& r, std::initializer_list<uint32_t> expect) {
    size_t hit = 0;
    for (uint32_t a : expect) {
        const PointerCandidate* c = find(r, a);
        if (c && c->promoted) hit++;
    }
    return hit;
}

int main() {
    CHECK(ppc_disasm(GARBAGE, 0).type == PPCInsnType::UNKNOWN);

    DOLFile dol;
    if (!build_dol(dol)) { std::printf("synthetic DOL failed to load\n"); return 1; }

    CFG cfg;
    cfg.call_targets.insert(dol.entry_point);
    cfg.add_extra_entries({at(F0), at(K)});
    cfg.build_functions(dol);
    CHECK(cfg.functions.size() == 2);

    // Off: everything is classified, nothing promoted.
    PointerDiscoveryResult off = discover_pointer_entries(cfg, dol, PointerPolicy::Off);
    check_tiers(off);
    CHECK(off.promoted == 0);

    PointerDiscoveryResult safe = discover_pointer_entries(cfg, dol, PointerPolicy::Safe);
    check_tiers(safe);
    CHECK(safe.promoted == 3);
    CHECK(promoted(safe, {at(CB_A), at(CB_B), at(CB_D)}) == 3);
    CHECK(!find(safe, at(J))->promoted);

    PointerDiscoveryResult aggr = discover_pointer_entries(cfg, dol, PointerPolicy::Aggressive);
    check_tiers(aggr);
    CHECK(aggr.promoted == 6);
    CHECK(promoted(aggr, {at(CB_A), at(CB_B), at(CB_D), at(J), at(CB_C), at(H)}) == 6);

    // No promoted entry may sit inside the body of a function that was already known.
    for (const auto& c : aggr.candidates) {
        if (!c.promoted) continue;
        for (const auto& [e, f] : cfg.functions)
            for (const auto& [s, blk] : f.blocks) CHECK(!(c.target > blk.start && c.target < blk.end));
    }

    // Promote and run again: the promoted entries are now known, nothing new appears.
    for (const auto& c : safe.candidates)
        if (c.promoted) cfg.add_function(c.target, dol);
    CHECK(cfg.functions.count(at(CB_A)) == 1);
    PointerDiscoveryResult again = discover_pointer_entries(cfg, dol, PointerPolicy::Safe);
    CHECK(find(again, at(CB_A)) && find(again, at(CB_A))->tier == PointerTier::Known);
    CHECK(again.promoted == 0);

    // The report names the origins and the rejection reasons.
    FILE* rf = std::tmpfile();
    write_pointer_report(rf, aggr);
    std::fflush(rf);
    std::rewind(rf);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, rf)) text += buf;
    std::fclose(rf);
    char want[96];
    std::snprintf(want, sizeof want, "lis+addi@0x%08X/0x%08X", at(3), at(5));
    CHECK(text.find(want) != std::string::npos);
    std::snprintf(want, sizeof want, "data@0x%08X", kData);
    CHECK(text.find(want) != std::string::npos);
    CHECK(text.find("mid-function") != std::string::npos);

    PointerPolicy p;
    CHECK(parse_pointer_policy("safe", p) && p == PointerPolicy::Safe);
    CHECK(parse_pointer_policy("aggressive", p) && p == PointerPolicy::Aggressive);
    CHECK(parse_pointer_policy("off", p) && p == PointerPolicy::Off);
    CHECK(!parse_pointer_policy("yes", p));

    if (g_failures) std::printf("%d failure(s)\n", g_failures);
    else std::printf("pointer_discovery: all checks passed\n");
    return g_failures ? 1 : 0;
}
