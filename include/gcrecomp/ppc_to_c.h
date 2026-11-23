#pragma once
// =============================================================================
// gcrecomp — PPC to C Emitter
//
// Translates each PowerPC instruction into a C expression operating
// on a PPCContext struct. The generated code compiles with any modern
// C/C++ compiler and runs natively.
// =============================================================================

#include "gcrecomp/ppc.h"
#include "gcrecomp/cfg.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace gcrecomp {

struct SymbolMap;  // fwd; defined in symbol_map.h

// ---- Position-independent code for REL modules (docs/REL_MODULES.md) --------------------------------------------
//
// A module is analysed at a fixed virtual layout, but runs wherever the guest linked it. RelEmitInfo tells the
// emitter which instructions carry a relocation and how to write the value they hold in terms of the run-time
// section bases, so that no virtual address ever reaches the generated expressions.

// How the 16 bits of an instruction's immediate derive from the relocated value T.
enum class RelImmKind { Plain, Lo, Hi, Ha };

struct RelImm {
    RelImmKind  kind;
    std::string target;   // C expression of the 32-bit value T (section base + addend, or an absolute address)
};

struct RelEmitInfo {
    std::vector<uint32_t> section_vbase;   // virtual address of every section in the analysed layout
    std::vector<uint32_t> section_size;
    std::string           base_array;      // C name of the run-time section base array, e.g. "rel_foo::base"
    std::map<uint32_t, RelImm>      imm;           // by address of the instruction
    std::map<uint32_t, std::string> external_branch;  // by address of the branch: C expression of its target

    // virtual address -> (section, offset); false when outside every section
    bool locate(uint32_t vaddr, uint32_t& section, uint32_t& offset) const;
    // C expression of the run-time guest address that stands for the virtual address
    std::string addr_expr(uint32_t vaddr) const;
};

class PPCToCEmitter {
public:
    FILE* out;
    int   indent_level;
    std::vector<uint32_t> block_addrs;
    // Optional: when set, direct branches resolve target addresses to
    // symbolic names. Unset addresses fall back to func_XXXXXXXX.
    const SymbolMap* syms = nullptr;
    const BasicBlock* current_block = nullptr;
    const std::map<uint32_t, Function>* func_map = nullptr;
    // Set when emitting a REL module: see RelEmitInfo.
    const RelEmitInfo* rel = nullptr;
    // Counters kept while emitting (module reports).
    unsigned unimplemented = 0;         // instructions the emitter has no translation for
    unsigned unhandled_reloc = 0;       // relocated instructions the emitter could not make position independent
    std::vector<uint32_t> emitted_imm;  // relocated instructions emitted position-independently

    PPCToCEmitter(FILE* f) : out(f), indent_level(1) {}

    void emit(const char* fmt, ...);
    void emit_raw(const char* fmt, ...);
    void emit_insn(const PPCInsn& insn);

    // Resolve a branch-target address to its declared function name
    // (uses func_map when available; falls back to "func_XXXXXXXX").
    std::string name_for(uint32_t addr) const;

private:
    bool in_reloc_rewrite = false;
    std::string* capture = nullptr;   // when set, emit() appends here instead of writing to `out`
    // Guest address stored in LR for the return address `vaddr` (a literal, or section base + offset in a module).
    std::string lr_value(uint32_t vaddr) const;
    // C statement that transfers control to the branch target of `insn` (goto, or call + return for an external one).
    std::string jump_to(const PPCInsn& insn) const;
    // C statement(s) calling the guest address expression `target` and setting LR to `lr`.
    std::string call_external(const std::string& target, const std::string& lr) const;
    void emit_reloc_insn(const PPCInsn& insn, const RelImm& imm);
};

void emit_file_header(FILE* out, const char* project_name = "gcrecomp");

} // namespace gcrecomp
