#pragma once
// =============================================================================
// gcrecomp — function-entry discovery from code pointers
//
// Callbacks (comparators, vtable slots, handlers stored in structs) are reached only through
// a pointer, so no `bl` names them. Unresolved at run time they show up as
// "[FuncTable] Unresolved call to 0xADDR". This pass finds them statically:
//
//   * address loads   `lis rA,hi` ... `addi rB,rA,lo` / `ori rB,rA,lo` whose result is a text address
//   * data words      4-byte aligned words in the DOL data sections that hold a text address
//
// and then filters hard (see docs/POINTER_DISCOVERY.md): the target must not lie inside an
// already-known function, must follow a function end (blr/b/bctr, or padding), must decode to
// sane code that ends in a terminator instead of running into another function, and so on.
// What survives is ranked; the policy picks the cut.
// =============================================================================

#include "gcrecomp/cfg.h"
#include "gcrecomp/dol.h"
#include <cstdint>
#include <string>
#include <vector>

namespace gcrecomp {

enum class PointerPolicy {
    Off,         // do not promote anything
    Safe,        // promote only high-confidence candidates (default)
    Aggressive,  // also promote weaker evidence (entries after `b`/`bctr`, lone data words, ...)
};

const char* pointer_policy_name(PointerPolicy p);
bool parse_pointer_policy(const std::string& text, PointerPolicy& out);

enum class PointerOriginKind {
    LisAddi,   // lis + addi  (at = lis address, at2 = addi address)
    LisOri,    // lis + ori
    DataWord,  // word in a data section (at = word address, run = length of the code-pointer run)
};

struct PointerOrigin {
    PointerOriginKind kind;
    uint32_t at;
    uint32_t at2 = 0;
    uint32_t run = 1;          // DataWord: consecutive code-pointer words around this one
    bool in_known_code = false;  // LisAddi/LisOri: the pair sits inside an already-known function
};

// Outcome of the filters for one target.
enum class PointerTier {
    Safe,        // promoted by safe and aggressive
    Aggressive,  // promoted by aggressive only
    Rejected,
    Known,       // reserved: targets that already are function entries are only counted
};

struct PointerCandidate {
    uint32_t target = 0;
    std::vector<PointerOrigin> origins;
    PointerTier tier = PointerTier::Rejected;
    int score = 0;
    std::string reason;   // why it landed in this tier (rejections: the filter that fired)
    bool promoted = false;
};

struct PointerDiscoveryResult {
    PointerPolicy policy = PointerPolicy::Safe;
    std::vector<PointerCandidate> candidates;  // sorted by target
    size_t pairs_seen = 0;       // lis+addi/ori pairs, any value
    size_t words_seen = 0;       // data words that hold a text address
    size_t known_seen = 0;       // distinct targets that were already function entries
    size_t promoted = 0;
};

// Run discovery over `dol` given the functions already in `cfg` (build_functions() must have
// run). Nothing in `cfg` is modified: the caller promotes with CFG::add_function() for each
// candidate whose `promoted` flag is set.
PointerDiscoveryResult discover_pointer_entries(const CFG& cfg, const DOLFile& dol, PointerPolicy policy);

// Write the candidate list (tab separated, `#` comment header) to `out`.
void write_pointer_report(FILE* out, const PointerDiscoveryResult& result);

}  // namespace gcrecomp
