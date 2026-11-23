// =============================================================================
// REL module recompiler - see docs/REL_MODULES.md
// =============================================================================

#include "gcrecomp/rel_recomp.h"

#include "gcrecomp/cfg.h"
#include "gcrecomp/ppc.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace gcrecomp {

namespace {

struct Image {
    DOLFile                image;
    RelEmitInfo            info;
    std::vector<RELPlace>  places;
    std::set<uint32_t>     header_entries;       // prolog / epilog / unresolved (virtual addresses)
    std::set<uint32_t>     data_targets;         // code addresses held by ADDR32 relocations
    std::set<uint32_t>     addr16_targets;       // code addresses named by ADDR16* relocations
};

uint32_t align_up(uint32_t v, uint32_t a) { return a < 2 ? v : (v + a - 1) & ~(a - 1); }

std::string hex32(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08Xu", v);
    return buf;
}

struct SectionBytes {
    std::vector<uint8_t>& data;
    uint16_t get16(uint32_t off) const { return (uint16_t)((data[off] << 8) | data[off + 1]); }
    uint32_t get32(uint32_t off) const {
        return ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) | ((uint32_t)data[off + 2] << 8) | data[off + 3];
    }
    void put16(uint32_t off, uint16_t v) { data[off] = (uint8_t)(v >> 8); data[off + 1] = (uint8_t)v; }
    void put32(uint32_t off, uint32_t v) {
        data[off] = (uint8_t)(v >> 24); data[off + 1] = (uint8_t)(v >> 16); data[off + 2] = (uint8_t)(v >> 8);
        data[off + 3] = (uint8_t)v;
    }
};

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
bool fail(std::string& error, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    error = buf;
    return false;
}

