#pragma once
// =============================================================================
// gcrecomp — REL module recompiler (docs/REL_MODULES.md)
//
// Recompiles a Dolphin SDK relocatable module to position-independent C++ plus a registration unit for the run-time
// module registry (src/runtime/rel_modules.cpp). The module is analysed at a fixed virtual layout, every relocated
// instruction is remembered, and the emitter writes the value of each relocation in terms of the run-time section
// bases instead of the analysed address.
// =============================================================================

#include "gcrecomp/dol.h"
#include "gcrecomp/pointer_discovery.h"
#include "gcrecomp/ppc_to_c.h"
#include "gcrecomp/rel.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace gcrecomp {

// Virtual address of the first section in the analysed layout. Far from the DOL's 0x80...: a value in
// [kRelVirtualBase, end of the layout) can only be an address of the module itself.
constexpr uint32_t kRelVirtualBase = 0x90000000u;
constexpr uint32_t kRelMaxLayout = 0x00800000u;  // larger modules are refused: branches to the DOL are parked 16 MB away

struct RelRecompileOptions {
    std::string   name;                 // C identifier of the module (namespace rel_<name>)
    std::string   output_dir;           // empty: analyse only, write nothing
    std::string   project = "gcrecomp";
    int           funcs_per_file = 200;
    bool          trace = false;        // TRACE_ENTER at every function entry
    PointerPolicy pointer_policy = PointerPolicy::Safe;
    std::string   pointer_report;       // optional pointer-discovery report path
};

struct RelReport {
    std::string name;
    uint32_t    id = 0;
    uint32_t    version = 0;
    uint32_t    num_sections = 0;
    uint32_t    code_bytes = 0;
    uint32_t    covered_code_bytes = 0;      // code bytes inside the basic blocks of a function
    uint32_t    uncovered_padding_bytes = 0; // zero words and nops between functions
    struct Range { uint32_t section, offset, length; };
    std::vector<Range> uncovered;            // runs of real code that no function contains

    // relocations of the file
    std::map<unsigned, unsigned> by_type;   // R_PPC type -> count
    unsigned relocs_self = 0, relocs_dol = 0, relocs_other_module = 0;
    unsigned relocs_in_code = 0;            // places inside an executable section
    unsigned imm_ha = 0, imm_lo = 0, imm_hi = 0, imm_plain = 0;  // relocated immediates in code, by kind
    unsigned branches_to_dol = 0;           // relocated branches that leave the module
    unsigned branches_internal = 0;         // relocated branches into the module itself
    unsigned ha_without_lo = 0, lo_without_ha = 0;

    // functions
    unsigned functions = 0;
    unsigned from_bl = 0, from_header = 0, from_data_reloc = 0, from_addr16_reloc = 0;
    unsigned only_data_reloc = 0;           // found by a data relocation and by neither a bl nor the header
    unsigned only_addr16_reloc = 0;
    unsigned other_origin = 0;              // none of the above: prologue scan, tail call, pointer discovery
    unsigned pointer_promoted = 0;          // by the pointer-discovery pass
    unsigned jump_table_labels = 0;         // code relocation targets that are jump-table cases of a function
    unsigned reloc_targets_promoted = 0;    // code relocation targets promoted to functions (not already entries)
    unsigned reloc_targets_promoted_mid = 0;  // ... of which lay inside the body of another function
    unsigned unresolved_bctr = 0;           // bctr with no resolved jump table
    unsigned swept_entries = 0;             // entries added for code nothing else reaches (see recompile_rel_module)

    // emission
    unsigned instructions = 0;
    unsigned unknown_instructions = 0;      // the disassembler rejected them
    unsigned unimplemented = 0;             // decoded but not translated
    unsigned unhandled_reloc = 0;           // relocated but not made position independent
    unsigned imm_not_emitted = 0;           // relocated instructions that no function contains
    unsigned baked_addresses = 0;           // analysed-layout addresses found in the generated code (must be 0)
    unsigned files = 0;

    // verbose: also list every run of code that no function contains
    void print(FILE* out, bool verbose = false) const;
};

// Parses the file and recompiles it. `rel` is consumed (sections are patched in place). Returns false on a malformed
// module or an unsupported relocation; `error` says why.
bool recompile_rel_module(RELFile& rel, const RelRecompileOptions& options, RelReport& report, std::string& error);

// Code addresses of the DOL that the module's relocations name (calls and branches into the DOL, address pairs and
// data words that point at DOL code). They are function entries of the DOL whether or not anything in the DOL
// reaches them: the DOL recompilation must contain them, or the module's call is unresolved at run time.
std::vector<uint32_t> rel_dol_entries(const RELFile& rel, const DOLFile& dol);

} // namespace gcrecomp
