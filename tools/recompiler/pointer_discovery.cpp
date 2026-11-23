// =============================================================================
// Function-entry discovery from code pointers (see include/gcrecomp/pointer_discovery.h and
// docs/POINTER_DISCOVERY.md).
// =============================================================================

#include "gcrecomp/pointer_discovery.h"
#include "gcrecomp/ppc.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace gcrecomp {

const char* pointer_policy_name(PointerPolicy p) {
    switch (p) {
        case PointerPolicy::Off: return "off";
        case PointerPolicy::Safe: return "safe";
        case PointerPolicy::Aggressive: return "aggressive";
    }
    return "?";
}

bool parse_pointer_policy(const std::string& text, PointerPolicy& out) {
    if (text == "off") out = PointerPolicy::Off;
    else if (text == "safe") out = PointerPolicy::Safe;
    else if (text == "aggressive") out = PointerPolicy::Aggressive;
    else return false;
    return true;
}

namespace {

constexpr int kPairWindow = 16;       // instructions a `lis` stays live waiting for its low half
constexpr int kWalkLimit = 4096;      // instructions examined when validating a body
constexpr int kPaddingLimit = 8;      // padding words tolerated between a function end and its successor

constexpr int kScoreSafe = 70;
constexpr int kScoreAggressive = 35;

// ---- Which GPR does an instruction overwrite? (conservative, only used to expire a tracked `lis`) ----

uint32_t written_gprs(uint32_t raw) {
    const uint32_t op = raw >> 26;
    const uint32_t rd = (raw >> 21) & 31, ra = (raw >> 16) & 31;
    auto bit = [](uint32_t r) { return 1u << r; };
    switch (op) {
        case 7: case 8: case 12: case 13: case 14: case 15:        // mulli subfic addic addic. addi addis
        case 32: case 34: case 40: case 42:                         // lwz lbz lhz lha
            return bit(rd);
        case 33: case 35: case 41: case 43:                         // update-form loads
            return bit(rd) | bit(ra);
        case 36: case 37: case 38: case 39: case 44: case 45:     // update-form stores write rA
            return (op == 37 || op == 39 || op == 45) ? bit(ra) : 0;
        case 20: case 21: case 23:                                  // rlwimi rlwinm rlwnm
        case 24: case 25: case 26: case 27: case 28: case 29:      // ori oris xori xoris andi. andis.
            return bit(ra);
        case 46: {                                                  // lmw: rD..r31
            uint32_t m = 0;
            for (uint32_t r = rd; r < 32; r++) m |= bit(r);
            return m;
        }
        case 31: {
            const uint32_t xo = (raw >> 1) & 0x3FF;
            switch (xo) {
                case 0: case 32: case 4:                            // cmp cmpl tw
                case 151: case 215: case 407: case 662: case 918:  // stores
                case 150: case 598: case 854: case 86: case 54: case 246: case 278: case 470:  // sync/cache
                case 467: case 144: case 146: case 210: case 242: case 306:  // mtspr/mtcrf/mtmsr/mtsr...
                    return 0;
                case 183: case 247: case 439: return bit(ra);      // update-form stores
                case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
                case 954: case 922: case 26: case 24: case 536: case 792: case 824: case 27: case 539:
                    return bit(ra);                                 // logical / shift / extend
                case 119: case 311: case 375: case 55: case 567: case 631:
                    return bit(rd) | bit(ra);                       // update-form indexed loads
                default:
                    return bit(rd);                                 // arithmetic, indexed loads, mfspr, ...
            }
        }
        default:
            return 0;
    }
}

// ---- Coverage of the known functions ----------------------------------------------------------------

struct Coverage {
    std::map<uint32_t, uint32_t> spans;  // start -> end, disjoint
    std::set<uint32_t> entries;
    std::map<uint32_t, bool> unresolved_bctr;  // function entry -> has a bctr with no resolved table