// Lay the sections out at kRelVirtualBase, apply every relocation to the image against that layout and remember the
// relocated instructions. Branches that leave the module are parked at a distance from where they stood, outside
// every code range, and recorded in `info.external_branch`.
bool build_image(RELFile& rel, const std::string& name, Image& img, RelReport& rep, std::string& error) {
    const size_t n = rel.sections.size();
    if (n == 0) return fail(error, "module has no sections");

    RelEmitInfo& info = img.info;
    info.base_array = "rel_" + name + "::base";
    info.section_vbase.assign(n, 0);
    info.section_size.assign(n, 0);
    uint32_t cursor = kRelVirtualBase;
    const uint32_t alignment = std::max<uint32_t>(32, std::max(rel.header.align, rel.header.bss_align));
    for (size_t i = 0; i < n; i++) {
        cursor = align_up(cursor, alignment);
        info.section_vbase[i] = cursor;
        info.section_size[i] = rel.sections[i].size;
        cursor += rel.sections[i].size;
    }
    if (cursor - kRelVirtualBase > kRelMaxLayout) return fail(error, "module is larger than %u bytes", kRelMaxLayout);

    if (!rel.places(img.places, &error)) return false;

    auto expr_of = [&](const RELPlace& p, uint32_t& value_in_image) -> std::string {
        char buf[160];
        if (p.module_id == 0) {
            value_in_image = p.addend;
            return hex32(p.addend);
        }
        if (p.module_id == rel.header.module_id) {
            value_in_image = info.section_vbase[p.target_section] + p.addend;
            snprintf(buf, sizeof buf, "%s[%u] + 0x%Xu", info.base_array.c_str(), p.target_section, p.addend);
            return buf;
        }
        value_in_image = 0;
        snprintf(buf, sizeof buf, "gcrecomp::rel_import_base(%u, %u) + 0x%Xu", p.module_id, p.target_section, p.addend);
        return buf;
    };

    for (const RELPlace& p : img.places) {
        rep.by_type[(unsigned)p.type]++;
        const bool self = p.module_id == rel.header.module_id;
        if (p.module_id == 0) rep.relocs_dol++;
        else if (self) rep.relocs_self++;
        else rep.relocs_other_module++;
        if (self && p.target_section >= n)
            return fail(error, "relocation at section %u offset 0x%X targets section %u", p.section, p.offset, p.target_section);

        SectionBytes bytes{rel.sections[p.section].data};
        const bool in_code = rel.sections[p.section].executable;
        if (in_code) rep.relocs_in_code++;
        uint32_t value = 0;
        const std::string expr = expr_of(p, value);
        const uint32_t place_va = info.section_vbase[p.section] + p.offset;
        const bool target_is_code = self && rel.sections[p.target_section].executable;

        switch (p.type) {
        case RELRelocType::R_PPC_NONE:
            break;

        case RELRelocType::R_PPC_ADDR32:
            bytes.put32(p.offset, value);
            if (self && !in_code && target_is_code && (value & 3) == 0) img.data_targets.insert(value);
            break;

        case RELRelocType::R_PPC_ADDR16:
        case RELRelocType::R_PPC_ADDR16_LO:
        case RELRelocType::R_PPC_ADDR16_HI:
        case RELRelocType::R_PPC_ADDR16_HA: {
            RelImmKind kind = RelImmKind::Plain;
            uint16_t field = (uint16_t)value;
            if (p.type == RELRelocType::R_PPC_ADDR16_LO) { kind = RelImmKind::Lo; }
            if (p.type == RELRelocType::R_PPC_ADDR16_HI) { kind = RelImmKind::Hi; field = (uint16_t)(value >> 16); }
            if (p.type == RELRelocType::R_PPC_ADDR16_HA) { kind = RelImmKind::Ha; field = (uint16_t)((value + 0x8000u) >> 16); }
            bytes.put16(p.offset, field);
            if (self && target_is_code && (value & 3) == 0) img.addr16_targets.insert(value);
            if (in_code) {
                if ((p.offset & 3) != 2)
                    return fail(error, "ADDR16 relocation at section %u offset 0x%X is not on an instruction immediate",
                                p.section, p.offset);
                img.info.imm[place_va - 2] = RelImm{kind, expr};
                switch (kind) {
                case RelImmKind::Ha: rep.imm_ha++; break;
                case RelImmKind::Lo: rep.imm_lo++; break;
                case RelImmKind::Hi: rep.imm_hi++; break;
                case RelImmKind::Plain: rep.imm_plain++; break;
                }
            }
            break;
        }

        case RELRelocType::R_PPC_ADDR24:
        case RELRelocType::R_PPC_REL24:
        case RELRelocType::R_PPC_ADDR14:
        case RELRelocType::R_PPC_ADDR14_BRTAKEN:
        case RELRelocType::R_PPC_ADDR14_BRNTAKEN:
        case RELRelocType::R_PPC_REL14: {
            const bool wide = p.type == RELRelocType::R_PPC_ADDR24 || p.type == RELRelocType::R_PPC_REL24;
            const bool relative = p.type == RELRelocType::R_PPC_REL24 || p.type == RELRelocType::R_PPC_REL14;
            if (!in_code || (p.offset & 3) != 0)
                return fail(error, "branch relocation at section %u offset 0x%X is not on an instruction", p.section, p.offset);
            const uint32_t mask = wide ? 0x03FFFFFCu : 0x0000FFFCu;
            const uint32_t word = bytes.get32(p.offset);
            if (self) {
                if (!relative)
                    return fail(error, "absolute branch (relocation type %u) into the module at section %u offset 0x%X is not supported",
                                (unsigned)p.type, p.section, p.offset);
                const int32_t disp = (int32_t)(value - place_va);
                const int32_t limit = wide ? 0x2000000 : 0x8000;
                if (disp < -limit || disp >= limit)
                    return fail(error, "branch relocation at section %u offset 0x%X is out of range", p.section, p.offset);
                bytes.put32(p.offset, (word & ~mask) | ((uint32_t)disp & mask));
                rep.branches_internal++;
            } else {
                // Leaves the module. The image branch becomes a relative branch to a place no code range holds
                // (a 24-bit branch) or to the next instruction (a 14-bit one: it cannot reach that far).
                const uint32_t disp = wide ? 0x01000000u : 4u;
                bytes.put32(p.offset, (word & ~mask & ~2u) | (disp & mask));
                img.info.external_branch[place_va] = expr;
                rep.branches_to_dol++;
            }
            break;
        }

        case RELRelocType::R_DOLPHIN_NOP:
        case RELRelocType::R_DOLPHIN_SECTION:
        case RELRelocType::R_DOLPHIN_END:
            break;
        }
        if ((unsigned)p.type > 11 && (unsigned)p.type < 201)
            return fail(error, "unknown relocation type %u at section %u offset 0x%X", (unsigned)p.type, p.section, p.offset);
    }

    // DOLFile view for the analysis passes.
    DOLFile& dol = img.image;
    dol = DOLFile{};
    dol.memory_base = kRelVirtualBase;
    dol.memory_end = cursor;
    dol.memory.assign(cursor - kRelVirtualBase, 0);
    for (size_t i = 0; i < n; i++) {
        const RELSection& s = rel.sections[i];
        if (s.size == 0) continue;
        DOLSection ds;
        ds.file_offset = s.offset;
        ds.address = info.section_vbase[i];
        ds.size = s.size;
        ds.is_text = s.executable;
        ds.index = (int)i;
        ds.data = s.data;
        if (ds.data.size() < ds.size) ds.data.resize(ds.size, 0);   // BSS
        std::memcpy(dol.memory.data() + (ds.address - kRelVirtualBase), ds.data.data(), ds.size);
        dol.sections.push_back(std::move(ds));
    }
    for (const RELSection& s : rel.sections) {
        if (s.executable) rep.code_bytes += s.size;
    }

    auto header_entry = [&](uint32_t section, uint32_t offset) {
        if (section == 0 || section >= n || !rel.sections[section].executable || offset >= rel.sections[section].size) return 0u;
        return info.section_vbase[section] + offset;
    };
    const uint32_t prolog = header_entry(rel.header.prolog_section, rel.header.prolog_offset);
    const uint32_t epilog = header_entry(rel.header.epilog_section, rel.header.epilog_offset);
    const uint32_t unres = header_entry(rel.header.unresolved_section, rel.header.unresolved_offset);
    for (uint32_t a : {prolog, epilog, unres}) {
        if (a) img.header_entries.insert(a);
    }
    dol.entry_point = prolog;
    return true;
}

