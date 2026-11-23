// =============================================================================
// REL module registry - see docs/REL_MODULES.md
//
// The recompiler turns a REL into position-independent code and a RelModuleDesc. This file finds, at run time, where
// the guest linked the module (the OSModuleInfo list the SDK's OSLink maintains) and serves its functions to
// FuncTable::call. Everything is read from guest memory: nothing here depends on the module being at the same
// address twice.
//
// Guest structures (Dolphin SDK OSModuleInfo / OSSectionInfo, layout confirmed against the game's own Link /
// Relocate / OSUnlink code):
//   list head 0x800030C8, tail 0x800030CC
//   OSModuleInfo: +0 id, +4 next, +8 prev, +0xC numSections, +0x10 sectionInfo (absolute after linking),
//                 +0x14 nameOffset, +0x18 nameSize, +0x1C version
//   OSSectionInfo (8 bytes): offset (absolute after linking, bit 0 = executable), length
// =============================================================================

#include "gcrecomp/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gcrecomp {

namespace {

constexpr uint32_t kMaxListNodes = 256;   // guards against a corrupt list
constexpr uint32_t kMaxSections  = 64;

struct Active {
    const RelModuleDesc*  desc = nullptr;
    uint32_t              info = 0;        // guest address of the OSModuleInfo
    std::vector<uint32_t> base;            // per section, 0 for an empty section
    std::unordered_map<uint32_t, RecompiledFunc> funcs;  // guest address -> function
};

struct Registry {
    std::mutex                       lock;
    std::vector<const RelModuleDesc*> descs;
    std::vector<Active>              active;
};

Registry& registry() {
    static Registry r;
    return r;
}

bool ram_word(uint32_t addr) {
    return addr >= Memory::MAIN_RAM_BASE && addr + 4 <= g_mem.ram_end && (addr & 3) == 0;
}

uint32_t guest32(uint32_t addr) { return ram_word(addr) ? g_mem.read32(addr) : 0; }

struct NodeView {
    uint32_t info = 0;
    uint32_t id = 0;
    uint32_t num_sections = 0;
    uint32_t version = 0;
    uint32_t section_info = 0;
    bool     ok = false;
};

NodeView read_node(uint32_t info) {
    NodeView n;
    n.info = info;
    if (!ram_word(info)) return n;
    n.id           = guest32(info + 0x00);
    n.num_sections = guest32(info + 0x0C);
    n.section_info = guest32(info + 0x10);
    n.version      = guest32(info + 0x1C);
    n.ok = n.num_sections != 0 && n.num_sections <= kMaxSections && ram_word(n.section_info) &&
           ram_word(n.section_info + n.num_sections * 8 - 4);
    return n;
}

uint32_t section_base(const NodeView& n, uint32_t i) { return guest32(n.section_info + i * 8) & ~1u; }
uint32_t section_len(const NodeView& n, uint32_t i)  { return guest32(n.section_info + i * 8 + 4); }
bool     section_exec(const NodeView& n, uint32_t i) { return (guest32(n.section_info + i * 8) & 1u) != 0; }

template <class F>
void for_each_node(F&& f) {
    uint32_t p = guest32(OS_MODULE_INFO_HEAD);
    for (uint32_t i = 0; p != 0 && i < kMaxListNodes; ++i) {
        const NodeView n = read_node(p);
        if (!n.ok) break;
        if (f(n)) return;
        p = guest32(p + 4);
    }
}

bool signature_matches(const RelModuleDesc& d, const NodeView& n) {
    if (d.version != n.version || d.num_sections != n.num_sections) return false;
    for (uint32_t i = 0; i < n.num_sections; ++i) {
        if (d.section_size[i] != section_len(n, i)) return false;
        if (((d.exec_mask >> i) & 1u) != (section_exec(n, i) ? 1u : 0u)) return false;
    }
    return true;
}

// The node whose section holds addr.
bool find_node_containing(uint32_t addr, NodeView& out, uint32_t* sec_out = nullptr) {
    bool found = false;
    for_each_node([&](const NodeView& n) {
        for (uint32_t i = 0; i < n.num_sections; ++i) {
            const uint32_t len = section_len(n, i);
            const uint32_t b = section_base(n, i);
            if (len != 0 && b != 0 && addr >= b && addr - b < len) {
                out = n;
                if (sec_out) *sec_out = i;
                found = true;
                return true;
            }
        }
        return false;
    });
    return found;
}

// Why the activation no longer describes what the guest has linked (nullptr when it does): the node is gone from the
// list, or its sections moved.
const char* invalid_reason(const Active& a) {
    bool in_list = false;
    NodeView node;
    for_each_node([&](const NodeView& n) {
        if (n.info != a.info) return false;
        in_list = true;
        node = n;
        return true;
    });
    if (!in_list) return "node no longer in the guest module list";
    if (node.num_sections != a.desc->num_sections || node.version != a.desc->version) return "node changed shape";
    for (uint32_t i = 0; i < node.num_sections; ++i) {
        if (section_len(node, i) != 0 && section_base(node, i) != a.base[i]) return "section bases moved";
    }
    return nullptr;
}

bool still_valid(const Active& a) { return invalid_reason(a) == nullptr; }

// GCRECOMP_TRACE_REL=1: every activation, drop and refusal, with the guest heap state at each link.
bool trace_rel() {
    static const bool on = [] {
        const char* v = std::getenv("GCRECOMP_TRACE_REL");
        return v && *v && *v != '0';
    }();
    return on;
}

bool in_exec_range(const Active& a, uint32_t addr) {
    for (uint32_t i = 0; i < a.desc->num_sections; ++i) {
        if (((a.desc->exec_mask >> i) & 1u) && a.base[i] != 0 && addr >= a.base[i] &&
            addr - a.base[i] < a.desc->section_size[i])
            return true;
    }
    return false;
}

void erase_active_of(Registry& r, const RelModuleDesc* d) {
    r.active.erase(std::remove_if(r.active.begin(), r.active.end(), [&](const Active& a) { return a.desc == d; }),
                   r.active.end());
}

}  // namespace