    void add_span(uint32_t s, uint32_t e) {
        if (e <= s) return;
        auto it = spans.upper_bound(s);
        if (it != spans.begin()) {
            auto p = std::prev(it);
            if (p->second >= s) { s = p->first; e = std::max(e, p->second); it = spans.erase(p); }
        }
        while (it != spans.end() && it->first <= e) { e = std::max(e, it->second); it = spans.erase(it); }
        spans[s] = e;
    }
    void add_function(const Function& f) {
        entries.insert(f.entry);
        bool unresolved = false;
        for (const auto& [a, b] : f.blocks) {
            add_span(b.start, b.end);
            if (!b.instructions.empty()) {
                const PPCInsn& last = b.instructions.back();
                if (last.type == PPCInsnType::BCCTR && !last.link && b.jump_table_targets.empty()) unresolved = true;
            }
        }
        unresolved_bctr[f.entry] = unresolved;
    }
    bool covered(uint32_t a) const {
        auto it = spans.upper_bound(a);
        if (it == spans.begin()) return false;
        --it;
        return a >= it->first && a < it->second;
    }
    // The covering span, for diagnostics.
    bool span_of(uint32_t a, uint32_t& s, uint32_t& e) const {
        auto it = spans.upper_bound(a);
        if (it == spans.begin()) return false;
        --it;
        if (a < it->first || a >= it->second) return false;
        s = it->first; e = it->second;
        return true;
    }
    // Nearest known entry at or below `a`.
    bool entry_below(uint32_t a, uint32_t& e) const {
        auto it = entries.upper_bound(a);
        if (it == entries.begin()) return false;
        e = *std::prev(it);
        return true;
    }
};

// ---- What precedes a target ---------------------------------------------------------------------------

enum class Before { Blr, Branch, Bctr, None };

// Classify the instruction run ending at target-4, skipping padding words (0 or nop).
// `padding` is set if any were skipped. Before::None means non-terminator (target is reached by fall-through).
Before classify_before(uint32_t target, const DOLFile& dol, bool& padding) {
    padding = false;
    uint32_t at = target;
    for (int i = 0; i <= kPaddingLimit; i++) {
        at -= 4;
        if (!dol.is_code(at)) return Before::None;
        const uint32_t raw = dol.read32(at);
        if (raw == 0 || raw == 0x60000000) { padding = true; continue; }
        const PPCInsn insn = ppc_disasm(raw, at);
        if (insn.is_return()) return Before::Blr;
        if (insn.type == PPCInsnType::B && !insn.link) return Before::Branch;
        if (insn.type == PPCInsnType::BCCTR && !insn.link && !insn.is_conditional()) return Before::Bctr;
        return Before::None;
    }
    return Before::None;
}

// ---- Body walk -----------------------------------------------------------------------------------------

enum class Walk { Ok, BadInsn, BadBranch, RunsIntoEntry, EntersBody, RunsOffEnd };

const char* walk_name(Walk w) {
    switch (w) {
        case Walk::Ok: return "ok";
        case Walk::BadInsn: return "undecodable instruction";
        case Walk::BadBranch: return "branch/call target outside code";
        case Walk::RunsIntoEntry: return "falls through into a known function start";
        case Walk::EntersBody: return "branches/falls into the middle of a known function";
        case Walk::RunsOffEnd: return "runs off the end of the text section";
    }
    return "?";
}

// Follow every path from `target` until a terminator. A tail call (`b`) to a known entry is fine;
// falling into one, or reaching the middle of another function, is not.
// Metrowerks schedules `mflr r0` / `stw r0,4(r1)` / address loads ahead of the `stwu r1,-N(r1)` that
// opens the frame, so a scan for `stwu` (CFG phase 1.6) plants a bogus entry a few instructions into a
// real function. Falling into such an entry from a target that opens with `mflr r0` is the same function.
bool hoisted_prologue_entry(uint32_t target, uint32_t entry, const DOLFile& dol) {
    if (entry <= target || entry - target > 32) return false;
    const PPCInsn first = ppc_disasm(dol.read32(target), target);
    if (first.type != PPCInsnType::MFSPR || first.spr != 8) return false;
    const PPCInsn at = ppc_disasm(dol.read32(entry), entry);
    if (!(at.type == PPCInsnType::STWU && at.ra == 1 && at.rs == 1 && at.simm < 0)) return false;
    for (uint32_t a = target; a < entry; a += 4)
        if (ppc_disasm(dol.read32(a), a).is_branch()) return false;
    return true;
}

Walk walk_body(uint32_t target, const DOLFile& dol, const Coverage& cov, uint32_t& where) {
    std::set<uint32_t> seen;
    std::vector<std::pair<uint32_t, bool>> work;  // address, reached by fall-through
    work.push_back({target, true});
    int budget = kWalkLimit;
    Walk soft = Walk::Ok;
    while (!work.empty()) {
        auto [pc, fall] = work.back();
        work.pop_back();
        while (true) {
            if (seen.count(pc)) break;
            if (!dol.is_code(pc)) { where = pc; return fall ? Walk::RunsOffEnd : Walk::BadBranch; }
            if (pc != target && cov.entries.count(pc)) {
                if (fall && hoisted_prologue_entry(target, pc, dol)) break;  // same function, see above
                if (fall) { where = pc; return Walk::RunsIntoEntry; }
                break;  // tail call
            }
            if (pc != target && cov.covered(pc)) { where = pc; soft = Walk::EntersBody; break; }
            if (--budget < 0) return soft;  // very large body: accept what was checked
            seen.insert(pc);
            const PPCInsn insn = ppc_disasm(dol.read32(pc), pc);
            if (insn.type == PPCInsnType::UNKNOWN) { where = pc; return Walk::BadInsn; }
            fall = true;
            if (insn.type == PPCInsnType::B) {
                const uint32_t t = (uint32_t)insn.branch_target;
                if (!dol.is_code(t)) { where = pc; return Walk::BadBranch; }
                if (insn.link) { pc += 4; continue; }
                work.push_back({t, false});
                break;
            }
            if (insn.type == PPCInsnType::BC) {
                const uint32_t t = (uint32_t)insn.branch_target;
                if (!dol.is_code(t)) { where = pc; return Walk::BadBranch; }
                if (!insn.link) work.push_back({t, false});
                pc += 4;
                continue;
            }
            if (insn.type == PPCInsnType::BCLR || insn.type == PPCInsnType::BCCTR) {
                if (insn.link || insn.is_conditional()) { pc += 4; continue; }
                break;  // blr / bctr
            }
            if (insn.type == PPCInsnType::RFI) break;
            pc += 4;
        }
    }
    return soft;
}

// ---- Origins -------------------------------------------------------------------------------------------

struct RawOrigins {
    std::map<uint32_t, std::vector<PointerOrigin>> by_target;
    size_t pairs = 0, words = 0;
};

void scan_address_loads(const DOLFile& dol, const Coverage& cov, RawOrigins& out) {
    for (const auto& sec : dol.sections) {
        if (!sec.is_text) continue;
        uint32_t val[32] = {0};
        uint32_t src[32] = {0};   // address of the lis
        int age[32];
        bool live[32] = {false};
        int idx = 0;
        for (uint32_t off = 0; off + 4 <= sec.size; off += 4, idx++) {
            const uint32_t at = sec.address + off;
            const uint32_t raw = dol.read32(at);
            const uint32_t op = raw >> 26;
            const uint32_t rd = (raw >> 21) & 31, ra = (raw >> 16) & 31;
            for (int r = 0; r < 32; r++)
                if (live[r] && idx - age[r] > kPairWindow) live[r] = false;

            // Any branch ends the straight-line run: registers do not carry meaningfully across it.
            if (op == 18 || op == 16 || (op == 19 && (((raw >> 1) & 0x3FF) == 16 || ((raw >> 1) & 0x3FF) == 528))) {
                for (bool& l : live) l = false;
                continue;
            }

            auto record = [&](PointerOriginKind kind, uint32_t hi_reg, uint32_t value) {
                out.pairs++;
                PointerOrigin o;
                o.kind = kind;
                o.at = src[hi_reg];
                o.at2 = at;
                o.in_known_code = cov.covered(at);
                if (dol.is_code(value)) out.by_target[value].push_back(o);
            };

            if (op == 14 && live[ra] && ra != 0) {  // addi rD, rA, simm
                record(PointerOriginKind::LisAddi, ra, val[ra] + (uint32_t)(int32_t)(int16_t)raw);
            } else if (op == 24 && live[rd]) {  // ori rA, rS, uimm  (rS in the rD field)
                record(PointerOriginKind::LisOri, rd, val[rd] | (raw & 0xFFFF));
            }
            const uint32_t w = written_gprs(raw);
            for (int r = 0; r < 32; r++)
                if (w & (1u << r)) live[r] = false;
            if (op == 15 && ra == 0) {  // lis rD, imm
                val[rd] = (raw & 0xFFFF) << 16;
                src[rd] = at;
                age[rd] = idx;
                live[rd] = true;
            }
        }
    }
}

void scan_data_words(const DOLFile& dol, RawOrigins& out) {
    for (const auto& sec : dol.sections) {
        if (sec.is_text) continue;
        std::vector<uint32_t> words;
        for (uint32_t off = 0; off + 4 <= sec.size; off += 4) words.push_back(dol.read32(sec.address + off));
        const size_t n = words.size();
        size_t i = 0;
        while (i < n) {
            if (!dol.is_code(words[i])) { i++; continue; }
            size_t j = i;
            while (j < n && dol.is_code(words[j])) j++;
            for (size_t k = i; k < j; k++) {
                PointerOrigin o;
                o.kind = PointerOriginKind::DataWord;
                o.at = sec.address + (uint32_t)k * 4;
                o.run = (uint32_t)(j - i);
                out.by_target[words[k]].push_back(o);
                out.words++;
            }
            i = j;
        }
    }
}

const char* kind_name(PointerOriginKind k) {
    switch (k) {
        case PointerOriginKind::LisAddi: return "lis+addi";
        case PointerOriginKind::LisOri: return "lis+ori";
        case PointerOriginKind::DataWord: return "data";
    }
    return "?";
}

}  // namespace