struct Span { uint32_t start, end, owner; };

}  // namespace

void RelReport::print(FILE* out, bool verbose) const {
    uint32_t uncovered_bytes = 0;
    for (const Range& r : uncovered) uncovered_bytes += r.length;
    fprintf(out, "[REL] module '%s' id=%u version=%u sections=%u code=%u bytes: %u in functions, %u padding between them, "
                 "%u in %zu runs no function contains\n",
            name.c_str(), id, version, num_sections, code_bytes, covered_code_bytes, uncovered_padding_bytes, uncovered_bytes,
            uncovered.size());
    if (verbose) {
        for (const Range& r : uncovered) fprintf(out, "[REL]   uncovered: section %u +0x%X, 0x%X bytes\n", r.section, r.offset, r.length);
    }
    fprintf(out, "[REL]   relocations by type:");
    for (const auto& [type, count] : by_type) fprintf(out, " %u:%u", type, count);
    fprintf(out, "\n[REL]   relocations against: itself %u, the DOL %u, another module %u; %u places in code\n", relocs_self,
            relocs_dol, relocs_other_module, relocs_in_code);
    fprintf(out, "[REL]   relocated immediates in code: HA %u, LO %u, HI %u, plain %u (HA without LO %u, LO without HA %u)\n",
            imm_ha, imm_lo, imm_hi, imm_plain, ha_without_lo, lo_without_ha);
    fprintf(out, "[REL]   relocated branches: %u inside the module, %u leaving it\n", branches_internal, branches_to_dol);
    fprintf(out, "[REL]   functions: %u (bl %u, header %u, data relocation %u, address relocation %u)\n", functions, from_bl,
            from_header, from_data_reloc, from_addr16_reloc);
    fprintf(out, "[REL]     only a data relocation: %u, only an address relocation: %u, none of those (scans, tail calls): %u, "
                 "swept up from unreached code: %u\n",
            only_data_reloc, only_addr16_reloc, other_origin, swept_entries);
    fprintf(out, "[REL]     pointer discovery promoted %u; relocation targets promoted %u (%u inside another function's body); "
                 "%u jump-table labels\n",
            pointer_promoted, reloc_targets_promoted, reloc_targets_promoted_mid, jump_table_labels);
    fprintf(out, "[REL]   instructions: %u; rejected by the disassembler %u; decoded but not translated %u; "
                 "relocated but not position independent %u; bctr without a resolved table %u\n",
            instructions, unknown_instructions, unimplemented, unhandled_reloc, unresolved_bctr);
    fprintf(out, "[REL]   relocated instructions in no function: %u; analysed-layout addresses baked into the output: %u; "
                 "output files: %u\n",
            imm_not_emitted, baked_addresses, files);
}