void register_rel_module(const RelModuleDesc* desc) {
    Registry& r = registry();
    std::lock_guard<std::mutex> g(r.lock);
    for (const RelModuleDesc* other : r.descs) {
        bool same = other->version == desc->version && other->num_sections == desc->num_sections &&
                    other->exec_mask == desc->exec_mask;
        for (uint32_t i = 0; same && i < desc->num_sections; ++i) same = other->section_size[i] == desc->section_size[i];
        if (same) {
            std::fprintf(stderr, "[REL] module '%s' has the same signature as '%s': the first one wins\n", desc->name,
                         other->name);
            return;
        }
    }
    r.descs.push_back(desc);
}

RecompiledFunc rel_lookup(uint32_t gc_addr) {
    Registry& r = registry();
    std::lock_guard<std::mutex> g(r.lock);
    if (r.descs.empty()) return nullptr;

    // An activation that holds the address answers, provided the guest still has the module there.
    for (size_t i = 0; i < r.active.size(); ++i) {
        Active& a = r.active[i];
        if (!in_exec_range(a, gc_addr)) continue;
        if (still_valid(a)) {
            auto it = a.funcs.find(gc_addr);
            return it == a.funcs.end() ? nullptr : it->second;
        }
        if (trace_rel())
            std::fprintf(stderr, "[REL] drop activation of '%s' (node 0x%08X) on a call to 0x%08X: %s\n", a.desc->name,
                         a.info, gc_addr, invalid_reason(a));
        r.active.erase(r.active.begin() + i);  // unlinked, freed or relinked elsewhere
        break;
    }

    NodeView node;
    if (!find_node_containing(gc_addr, node)) return nullptr;
    const RelModuleDesc* desc = nullptr;
    for (const RelModuleDesc* d : r.descs) {
        if (signature_matches(*d, node)) { desc = d; break; }
    }
    if (!desc) return nullptr;

    for (const Active& a : r.active) {
        if (a.desc != desc) continue;
        if (still_valid(a)) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                             "[REL] module '%s' is linked twice (0x%08X and 0x%08X): only one instance is supported\n",
                             desc->name, a.info, node.info);
            }
            return nullptr;
        }
        if (trace_rel())
            std::fprintf(stderr, "[REL] replacing the activation of '%s' (node 0x%08X): %s\n", desc->name, a.info,
                         invalid_reason(a));
        break;
    }
    erase_active_of(r, desc);

    Active act;
    act.desc = desc;
    act.info = node.info;
    act.base.assign(desc->num_sections, 0);
    for (uint32_t i = 0; i < desc->num_sections; ++i) {
        act.base[i] = section_len(node, i) != 0 ? section_base(node, i) : 0;
        desc->base[i] = act.base[i];
    }
    act.funcs.reserve(desc->num_funcs * 2);
    for (uint32_t i = 0; i < desc->num_funcs; ++i) {
        const RelFunc& f = desc->funcs[i];
        if (f.section < desc->num_sections && act.base[f.section] != 0) act.funcs[act.base[f.section] + f.offset] = f.fn;
    }
    std::printf("[REL] module '%s' (id %u) linked at 0x%08X: %zu functions registered\n", desc->name, node.id, node.info,
                act.funcs.size());
    std::fflush(stdout);
    if (trace_rel()) {
        for (uint32_t i = 0; i < desc->num_sections; ++i)
            std::fprintf(stderr, "[REL]   section %u at 0x%08X length 0x%X%s\n", i, act.base[i], desc->section_size[i],
                         ((desc->exec_mask >> i) & 1u) ? " (code)" : "");
        dump_guest_heaps("module link", &g_ctx, false);
    }
    r.active.push_back(std::move(act));
    const Active& a = r.active.back();
    auto it = a.funcs.find(gc_addr);
    return it == a.funcs.end() ? nullptr : it->second;
}

