// SPDX-License-Identifier: MIT
//
// REL module analysis (tools/recompiler/rel_recomp.cpp) on the synthetic module of rel_synth.h: what the recompiler
// finds (functions and where they come from, relocation counts, branches that leave the module) and what it refuses.
#include <cstdio>
#include <string>

#include "gcrecomp/rel.h"
#include "gcrecomp/rel_recomp.h"
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
#define CHECK_EQ(a, b)                                                                                           \
    do {                                                                                                         \
        if ((long long)(a) != (long long)(b)) {                                                                  \
            std::printf("%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, (long long)(a), (long long)(b)); \
            ++g_failures;                                                                                        \
        }                                                                                                        \
    } while (0)

static bool analyse(const std::vector<uint8_t>& file, RelReport& report, std::string& error) {
    RELFile rel;
    if (!rel.load_from_buffer(file.data(), file.size(), "synthetic")) {
        error = "parse failed";
        return false;
    }
    RelRecompileOptions options;
    options.name = "synth";
    return recompile_rel_module(rel, options, report, error);
}

int main() {
    using namespace relsynth;
    Module m = build();

    // ---- the parser reads what the writer wrote ----
    {
        RELFile rel;
        CHECK(rel.load_from_buffer(m.file.data(), m.file.size(), "synthetic"));
        CHECK_EQ(rel.header.module_id, kModuleId);
        CHECK_EQ(rel.header.version, 3);
        CHECK_EQ(rel.header.num_sections, kNumSections);
        CHECK_EQ(rel.header.prolog_offset, m.off["addr_pair"]);
        CHECK(rel.sections[1].executable && rel.sections[2].executable && !rel.sections[3].executable);
        CHECK_EQ(rel.sections[4].size, kBssSize);
        CHECK_EQ(rel.imports.size(), 3);
        std::vector<RELPlace> places;
        std::string error;
        CHECK(rel.places(places, &error));
        CHECK_EQ(places.size(), m.relocs.size());   // NOP / SECTION / END entries are consumed, not reported
        for (const Reloc& r : m.relocs) {
            bool found = false;
            for (const RELPlace& p : places) {
                found = found || (p.section == r.section && p.offset == r.offset && (uint8_t)p.type == r.type &&
                                  p.module_id == r.module && p.target_section == r.target_section && p.addend == r.addend);
            }
            CHECK(found);
        }
    }

    // ---- analysis ----
    RelReport rep;
    std::string error;
    CHECK(analyse(m.file, rep, error));
    CHECK(error.empty());
    CHECK_EQ(rep.by_type[1], 7);    // vtable x2, jump table x3, DOL word, BSS pointer
    CHECK_EQ(rep.by_type[2], 1);
    CHECK_EQ(rep.by_type[3], 1);
    CHECK_EQ(rep.by_type[7], 1);
    CHECK_EQ(rep.by_type[8], 1);
    CHECK_EQ(rep.by_type[9], 1);
    CHECK_EQ(rep.by_type[10], 2);
    CHECK_EQ(rep.by_type[11], 1);
    CHECK(rep.by_type[4] > 0 && rep.by_type[5] == 1 && rep.by_type[6] > 0);
    CHECK_EQ(rep.relocs_other_module, 2);   // the pair against the other module (HA + LO)
    CHECK_EQ(rep.branches_to_dol, 5);       // REL24, ADDR24 and the three conditional ones
    CHECK_EQ(rep.branches_internal, 2);     // REL24 and REL14 between sections
    CHECK_EQ(rep.ha_without_lo, 0);
    CHECK_EQ(rep.lo_without_ha, 0);
    CHECK_EQ(rep.jump_table_labels, 3);     // the table's cases are labels, not functions
    CHECK(rep.only_data_reloc >= 1);        // vslot1 is named by nothing but the vtable
    CHECK_EQ(rep.unknown_instructions, 0);
    CHECK_EQ(rep.unresolved_bctr, 0);
    CHECK_EQ(rep.uncovered.size(), 0);
    CHECK(rep.functions >= 17);

    // ---- the DOL code the module names must be a function entry of the DOL ----
    {
        RELFile rel;
        CHECK(rel.load_from_buffer(m.file.data(), m.file.size(), "synthetic"));
        DOLFile dol;
        DOLSection text;
        text.address = 0x80004000;
        text.size = 0x1000;
        text.is_text = true;
        dol.sections.push_back(text);
        const std::vector<uint32_t> entries = rel_dol_entries(rel, dol);
        const std::vector<uint32_t> expected = {kDolDouble, kDolAdd1000, kDolCondA, kDolCondB, kDolCondC, kDolDataWord};
        CHECK(entries == expected);
        dol.sections[0].size = 0x300;   // a DOL that does not hold the later addresses
        CHECK_EQ(rel_dol_entries(rel, dol).size(), 3);
    }

    // ---- what it refuses ----
    {
        // an absolute branch into the module itself cannot be made position independent
        Module bad = build();
        bad.relocs.push_back(Reloc{1, bad.off["leaf"], R_ADDR24, kModuleId, 1, bad.off["addr_pair"]});
        write_file(bad, bad.off["addr_pair"], bad.off["leaf"], bad.off["vslot0"]);
        RelReport r;
        std::string e;
        CHECK(!analyse(bad.file, r, e));
        CHECK(e.find("absolute branch") != std::string::npos);
    }
    {
        // a relocation whose place is outside its section
        Module bad = build();
        bad.relocs.push_back(Reloc{3, kDataSize, R_ADDR32, kModuleId, 1, 0});
        write_file(bad, bad.off["addr_pair"], bad.off["leaf"], bad.off["vslot0"]);
        RelReport r;
        std::string e;
        CHECK(!analyse(bad.file, r, e));
        CHECK(e.find("outside the section") != std::string::npos);
    }
    {
        // a 16-bit relocation that is not on an instruction immediate
        Module bad = build();
        bad.relocs.push_back(Reloc{1, bad.off["leaf"], R_LO, kModuleId, 3, 0});
        write_file(bad, bad.off["addr_pair"], bad.off["leaf"], bad.off["vslot0"]);
        RelReport r;
        std::string e;
        CHECK(!analyse(bad.file, r, e));
    }

    if (g_failures == 0) std::printf("rel_recomp_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