bool recompile_rel_module(RELFile& rel, const RelRecompileOptions& opt, RelReport& rep, std::string& error) {
    rep = RelReport{};
    rep.name = opt.name;
    rep.id = rel.header.module_id;
    rep.version = rel.header.version;
    rep.num_sections = (uint32_t)rel.sections.size();

    Image img;
    if (!build_image(rel, opt.name, img, rep, error)) return false;
    const DOLFile& dol = img.image;
    const RelEmitInfo& info = img.info;

    // ---- Functions: the DOL pipeline, unchanged, on the relocated image ----
    CFG cfg;
    cfg.scan_targets(dol);
    cfg.add_extra_entries(std::vector<uint32_t>(img.header_entries.begin(), img.header_entries.end()));
    cfg.build_functions(dol);
    std::set<uint32_t> bl_targets;
    for (const auto& sec : dol.sections) {
        if (!sec.is_text) continue;
        for (const PPCInsn& insn : ppc_disasm_range(sec.data.data(), sec.address, sec.size)) {
            if (insn.type == PPCInsnType::B && insn.link && dol.is_code(insn.branch_target) &&
                img.info.external_branch.find(insn.address) == img.info.external_branch.end())
                bl_targets.insert(insn.branch_target);
        }
    }

    if (opt.pointer_policy != PointerPolicy::Off || !opt.pointer_report.empty()) {
        PointerDiscoveryResult found = discover_pointer_entries(cfg, dol, opt.pointer_policy);
        printf("[CFG] Pointer discovery (%s): %zu candidates, %zu promoted\n", pointer_policy_name(opt.pointer_policy),
               found.candidates.size(), found.promoted);
        for (const auto& c : found.candidates) {
            if (c.promoted) { cfg.add_function(c.target, dol); rep.pointer_promoted++; }
        }
        if (!opt.pointer_report.empty()) {
            FILE* rf = opt.pointer_report == "-" ? stdout : fopen(opt.pointer_report.c_str(), "w");
            if (rf) {
                write_pointer_report(rf, found);
                if (rf != stdout) fclose(rf);
            }
        }
    }

    // ---- Function bodies: which code each function holds, and which addresses are cases of a jump table ----
    std::vector<Span> spans;
    std::set<uint32_t> jump_labels;
    auto add_spans = [&](const Function& f) {
        for (const auto& [a, b] : f.blocks) {
            spans.push_back(Span{b.start, b.end, f.entry});
            for (uint32_t t : b.jump_table_targets) jump_labels.insert(t);
        }
    };
    for (const auto& [addr, f] : cfg.functions) add_spans(f);
    // ---- Code nothing reaches ----
    // Runs of code that no function contains (padding excepted) are functions nobody names through a bl or a
    // relocation: dead code, or reached through a pointer computed at run time. Each becomes an entry, so a call that
    // does arrive finds it. Repeats until no run is left: an entry covers only what it reaches.
    std::set<uint32_t> swept;
    auto scan_uncovered = [&](std::vector<RelReport::Range>& runs, uint32_t& padding_bytes, uint32_t& covered_bytes) {
        std::vector<bool> covered;
        for (const auto& sec : dol.sections) {
            if (!sec.is_text) continue;
            covered.assign(sec.size / 4, false);
            for (const Span& s : spans) {
                if (s.start < sec.address || s.start >= sec.address + sec.size) continue;
                for (uint32_t a = s.start; a < s.end && a < sec.address + sec.size; a += 4) covered[(a - sec.address) / 4] = true;
            }
            for (bool c : covered) covered_bytes += c ? 4u : 0u;
            // Padding (zero words, nops) is expected between functions.
            for (size_t i = 0; i < covered.size();) {
                if (covered[i]) { i++; continue; }
                size_t j = i;
                bool padding = true;
                while (j < covered.size() && !covered[j]) {
                    const uint32_t word = dol.read32(sec.address + (uint32_t)j * 4);
                    padding = padding && (word == 0 || word == 0x60000000u);
                    j++;
                }
                if (padding) padding_bytes += (uint32_t)(j - i) * 4;
                else runs.push_back(RelReport::Range{(uint32_t)sec.index, (uint32_t)i * 4, (uint32_t)(j - i) * 4});
                i = j;
            }
        }
    };
    for (;;) {
        std::vector<RelReport::Range> runs;
        uint32_t padding = 0, covered_bytes = 0;
        scan_uncovered(runs, padding, covered_bytes);
        if (runs.empty()) break;
        bool progress = false;
        for (const RelReport::Range& r : runs) {
            const uint32_t entry = info.section_vbase[r.section] + r.offset;
            if (cfg.functions.count(entry)) continue;
            cfg.add_function(entry, dol);
            auto it = cfg.functions.find(entry);
            if (it == cfg.functions.end()) continue;
            add_spans(it->second);
            swept.insert(entry);
            progress = true;
        }
        if (!progress) break;
    }
    rep.swept_entries = (unsigned)swept.size();
    auto inside_other = [&](uint32_t t) {
        for (const Span& s : spans) {
            if (t >= s.start && t < s.end && s.owner != t) return true;
        }
        return false;
    };
    std::set<uint32_t> reloc_targets;
    reloc_targets.insert(img.addr16_targets.begin(), img.addr16_targets.end());
    for (uint32_t t : img.data_targets) {
        if (jump_labels.count(t)) rep.jump_table_labels++;
        if (!img.addr16_targets.count(t) && jump_labels.count(t) && !cfg.functions.count(t)) continue;
        reloc_targets.insert(t);
    }
    for (uint32_t t : reloc_targets) {
        if (cfg.functions.count(t)) continue;
        const bool mid = inside_other(t);
        cfg.add_function(t, dol);
        auto it = cfg.functions.find(t);
        if (it == cfg.functions.end()) continue;
        add_spans(it->second);
        rep.reloc_targets_promoted++;
        if (mid) rep.reloc_targets_promoted_mid++;
    }


    // ---- Statistics on the functions ----
    rep.functions = (unsigned)cfg.functions.size();
    for (const auto& [addr, f] : cfg.functions) {
        const bool bl = bl_targets.count(addr) != 0, header = img.header_entries.count(addr) != 0;
        const bool data = img.data_targets.count(addr) != 0, a16 = img.addr16_targets.count(addr) != 0;
        rep.from_bl += bl;
        rep.from_header += header;
        rep.from_data_reloc += data;
        rep.from_addr16_reloc += a16;
        if (data && !bl && !header) rep.only_data_reloc++;
        if (a16 && !bl && !header && !data) rep.only_addr16_reloc++;
        if (!bl && !header && !data && !a16 && !swept.count(addr)) rep.other_origin++;
        for (const auto& [ba, b] : f.blocks) {
            rep.instructions += (unsigned)b.instructions.size();
            for (const PPCInsn& insn : b.instructions) {
                if (insn.type == PPCInsnType::UNKNOWN) rep.unknown_instructions++;
            }
            if (!b.instructions.empty() && b.instructions.back().type == PPCInsnType::BCCTR && !b.instructions.back().link &&
                b.jump_table_targets.empty())
                rep.unresolved_bctr++;
        }
    }
    {
        std::vector<RelReport::Range> runs;
        uint32_t padding = 0, covered_bytes = 0;
        scan_uncovered(runs, padding, covered_bytes);
        rep.uncovered = runs;
        rep.uncovered_padding_bytes = padding;
        rep.covered_code_bytes = covered_bytes;
    }
    // Every HA wants its LO (same target) a few instructions away, and the other way round.
    {
        std::map<uint32_t, const RelImm*> by_addr;
        for (const auto& [a, imm] : img.info.imm) by_addr[a] = &imm;
        for (const auto& [a, imm] : img.info.imm) {
            if (imm.kind == RelImmKind::Ha) {
                bool found = false;
                for (uint32_t k = 1; k <= 64 && !found; k++) {
                    auto it = by_addr.find(a + 4 * k);
                    found = it != by_addr.end() && it->second->kind != RelImmKind::Ha && it->second->target == imm.target;
                }
                if (!found) rep.ha_without_lo++;
            } else if (imm.kind == RelImmKind::Lo) {
                bool found = false;
                for (uint32_t k = 1; k <= 64 && !found; k++) {
                    if (a < 4 * k) break;
                    auto it = by_addr.find(a - 4 * k);
                    found = it != by_addr.end() && (it->second->kind == RelImmKind::Ha || it->second->kind == RelImmKind::Hi) &&
                            it->second->target == imm.target;
                }
                if (!found) rep.lo_without_ha++;
            }
        }
    }

    // ---- Emit ----
    if (opt.output_dir.empty()) {
        std::set<uint32_t> in_functions;
        for (const auto& [addr, f] : cfg.functions)
            for (const auto& [ba, b] : f.blocks)
                for (const PPCInsn& insn : b.instructions) in_functions.insert(insn.address);
        for (const auto& [a, imm] : info.imm) {
            if (!in_functions.count(a)) rep.imm_not_emitted++;
        }
        return true;
    }

    const std::string ns = "rel_" + opt.name;
    const std::string prefix = opt.output_dir + "/" + ns;
    const size_t n = rel.sections.size();

    FILE* hdr = fopen((prefix + ".h").c_str(), "w");
    if (!hdr) return fail(error, "cannot write %s.h", prefix.c_str());
    fprintf(hdr, "#pragma once\n// Auto-generated by gcrecomp_recompiler (REL module '%s'): do not edit.\n", opt.name.c_str());
    fprintf(hdr, "#include \"recomp_common.h\"\n\nnamespace %s {\n\n", ns.c_str());
    fprintf(hdr, "// Run-time address of every section of the module, written by the module registry when it finds the\n"
                 "// module the guest linked.\nextern uint32_t base[%zu];\n\n", n);
    for (const auto& [addr, f] : cfg.functions) fprintf(hdr, "void %s(PPCContext* ctx, Memory* mem);\n", f.name.c_str());
    fprintf(hdr, "\n} // namespace %s\n", ns.c_str());
    fclose(hdr);

    std::vector<std::string> files;
    FILE* cur = nullptr;
    unsigned in_file = 0;
    std::set<uint32_t> emitted_imm;
    std::vector<std::pair<uint32_t, std::string>> registered;   // (entry, name)
    auto close_file = [&] {
        if (!cur) return;
        fprintf(cur, "\n} // namespace %s\n", ns.c_str());
        fclose(cur);
        cur = nullptr;
    };
    for (const auto& [addr, func] : cfg.functions) {
        if (!cur || in_file >= (unsigned)std::max(1, opt.funcs_per_file)) {
            close_file();
            char path[600];
            snprintf(path, sizeof path, "%s_%04zu.cpp", prefix.c_str(), files.size());
            cur = fopen(path, "w");
            if (!cur) return fail(error, "cannot write %s", path);
            files.push_back(path);
            fprintf(cur, "// =============================================================\n"
                         "// %s: REL module '%s', position-independent code\n"
                         "// DO NOT EDIT - generated by gcrecomp_recompiler (docs/REL_MODULES.md)\n"
                         "// Addresses in comments are in the analysed layout, not run-time addresses.\n"
                         "// =============================================================\n\n",
                    opt.project.c_str(), opt.name.c_str());
            fprintf(cur, "#include \"%s.h\"\n\nnamespace %s {\n", ns.c_str(), ns.c_str());
            in_file = 0;
        }
        in_file++;
        uint32_t sec = 0, off = 0;
        info.locate(addr, sec, off);
        fprintf(cur, "\n// ---- section %u +0x%X ----\nvoid %s(PPCContext* ctx, Memory* mem) {\n", sec, off, func.name.c_str());
        if (opt.trace) fprintf(cur, "    TRACE_ENTER(%s);\n", info.addr_expr(addr).c_str());
        PPCToCEmitter emitter(cur);
        emitter.block_addrs = func.block_addrs;
        emitter.func_map = &cfg.functions;
        emitter.rel = &info;
        for (uint32_t block_addr : func.block_addrs) {
            const auto& block = func.blocks.at(block_addr);
            emitter.current_block = &block;
            fprintf(cur, "label_%08X:\n", block_addr);
            for (const PPCInsn& insn : block.instructions) emitter.emit_insn(insn);
        }
        fprintf(cur, "}\n");
        rep.unimplemented += emitter.unimplemented;
        rep.unhandled_reloc += emitter.unhandled_reloc;
        emitted_imm.insert(emitter.emitted_imm.begin(), emitter.emitted_imm.end());
        registered.emplace_back(addr, func.name);
    }
    close_file();
    rep.unimplemented -= std::min(rep.unimplemented, rep.unknown_instructions);
    for (const auto& [a, imm] : info.imm) {
        if (!emitted_imm.count(a)) rep.imm_not_emitted++;
    }

    // Registration unit.
    {
        FILE* reg = fopen((prefix + "_register.cpp").c_str(), "w");
        if (!reg) return fail(error, "cannot write %s_register.cpp", prefix.c_str());
        fprintf(reg, "// Auto-generated by gcrecomp_recompiler: registers REL module '%s' with the module registry.\n"
                     "#include \"%s.h\"\n\nnamespace %s {\n\nuint32_t base[%zu];\n\nnamespace {\n\n",
                opt.name.c_str(), ns.c_str(), ns.c_str(), n);
        fprintf(reg, "const uint32_t section_size[%zu] = {", n);
        for (size_t i = 0; i < n; i++) fprintf(reg, "%s0x%Xu", i ? ", " : "", rel.sections[i].size);
        fprintf(reg, "};\n\nconst gcrecomp::RelFunc funcs[] = {\n");
        for (const auto& [addr, name] : registered) {
            uint32_t sec = 0, off = 0;
            info.locate(addr, sec, off);
            fprintf(reg, "    {%uu, 0x%Xu, %s},\n", sec, off, name.c_str());
        }
        uint32_t mask = 0;
        for (size_t i = 0; i < n && i < 32; i++) mask |= rel.sections[i].executable ? (1u << i) : 0u;
        fprintf(reg, "};\n\nconst gcrecomp::RelModuleDesc desc = {\"%s\", %uu, %zuu, section_size, 0x%Xu, base, funcs, %zuu};\n\n",
                opt.name.c_str(), rel.header.version, n, mask, registered.size());
        fprintf(reg, "[[maybe_unused]] const bool registered = (gcrecomp::register_rel_module(&desc), true);\n\n"
                     "} // namespace\n} // namespace %s\n", ns.c_str());
        fclose(reg);
        files.push_back(prefix + "_register.cpp");
    }
    rep.files = (unsigned)files.size() + 1;

    // The generated code must not contain an address of the analysed layout: those are the ones that would be wrong
    // at run time. Comments, labels and function names carry them on purpose; constants do not.
    const uint32_t lo = kRelVirtualBase, hi = info.section_vbase.back() + info.section_size.back();
    for (const std::string& path : files) {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) continue;
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            std::string text = line;
            const size_t comment = text.find("//");
            if (comment != std::string::npos) text.resize(comment);
            for (size_t at = text.find("0x"); at != std::string::npos; at = text.find("0x", at + 2)) {
                char* end = nullptr;
                const unsigned long v = strtoul(text.c_str() + at + 2, &end, 16);
                if (end - (text.c_str() + at + 2) == 8 && v >= lo && v < hi) rep.baked_addresses++;
            }
        }
        fclose(f);
    }
    return true;
}

std::vector<uint32_t> rel_dol_entries(const RELFile& rel, const DOLFile& dol) {
    std::vector<RELPlace> places;
    std::set<uint32_t> entries;
    if (rel.places(places)) {
        for (const RELPlace& p : places) {
            if (p.module_id == 0 && (p.addend & 3) == 0 && dol.is_code(p.addend)) entries.insert(p.addend);
        }
    }
    return std::vector<uint32_t>(entries.begin(), entries.end());
}

} // namespace gcrecomp