// ---- Driver ----------------------------------------------------------------------------------------------

PointerDiscoveryResult discover_pointer_entries(const CFG& cfg, const DOLFile& dol, PointerPolicy policy) {
    PointerDiscoveryResult result;
    result.policy = policy;

    Coverage cov;
    for (const auto& [entry, f] : cfg.functions) cov.add_function(f);
    for (uint32_t e : cfg.call_targets) cov.entries.insert(e);

    RawOrigins raw;
    scan_address_loads(dol, cov, raw);
    scan_data_words(dol, raw);
    result.pairs_seen = raw.pairs;
    result.words_seen = raw.words;

    CFG scratch;  // build_blocks() for trial bodies; leaves `cfg` untouched

    for (auto& [target, origins] : raw.by_target) {  // ascending: accepted bodies cover later candidates
        PointerCandidate c;
        c.target = target;
        c.origins = std::move(origins);
        auto reject = [&](const std::string& why) {
            c.tier = PointerTier::Rejected;
            c.reason = why;
            result.candidates.push_back(std::move(c));
        };
        char buf[160];

        if (cov.entries.count(target)) {
            c.tier = PointerTier::Known;
            c.reason = "already a function entry before this pass";
            result.known_seen++;
            result.candidates.push_back(std::move(c));
            continue;
        }
        if (target & 3) { reject("unaligned"); continue; }
        uint32_t s, e;
        if (cov.span_of(target, s, e)) {
            uint32_t owner = 0;
            cov.entry_below(target, owner);
            snprintf(buf, sizeof buf, "mid-function (inside known code 0x%08X-0x%08X, nearest entry 0x%08X)", s, e, owner);
            reject(buf);
            continue;
        }

        bool padding = false;
        const Before before = classify_before(target, dol, padding);
        if (before == Before::None) { reject("not preceded by a function end (falls through from code above)"); continue; }

        // Doubtful shapes. `safe` rejects them; `aggressive` keeps them at a penalty (hand-written
        // asm such as exception handlers trips these although they are real entries).
        int penalty = 0;
        std::string doubt;
        if (!looks_like_function_entry(target, dol)) {
            penalty += 30;
            doubt = "entry tears down a frame it never set up (mid-function)";
        }
        uint32_t where = 0;
        const Walk walk = walk_body(target, dol, cov, where);
        if (walk == Walk::BadInsn) {
            snprintf(buf, sizeof buf, "body: %s at 0x%08X", walk_name(walk), where);
            reject(buf);
            continue;
        }
        if (walk != Walk::Ok && walk != Walk::EntersBody) {
            penalty += 30;
            snprintf(buf, sizeof buf, "body: %s at 0x%08X", walk_name(walk), where);
            if (!doubt.empty()) doubt += "; ";
            doubt += buf;
        }

        // ---- score ----
        int score = 0;
        std::string why;
        auto note = [&](const char* s) { if (!why.empty()) why += ", "; why += s; };

        uint32_t owner = 0;
        const bool have_owner = cov.entry_below(target, owner);
        const bool owner_unresolved = have_owner && cov.unresolved_bctr[owner];
        const bool prev_covered = cov.covered(target - 4);

        switch (before) {
            case Before::Blr:
                score += padding ? 35 : 40;
                note(prev_covered ? "after blr of a known function" : (padding ? "after blr+padding" : "after blr"));
                break;
            case Before::Branch: score += 15; note(prev_covered ? "after b of a known function" : "after b"); break;
            case Before::Bctr: score += 15; note("after bctr"); break;
            case Before::None: break;
        }
        if (owner_unresolved) { score -= 25; note("preceding function has an unresolved bctr (possible switch cases)"); }

        int origin_score = 0;
        bool has_code_pair = false, has_word = false;
        for (const PointerOrigin& o : c.origins) {
            int s2 = 0;
            if (o.kind == PointerOriginKind::DataWord) {
                s2 = 25 + (o.run >= 2 ? 5 : 0);
                has_word = true;
            } else {
                s2 = o.in_known_code ? 30 : 10;
                if (o.in_known_code) has_code_pair = true;
            }
            origin_score = std::max(origin_score, s2);
        }
        score += origin_score;
        if (c.origins.size() > 1) { score += 10; note("multiple origins"); }
        if (has_code_pair && has_word) { score += 5; note("code and data origin"); }
        if (walk == Walk::Ok) { score += 10; note("body decodes cleanly"); }
        else if (walk == Walk::EntersBody) { note("body enters a known function"); }
        {
            const PPCInsn first = ppc_disasm(dol.read32(target), target);
            if (first.type == PPCInsnType::STWU && first.ra == 1 && first.rs == 1 && first.simm < 0) { score += 5; note("opens a stack frame"); }
        }

        if (penalty) { score -= penalty; note(("doubtful: " + doubt).c_str()); }
        c.score = score;
        const bool safe_ok = (before == Before::Blr) && !owner_unresolved && walk == Walk::Ok && penalty == 0 && score >= kScoreSafe;
        const bool aggr_ok = score >= kScoreAggressive;
        if (safe_ok) {
            c.tier = PointerTier::Safe;
        } else if (aggr_ok) {
            c.tier = PointerTier::Aggressive;
        } else {
            c.reason = "low confidence (" + why + ")";
            c.tier = PointerTier::Rejected;
            result.candidates.push_back(std::move(c));
            continue;
        }
        c.reason = why;
        c.promoted = (policy == PointerPolicy::Aggressive) || (policy == PointerPolicy::Safe && c.tier == PointerTier::Safe);
        if (c.promoted) {
            result.promoted++;
            Function f;
            f.entry = target;
            scratch.build_blocks(f, dol);
            cov.add_function(f);
        }
        result.candidates.push_back(std::move(c));
    }
    return result;
}

