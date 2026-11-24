// SPDX-License-Identifier: MIT
#include "efb_gpu_copy.h"

namespace gcrecomp::gp {

namespace {
bool overlaps(const GpuCopy& c, uint32_t addr, uint32_t len) {
    return uint64_t(c.addr) < uint64_t(addr) + len && uint64_t(addr) < uint64_t(c.addr) + c.extent;
}
}  // namespace

int CopyTracker::begin(const GpuCopy& fresh) {
    int same = -1, unused = -1, oldest = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (!used_[i]) {
            if (unused < 0) unused = i;
            continue;
        }
        const GpuCopy& c = slots_[i];
        if (c.addr == fresh.addr && c.w == fresh.w && c.h == fresh.h && c.fmt == fresh.fmt) same = i;
        else if (overlaps(c, fresh.addr, fresh.extent)) slots_[i].valid = false;
        if (!c.pending && (oldest < 0 || c.seq < slots_[oldest].seq)) oldest = i;
    }
    const int slot = same >= 0 ? same : unused >= 0 ? unused : oldest;
    if (slot < 0) return -1;
    slots_[slot] = fresh;
    slots_[slot].valid = true;
    slots_[slot].pending = true;
    slots_[slot].seq = next_seq_++;
    used_[slot] = true;
    return slot;
}

int CopyTracker::find(uint32_t addr, uint32_t w, uint32_t h, uint8_t fmt) const {
    for (int i = 0; i < kSlots; ++i) {
        const GpuCopy& c = slots_[i];
        if (used_[i] && c.valid && c.addr == addr && c.w == w && c.h == h && c.fmt == fmt) return i;
    }
    return -1;
}

int CopyTracker::pending_overlapping(uint32_t addr, uint32_t len, int (&out)[kSlots]) const {
    int n = 0;
    for (int i = 0; i < kSlots; ++i)
        if (used_[i] && slots_[i].pending && overlaps(slots_[i], addr, len)) out[n++] = i;
    return n;
}

int CopyTracker::pending_count() const {
    int n = 0;
    for (int i = 0; i < kSlots; ++i) n += used_[i] && slots_[i].pending;
    return n;
}

void CopyTracker::mark_written(int slot, uint32_t clock) {
    slots_[slot].pending = false;
    slots_[slot].clock = clock;
}

void CopyTracker::invalidate_overlapping(uint32_t addr, uint32_t len) {
    for (int i = 0; i < kSlots; ++i)
        if (used_[i] && overlaps(slots_[i], addr, len)) slots_[i].valid = false;
}

}  // namespace gcrecomp::gp
