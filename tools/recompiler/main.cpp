// =============================================================================
// gcrecomp Static Recompiler — PPC-to-C translation tool
//
// Generic recompiler for any Gekko-based platform (GameCube, Triforce, Wii).
// Reads a DOL executable, builds a control flow graph, and emits C code
// that operates on the gcrecomp PPCContext and Memory runtime.
//
// Usage: gcrecomp_recompiler [<input.dol>] [--rel <module.rel>]... [options]
//   --map <file>       Load symbol map (Dolphin format)
//   --csv <file>       Load symbol map (CSV format)
//   --output <dir>     Output directory (default: ./recompiled)
//   --extra-funcs <f>  Force-add function entries (one hex addr per line)
//   --discover-pointers <off|safe|aggressive>  Find callback entries (default: safe)
//   --report-pointers <f>  Write the pointer-discovery candidate report
//   --info             Print DOL info and exit
//   --stats            Print CFG statistics and exit
// =============================================================================

#include "gcrecomp/dol.h"
#include "gcrecomp/ppc.h"
#include "gcrecomp/cfg.h"
#include "gcrecomp/symbol_map.h"
#include "gcrecomp/ppc_to_c.h"
#include "gcrecomp/pointer_discovery.h"
#include "gcrecomp/rel.h"
#include "gcrecomp/rel_recomp.h"
#include "gcrecomp/yaz0.h"
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

using namespace gcrecomp;

static void print_usage(const char* prog) {
    printf("gcrecomp Static Recompiler (Gekko PowerPC -> C)\n");
    printf("================================================\n\n");
    printf("Usage: %s [<input.dol>] [--rel <module.rel> [--rel-name <name>]]... [options]\n\n", prog);
    printf("Options:\n");
    printf("  --map <file>         Load Dolphin symbol map\n");
    printf("  --csv <file>         Load CSV symbol map (addr,name)\n");
    printf("  --ghidra <dir>       Load Ghidra headless JSON exports from <dir>\n");
    printf("                       (expects functions.json + symbols.json)\n");
    printf("  --output <dir>       Output directory (default: ./recompiled)\n");
    printf("  --extra-funcs <file> Force-add function entries (one hex addr/line)\n");
    printf("  --funcs-per-file <n> Functions per output file (default: 200)\n");
    printf("  --project <name>     Project name in file headers\n");
    printf("  --discover-pointers <off|safe|aggressive>\n");
    printf("                       Find function entries reached only through pointers\n");
    printf("                       (lis/addi address loads, code addresses in data) and\n");
    printf("                       promote them (default: safe; also --discover-pointers=<p>)\n");
    printf("  --report-pointers <file>\n");
    printf("                       Write the pointer-discovery candidate report ('-' = stdout)\n");
    printf("  --info               Print DOL info and exit\n");
    printf("  --stats              Print CFG statistics and exit\n");
    printf("  --trace              Emit TRACE_ENTER(addr) at every function entry\n");
    printf("                       (records calls in gcrecomp's trace ring)\n");
    printf("  --namespace <name>   Wrap recompiled code in a C++ namespace\n");
    printf("                       (avoids collisions with C stdlib main/memcpy/etc.)\n");
    printf("  --rel <path>         Recompile a REL module (Dolphin SDK relocatable module) to\n");
    printf("                       position-independent code (docs/REL_MODULES.md). May be given\n");
    printf("                       several times, with or without a DOL; output goes to --output\n");
    printf("                       as rel_<name>*.cpp (modules register themselves at start-up)\n");
    printf("  --rel-name <name>    C identifier of the module named by the preceding --rel\n");
    printf("                       (default: the file stem, e.g. FR_m_Top2Btl)\n");
    printf("  --report-rel <file>  Also write the per-module report (counts of functions, relocations,\n");
    printf("                       unknown instructions ...) to <file>\n");
    printf("  --help               Show this help\n");
}