void write_pointer_report(FILE* out, const PointerDiscoveryResult& r) {
    size_t n_safe = 0, n_aggr = 0, n_rej = 0;
    for (const auto& c : r.candidates) {
        switch (c.tier) {
            case PointerTier::Safe: n_safe++; break;
            case PointerTier::Aggressive: n_aggr++; break;
            case PointerTier::Rejected: n_rej++; break;
            case PointerTier::Known: break;  // counted via known_seen
        }
    }
    fprintf(out, "# gcrecomp pointer-discovery report (policy=%s)\n", pointer_policy_name(r.policy));
    fprintf(out, "# lis+addi/ori pairs seen: %zu, data words holding a text address: %zu\n", r.pairs_seen, r.words_seen);
    fprintf(out, "# targets: %zu safe, %zu aggressive-only, %zu rejected; %zu more targets were already function entries; promoted by policy: %zu\n",
            n_safe, n_aggr, n_rej, r.known_seen, r.promoted);
    fprintf(out, "# columns: target\ttier\tscore\tpromoted\treason\torigins (kind@address)\n");
    for (const auto& c : r.candidates) {
        const char* tier = c.tier == PointerTier::Safe ? "safe" : c.tier == PointerTier::Aggressive ? "aggressive"
                         : c.tier == PointerTier::Known ? "known" : "rejected";
        fprintf(out, "0x%08X\t%s\t%d\t%s\t%s\t", c.target, tier, c.score, c.promoted ? "yes" : "no", c.reason.c_str());
        for (size_t i = 0; i < c.origins.size(); i++) {
            const PointerOrigin& o = c.origins[i];
            if (i) fputc(' ', out);
            if (o.kind == PointerOriginKind::DataWord) {
                fprintf(out, "%s@0x%08X(run=%u)", kind_name(o.kind), o.at, o.run);
            } else {
                fprintf(out, "%s@0x%08X/0x%08X%s", kind_name(o.kind), o.at, o.at2, o.in_known_code ? "" : "(unreached)");
            }
        }
        fputc('\n', out);
    }
}

}  // namespace gcrecomp
