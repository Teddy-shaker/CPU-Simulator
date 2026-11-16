#include "cache.h"

#include <cstring>

bool Cache::probe(uint32_t addr) const {
    addr &= ADDR_MASK;
    const CacheLine& l = line[slot_of(addr)];
    return l.valid && l.tag == tag_of(addr);
}

bool Cache::ensure(uint32_t addr, uint8_t* mem, bool* wrote_back) {
    const uint32_t slot = slot_of(addr);
    CacheLine& l = line[slot];
    if (l.valid && l.tag == tag_of(addr)) return true;

    // Miss: write the old block back first if it was modified.
    if (l.valid && l.dirty) {
        std::memcpy(mem + block_base(static_cast<int>(slot)), l.data, BLOCK_SIZE);
        if (wrote_back) *wrote_back = true;
    }
    std::memcpy(l.data, mem + (addr & ~0xFu), BLOCK_SIZE);
    l.valid = true;
    l.dirty = false;
    l.tag = tag_of(addr);
    return false;
}

bool Cache::read(uint32_t addr, uint8_t* mem, uint8_t& out, bool* wrote_back) {
    addr &= ADDR_MASK;
    bool hit = ensure(addr, mem, wrote_back);
    out = line[slot_of(addr)].data[offset_of(addr)];
    return hit;
}

bool Cache::write(uint32_t addr, uint8_t value, uint8_t* mem, bool* wrote_back) {
    addr &= ADDR_MASK;
    bool hit = ensure(addr, mem, wrote_back);
    CacheLine& l = line[slot_of(addr)];
    l.data[offset_of(addr)] = value;
    l.dirty = true;
    return hit;
}