// Read whole file into a vector. Yaz0-aware: if file starts with "Yaz0",
// decompresses in-place and returns the decompressed buffer.
static bool slurp_maybe_yaz0(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", path.c_str());
        return false;
    }
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (sz >= 4 && buf[0] == 'Y' && buf[1] == 'a' && buf[2] == 'z' && buf[3] == '0') {
        uint32_t out_sz = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) |
                          (uint32_t(buf[6]) << 8)  |  uint32_t(buf[7]);
        std::vector<uint8_t> dec(out_sz);
        if (!gcrecomp::yaz0_decompress(buf.data(), buf.size(),
                                        dec.data(), dec.size())) {
            fprintf(stderr, "Yaz0 decompress failed for %s\n", path.c_str());
            return false;
        }
        out = std::move(dec);
    } else {
        out = std::move(buf);
    }
    return true;
}

// Derive a clean basename from a REL path (no dir, no extension, no
// non-identifier chars).
static std::string sanitize_name(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string stem = (slash == std::string::npos) ? path
                                                    : path.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    for (auto& c : stem) {
        if (!(isalnum((unsigned char)c) || c == '_')) c = '_';
    }
    if (stem.empty()) stem = "rel";
    return stem;
}

struct RelInput {
    std::string path;
    std::string name;   // empty: derived from the file name
};

// recomp_common.h: the macros every generated function uses. Written once; a project-supplied one is kept.
static void write_common_header(const std::string& output_dir, bool trace) {
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/recomp_common.h", output_dir.c_str());
        // Do not clobber an existing one. This header is what binds every
        // generated function to a particular runtime -- which g_mem and
        // g_func_table they touch -- so a project that supplies its own
        // must keep it. Overwriting it compiles cleanly and then reads and
        // writes an entirely different, uninitialised Memory at run time.
        if (FILE* existing = fopen(path, "r")) {
            fclose(existing);
            printf("[*] Keeping existing %s (project-supplied runtime bindings)\n",
                   path);
        } else {
        FILE* common = fopen(path, "w");
        if (common) {
            fprintf(common, "#pragma once\n");
            fprintf(common, "// Auto-generated by gcrecomp recompiler\n");
            fprintf(common, "#include \"gcrecomp/runtime.h\"\n");
            fprintf(common, "#include \"gcrecomp/ppc_helpers.h\"\n");
            fprintf(common, "#include <cstring>  // std::memcpy/std::memset used in recompiled code\n");
            fprintf(common, "#include <cmath>    // fabs/sqrt/... used in recompiled code (needed by GCC and Clang)\n\n");
            fprintf(common, "using gcrecomp::PPCContext;\n");
            fprintf(common, "using gcrecomp::Memory;\n");
            fprintf(common, "using gcrecomp::FuncTable;\n");
            fprintf(common, "using gcrecomp::g_mem;\n");
            fprintf(common, "using gcrecomp::g_func_table;\n\n");
            fprintf(common, "// Memory access macros\n");
            fprintf(common, "#define MEM_READ8(addr)         g_mem.read8(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READ16(addr)        g_mem.read16(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READ32(addr)        g_mem.read32(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READF32(addr)       g_mem.readf32(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READF64(addr)       g_mem.readf64(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_WRITE8(addr, val)   g_mem.write8(static_cast<uint32_t>(addr), static_cast<uint8_t>(val))\n");
            fprintf(common, "#define MEM_WRITE16(addr, val)  g_mem.write16(static_cast<uint32_t>(addr), static_cast<uint16_t>(val))\n");
            fprintf(common, "#define MEM_WRITE32(addr, val)  g_mem.write32(static_cast<uint32_t>(addr), static_cast<uint32_t>(val))\n");
            fprintf(common, "#define MEM_WRITEF32(addr, val) g_mem.writef32(static_cast<uint32_t>(addr), static_cast<float>(val))\n");
            fprintf(common, "#define MEM_WRITEF64(addr, val) g_mem.writef64(static_cast<uint32_t>(addr), static_cast<double>(val))\n\n");
            fprintf(common, "// Indirect call macro\n");
            fprintf(common, "#define CALL_INDIRECT(addr, ctx, mem) g_func_table.call(static_cast<uint32_t>(addr), ctx, mem)\n\n");
            if (trace) {
                fprintf(common, "// Function-entry trace (--trace was passed to gcrecomp_recompiler)\n");
                fprintf(common, "#define TRACE_ENTER(addr) gcrecomp::trace_enter(static_cast<uint32_t>(addr))\n\n");
            } else {
                fprintf(common, "// Function-entry trace disabled (rebuild with --trace to enable)\n");
                fprintf(common, "#define TRACE_ENTER(addr) ((void)0)\n\n");
            }
            fprintf(common, "// PSQ wrapper macros (delegate to gcrecomp runtime helpers with g_mem)\n");
            fprintf(common, "#define PSQ_LOAD_ONE(addr, gqr)              psq_load_one(g_mem, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_LOAD_PAIR(ps0, ps1, addr, gqr)   psq_load_pair(g_mem, ps0, ps1, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_STORE_ONE(val, addr, gqr)         psq_store_one(g_mem, val, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_STORE_PAIR(v0, v1, addr, gqr)     psq_store_pair(g_mem, v0, v1, static_cast<uint32_t>(addr), gqr)\n");
            fclose(common);
        }
        }
    }
}

