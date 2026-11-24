// SPDX-License-Identifier: MIT
// Bookkeeping of EFB copies kept on the GPU (no OpenGL, no memory map: unit-tested in tests/efb_gpu_copy_test.cpp).
//
// A copy to memory is also kept as a GL texture, and the texture is what draws sample while it is valid; the guest
// bytes are written later (asynchronously). This tracker decides which copy a texture use may take, and which
// copies must be written before guest memory is read or overwritten. The renderer owns the GL objects.
#pragma once

#include <cstdint>

namespace gcrecomp::gp {

struct GpuCopy {
    uint32_t addr = 0, extent = 0;  // guest bytes [addr, addr + extent) the copy writes
    uint32_t w = 0, h = 0;          // image size in texels
    uint8_t fmt = 0;                // texture format code a draw must use to sample it
    uint32_t clock = 0;             // Memory::write_clock epoch the content is valid for: a store at or after it makes the copy stale
    bool valid = false;             // the texture holds what guest memory holds (or will hold once written)
    bool pending = false;           // the guest bytes are not written yet
    uint64_t seq = 0;               // creation order
};

class CopyTracker {
public:
    static constexpr int kSlots = 16;

    // Slot for a new copy: the slot holding the same (addr, w, h, fmt), else an unused one, else the oldest that is not
    // pending (a pending one is never recycled: the caller flushes first); -1 if every slot is pending. Every OTHER valid
    // copy overlapping the range is invalidated (the new copy overwrites their bytes). The slot is returned reset to
    // `fresh` with valid = pending = true.
    int begin(const GpuCopy& fresh);

    // The valid copy a texture use at (addr, w, h, fmt) may sample, or -1.
    int find(uint32_t addr, uint32_t w, uint32_t h, uint8_t fmt) const;

    // Slots whose guest bytes are not written yet and overlap [addr, addr + len); fills `out`, returns the count.
    int pending_overlapping(uint32_t addr, uint32_t len, int (&out)[kSlots]) const;
    int pending_count() const;

    // The guest bytes of the copy are written; `clock` is the epoch after that write.
    void mark_written(int slot, uint32_t clock);
    // Guest memory under the copy was modified by something else: the texture no longer matches.
    void invalidate(int slot) { slots_[slot].valid = false; }
    void invalidate_overlapping(uint32_t addr, uint32_t len);

    GpuCopy& at(int slot) { return slots_[slot]; }
    const GpuCopy& at(int slot) const { return slots_[slot]; }

private:
    GpuCopy slots_[kSlots];
    uint64_t next_seq_ = 1;
    bool used_[kSlots] = {};
};

}  // namespace gcrecomp::gp
