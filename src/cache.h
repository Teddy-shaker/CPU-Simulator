// Direct-mapped write-back data cache in front of main memory (this grew out
// of the course "cache" project): 16 slots x 16-byte blocks over 2048 bytes.
// Address layout: | tag (3 bits) | slot (4 bits) | offset (4 bits) |
#pragma once
#include <cstdint>

constexpr int MEM_SIZE = 2048;
constexpr int CACHE_SLOTS = 16;
constexpr int BLOCK_SIZE = 16;
constexpr uint32_t ADDR_MASK = MEM_SIZE - 1;

struct CacheLine {
    bool valid = false;
    bool dirty = false;
    uint32_t tag = 0;
    uint8_t data[BLOCK_SIZE] = {};
};

struct Cache {
    CacheLine line[CACHE_SLOTS];

    static uint32_t offset_of(uint32_t addr) { return addr & 0xF; }
    static uint32_t slot_of(uint32_t addr) { return (addr >> 4) & 0xF; }
    static uint32_t tag_of(uint32_t addr) { return (addr & ADDR_MASK) >> 8; }
    uint32_t block_base(int slot) const { return (line[slot].tag << 8) | (static_cast<uint32_t>(slot) << 4); }

    // True if `addr` is currently cached. Does not change any state.
    bool probe(uint32_t addr) const;

    // Read/write one byte through the cache. On a miss the old block is written
    // back to `mem` if dirty and the new block is loaded. Returns true on a hit.
    bool read(uint32_t addr, uint8_t* mem, uint8_t& out, bool* wrote_back = nullptr);
    bool write(uint32_t addr, uint8_t value, uint8_t* mem, bool* wrote_back = nullptr);

private:
    bool ensure(uint32_t addr, uint8_t* mem, bool* wrote_back);
};