uint32_t rel_import_base(uint32_t module_id, uint32_t section) {
    uint32_t result = 0;
    for_each_node([&](const NodeView& n) {
        if (n.id != module_id) return false;
        if (section < n.num_sections) result = section_base(n, section);
        return true;
    });
    return result;
}

void rel_invalidate_range(uint32_t addr, uint32_t size) {
    Registry& r = registry();
    std::lock_guard<std::mutex> g(r.lock);
    if (r.active.empty()) return;
    if (trace_rel()) std::fprintf(stderr, "[REL] ICInvalidateRange 0x%08X+0x%X with %zu active module(s)\n", addr, size, r.active.size());
    const uint64_t end = uint64_t(addr) + size;
    r.active.erase(std::remove_if(r.active.begin(), r.active.end(),
                                  [&](const Active& a) {
                                      for (uint32_t i = 0; i < a.desc->num_sections; ++i) {
                                          if (a.base[i] != 0 && ((a.desc->exec_mask >> i) & 1u) &&
                                              a.base[i] < end && uint64_t(a.base[i]) + a.desc->section_size[i] > addr)
                                              return true;
                                      }
                                      return false;
                                  }),
                   r.active.end());
}

std::string rel_describe(uint32_t gc_addr) {
    Registry& r = registry();
    std::lock_guard<std::mutex> g(r.lock);
    NodeView node;
    uint32_t sec = 0;
    if (!find_node_containing(gc_addr, node, &sec)) return {};
    const char* name = nullptr;
    for (const RelModuleDesc* d : r.descs) {
        if (signature_matches(*d, node)) { name = d->name; break; }
    }
    char buf[160];
    if (name)
        std::snprintf(buf, sizeof buf, "inside module '%s' (id %u) section %u +0x%X", name, node.id, sec,
                      gc_addr - section_base(node, sec));
    else
        std::snprintf(buf, sizeof buf, "inside module id %u section %u +0x%X (no recompiled module has this signature)",
                      node.id, sec, gc_addr - section_base(node, sec));
    return buf;
}

void rel_reset_active() {
    Registry& r = registry();
    std::lock_guard<std::mutex> g(r.lock);
    r.active.clear();
}

void rel_dump_state() {
    Registry& r = registry();
    std::lock_guard<std::mutex> g(r.lock);
    std::fprintf(stderr, "[REL] state: head=0x%08X tail=0x%08X, %zu recompiled module(s), %zu active\n",
                 guest32(OS_MODULE_INFO_HEAD), guest32(OS_MODULE_INFO_HEAD + 4), r.descs.size(), r.active.size());
    uint32_t count = 0;
    for_each_node([&](const NodeView& n) {
        ++count;
        std::fprintf(stderr, "[REL]   node 0x%08X id %u version %u, %u section(s):", n.info, n.id, n.version, n.num_sections);
        for (uint32_t i = 0; i < n.num_sections && i < 8; ++i)
            std::fprintf(stderr, " [%u]=0x%08X+0x%X", i, section_base(n, i), section_len(n, i));
        std::fprintf(stderr, "\n");
        return false;
    });
    if (count == 0) std::fprintf(stderr, "[REL]   the guest module list is empty or unreadable\n");
    for (const Active& a : r.active) {
        const char* why = invalid_reason(a);
        std::fprintf(stderr, "[REL]   active '%s' node 0x%08X: %s\n", a.desc->name, a.info,
                     why ? why : "valid");
    }
    std::fflush(stderr);
}

}  // namespace gcrecomp