struct LoadedRel {
    RelInput in;
    RELFile  rel;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string dol_path;
    std::string map_path;
    std::string csv_path;
    std::string ghidra_dir;
    std::string extra_funcs_path;
    std::string output_dir = "recompiled";
    std::string project_name = "gcrecomp";
    std::vector<RelInput> rels;
    std::string rel_report_path;
    int funcs_per_file = 200;
    PointerPolicy pointer_policy = PointerPolicy::Safe;
    std::string pointer_report_path;
    bool info_only = false;
    bool stats_only = false;
    bool trace = false;
    std::string ns_name;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (strcmp(argv[i], "--ghidra") == 0 && i + 1 < argc) {
            ghidra_dir = argv[++i];
        } else if (strcmp(argv[i], "--extra-funcs") == 0 && i + 1 < argc) {
            extra_funcs_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_name = argv[++i];
        } else if (strcmp(argv[i], "--funcs-per-file") == 0 && i + 1 < argc) {
            funcs_per_file = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--discover-pointers") == 0 ||
                   strncmp(argv[i], "--discover-pointers=", 20) == 0) {
            const char* v = argv[i][19] == '=' ? argv[i] + 20 : (i + 1 < argc ? argv[++i] : "");
            if (!parse_pointer_policy(v, pointer_policy)) {
                fprintf(stderr, "Error: --discover-pointers expects off, safe or aggressive (got '%s')\n", v);
                return 1;
            }
        } else if (strcmp(argv[i], "--report-pointers") == 0 && i + 1 < argc) {
            pointer_report_path = argv[++i];
        } else if (strcmp(argv[i], "--info") == 0) {
            info_only = true;
        } else if (strcmp(argv[i], "--stats") == 0) {
            stats_only = true;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--namespace") == 0 && i + 1 < argc) {
            ns_name = argv[++i];
        } else if (strcmp(argv[i], "--rel") == 0 && i + 1 < argc) {
            rels.push_back(RelInput{argv[++i], ""});
        } else if (strcmp(argv[i], "--rel-name") == 0 && i + 1 < argc) {
            if (rels.empty()) {
                fprintf(stderr, "Error: --rel-name must follow the --rel it names\n");
                return 1;
            }
            rels.back().name = argv[++i];
        } else if (strcmp(argv[i], "--report-rel") == 0 && i + 1 < argc) {
            rel_report_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            dol_path = argv[i];
        }
    }

    if (dol_path.empty() && rels.empty()) {
        fprintf(stderr, "Error: No DOL or REL file specified\n");
        return 1;
    }

    printf("\n  gcrecomp Static Recompiler\n");
    printf("  Gekko PowerPC 750CXe -> Native C\n\n");

    std::vector<LoadedRel> loaded;
    for (const RelInput& in : rels) {
        std::vector<uint8_t> bytes;
        RELFile rel;
        if (!slurp_maybe_yaz0(in.path, bytes)) return 1;
        if (!rel.load_from_buffer(bytes.data(), bytes.size(), in.path)) {
            fprintf(stderr, "Error: %s is not a REL module\n", in.path.c_str());
            return 1;
        }
        loaded.push_back(LoadedRel{in, std::move(rel)});
    }
    if (info_only) {
        for (const LoadedRel& l : loaded) l.rel.print_info();
    }

    if (!dol_path.empty()) {
    DOLFile host_dol;
    printf("[*] Loading DOL: %s\n", dol_path.c_str());
    if (!host_dol.load(dol_path)) {
        fprintf(stderr, "Failed to load DOL file\n");
        return 1;
    }
    host_dol.print_info();

    if (info_only) return 0;

    DOLFile& dol = host_dol;

    // ---- Load symbols ----
    SymbolMap syms;
    if (!map_path.empty()) syms.load_dolphin_map(map_path);
    if (!csv_path.empty()) syms.load_csv(csv_path);
    if (!ghidra_dir.empty()) {
        std::string fns_path = ghidra_dir + "/functions.json";
        std::string sym_path = ghidra_dir + "/symbols.json";
        syms.load_ghidra_json(fns_path, sym_path);
    }

    // ---- Build CFG ----
    printf("\n[*] Building control flow graph...\n");
    CFG cfg;
    cfg.scan_targets(dol);

    // Promote every symbol-map function entry to a CFG entry. Static
    // analysis misses tiny vtable stubs (`blr` only, called via bclrl)
    // because no `bl` ever references them and no data scan picks them up.
    {
        std::vector<uint32_t> sym_entries;
        for (const auto& [addr, sym] : syms.symbols) {
            if (sym.is_function && addr >= dol.sections.front().address) {
                sym_entries.push_back(addr);
            }
        }
        if (!sym_entries.empty()) {
            printf("[CFG] Adding %zu symbol-map function entries as CFG seeds.\n",
                   sym_entries.size());
            cfg.add_extra_entries(sym_entries);
        }
    }

    // Load extra function entries if specified
    if (!extra_funcs_path.empty()) {
        FILE* ef = fopen(extra_funcs_path.c_str(), "r");
        if (ef) {
            std::vector<uint32_t> extras;
            char line[256];
            while (fgets(line, sizeof(line), ef)) {
                if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
                uint32_t addr = 0;
                if (sscanf(line, "%x", &addr) == 1 && addr != 0) {
                    extras.push_back(addr);
                }
            }
            fclose(ef);
            cfg.add_extra_entries(extras);
        } else {
            fprintf(stderr, "Warning: Could not open extra-funcs file: %s\n",
                    extra_funcs_path.c_str());
        }
    }

    // Code of the DOL that REL modules call or take the address of is a function entry whether or not the DOL names
    // it: nothing in the DOL may call it, and a missing entry is an unresolved call at run time.
    {
        std::vector<uint32_t> from_modules;
        for (const LoadedRel& l : loaded) {
            const std::vector<uint32_t> t = rel_dol_entries(l.rel, dol);
            from_modules.insert(from_modules.end(), t.begin(), t.end());
        }
        if (!from_modules.empty()) {
            printf("[CFG] %zu code addresses of the DOL are used by REL modules\n", from_modules.size());
            cfg.add_extra_entries(from_modules);
        }
    }

    cfg.build_functions(dol);

    // Entries reached only through pointers (callbacks): see docs/POINTER_DISCOVERY.md.
    if (pointer_policy != PointerPolicy::Off || !pointer_report_path.empty()) {
        PointerDiscoveryResult found = discover_pointer_entries(cfg, dol, pointer_policy);
        printf("[CFG] Pointer discovery (%s): %zu candidates, %zu promoted\n",
               pointer_policy_name(pointer_policy), found.candidates.size(), found.promoted);
        for (const auto& c : found.candidates) {
            if (c.promoted) cfg.add_function(c.target, dol);
        }
        if (!pointer_report_path.empty()) {
            FILE* rf = pointer_report_path == "-" ? stdout : fopen(pointer_report_path.c_str(), "w");
            if (rf) {
                write_pointer_report(rf, found);
                if (rf != stdout) fclose(rf);
            } else {
                fprintf(stderr, "Warning: Could not write pointer report: %s\n", pointer_report_path.c_str());
            }
        }
    }
    cfg.print_stats();

    if (!stats_only) {

    // ---- Emit C code ----
    printf("\n[*] Generating C code -> %s/\n", output_dir.c_str());

    std::string mkdir_cmd = "mkdir -p " + output_dir;
    system(mkdir_cmd.c_str());

    int file_index = 0;
    int func_count = 0;
    FILE* current_file = nullptr;

    // recomp_common.h: shared macros and includes for all recompiled code (a project's own is kept).
    write_common_header(output_dir, trace);

    // ---- Assign function names (used by both DOL and REL emit paths) ----
    for (auto& [addr, func] : cfg.functions) {
        func.name = syms.get_name(addr);
    }

    {
        // ---- DOL: forward decl header + registration ----
        char path[512];
        snprintf(path, sizeof(path), "%s/recomp_funcs.h", output_dir.c_str());
        FILE* hdr = fopen(path, "w");
        if (hdr) {
            fprintf(hdr, "#pragma once\n");
            fprintf(hdr, "// Auto-generated: forward declarations for all recompiled functions\n");
            fprintf(hdr, "#include \"recomp_common.h\"\n\n");
            if (!ns_name.empty()) fprintf(hdr, "namespace %s {\n\n", ns_name.c_str());
            for (auto& [addr, func] : cfg.functions) {
                fprintf(hdr, "void %s(PPCContext* ctx, Memory* mem);\n", func.name.c_str());
            }
            if (!ns_name.empty()) fprintf(hdr, "\n} // namespace %s\n", ns_name.c_str());
            fprintf(hdr, "\n// Register all recompiled functions into the function table\n");
            fprintf(hdr, "void register_recompiled_functions(FuncTable& table);\n");
            fclose(hdr);
        }

        snprintf(path, sizeof(path), "%s/recomp_register.cpp", output_dir.c_str());
        FILE* reg = fopen(path, "w");
        if (reg) {
            fprintf(reg, "// Auto-generated: registers all recompiled functions\n");
            fprintf(reg, "#include \"recomp_funcs.h\"\n\n");
            fprintf(reg, "void register_recompiled_functions(FuncTable& table) {\n");
            std::string qual_prefix = ns_name.empty() ? "" : (ns_name + "::");
            for (auto& [addr, func] : cfg.functions) {
                fprintf(reg, "    table.register_func(0x%08X, %s%s);\n",
                        addr, qual_prefix.c_str(), func.name.c_str());
            }
            fprintf(reg, "}\n");
            fclose(reg);
        }

        // Generate recompiled function files (one per N functions).
        for (auto& [addr, func] : cfg.functions) {
            if (func_count % funcs_per_file == 0) {
                if (current_file) {
                    if (!ns_name.empty())
                        fprintf(current_file, "\n} // namespace %s\n", ns_name.c_str());
                    fclose(current_file);
                }
                char filename[512];
                snprintf(filename, sizeof(filename), "%s/recomp_%04d.cpp",
                         output_dir.c_str(), file_index++);
                current_file = fopen(filename, "w");
                if (!current_file) {
                    fprintf(stderr, "Failed to create %s\n", filename);
                    return 1;
                }
                emit_file_header(current_file, project_name.c_str());
                if (!ns_name.empty()) fprintf(current_file, "namespace %s {\n", ns_name.c_str());
            }

            fprintf(current_file, "\n// ---- %s @ 0x%08X ----\n",
                    func.name.c_str(), addr);
            fprintf(current_file, "void %s(PPCContext* ctx, Memory* mem) {\n",
                    func.name.c_str());
            if (trace) fprintf(current_file, "    TRACE_ENTER(0x%08Xu);\n", addr);
            // A function with a real (symbol map) name may have a native replacement in the runtime
            // (OS/DVD/GX HLE). Direct calls reach this function, not the function table, so the
            // override is checked here. Names the runtime does not know fall through to the
            // recompiled body below.
            if (func.name.rfind("func_", 0) != 0) {
                fprintf(current_file,
                        "    { static const gcrecomp::RecompiledFunc hle_ = gcrecomp::lookup_os_func(\"%s\");\n"
                        "      if (hle_) { gcrecomp::hle_count(\"%s\"); hle_(ctx, mem); return; } }\n",
                        func.name.c_str(), func.name.c_str());
            }

            PPCToCEmitter emitter(current_file);
            emitter.block_addrs = func.block_addrs;
            emitter.syms = &syms;
            emitter.func_map = &cfg.functions;
            for (uint32_t block_addr : func.block_addrs) {
                auto& block = func.blocks[block_addr];
                emitter.current_block = &block;
                fprintf(current_file, "label_%08X:\n", block_addr);
                for (const auto& insn : block.instructions) {
                    emitter.emit_insn(insn);
                }
            }
            fprintf(current_file, "}\n");
            func_count++;
        }
        if (current_file) {
            if (!ns_name.empty())
                fprintf(current_file, "\n} // namespace %s\n", ns_name.c_str());
            fclose(current_file);
        }

        printf("\n[*] Done! Generated %d files with %d functions.\n",
               file_index, func_count);
    }
    }
    }

    // ---- REL modules ----
    if (!rels.empty()) {
        if (!stats_only) {
            std::string mkdir_cmd = "mkdir -p " + output_dir;
            system(mkdir_cmd.c_str());
            write_common_header(output_dir, trace);
        }
        FILE* report_file = nullptr;
        if (!rel_report_path.empty()) {
            report_file = fopen(rel_report_path.c_str(), "w");
            if (!report_file) fprintf(stderr, "Warning: Could not write %s\n", rel_report_path.c_str());
        }
        std::set<std::string> names;
        bool failed = false;
        for (LoadedRel& loaded_rel : loaded) {
            const RelInput& in = loaded_rel.in;
            RELFile& rel = loaded_rel.rel;
            printf("\n[*] REL module: %s\n", in.path.c_str());
            rel.print_info();
            RelRecompileOptions options;
            options.name = in.name.empty() ? sanitize_name(in.path) : in.name;
            options.output_dir = stats_only ? "" : output_dir;
            options.project = project_name;
            options.funcs_per_file = funcs_per_file;
            options.trace = trace;
            options.pointer_policy = pointer_policy;
            if (!names.insert(options.name).second) {
                fprintf(stderr, "Error: two modules are named '%s' (use --rel-name)\n", options.name.c_str());
                return 1;
            }
            RelReport report;
            std::string error;
            if (!recompile_rel_module(rel, options, report, error)) {
                fprintf(stderr, "Error: %s: %s\n", in.path.c_str(), error.c_str());
                return 1;
            }
            report.print(stdout);
            if (report_file) report.print(report_file, true);
            if (report.unhandled_reloc != 0 || report.baked_addresses != 0) {
                fprintf(stderr, "Error: %s: the output is not position independent (see the report)\n", in.path.c_str());
                failed = true;
            }
            if (!stats_only) printf("\n[*] Done! Generated rel_%s*.cpp with %u functions.\n", options.name.c_str(), report.functions);
        }
        if (report_file) fclose(report_file);
        if (failed) return 1;
    }

    return 0;
}
