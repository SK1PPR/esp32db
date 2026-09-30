#pragma once
// mem.hpp — region-aware allocation.
//
// Why a component for this: on the S3 you have two very different RAMs.
//   Internal SRAM (~300 KB usable): fast, single-cycle, DMA capable, scarce.
//   PSRAM (8 MB octal):             ~5-10x slower on a cache miss, plentiful.
// Everything else in the project should say *what kind* of memory it wants
// (Region) instead of calling heap_caps_malloc with raw MALLOC_CAP_* flags.
// That keeps the placement policy in one file and makes it easy to add
// accounting / limits (e.g. "hot index may use at most 128 KB SRAM") later.

#include <cstddef>
#include "esp_err.h"

namespace mem {

// Unit the balloon arenas are measured in: B+ tree nodes are exactly one
// block, so an arena carves into nodes with nothing left over. A multiple of
// the cache line (32/64 B) so no block straddles two lines.
constexpr size_t BLOCK_SIZE = 512;

enum class Region {
    Internal,  // hot data: upper B+ tree levels, log append buffer
    Psram,     // bulk data: B+ tree leaves, value read cache
};

void *alloc(Region region, size_t size, size_t align = alignof(max_align_t));
void free(void *ptr);

size_t free_bytes(Region region);
size_t largest_free_block(Region region);

// How much can be taken from `region` in one contiguous block while still
// leaving at least `reserve` bytes free for everyone else.
size_t spare_bytes(Region region, size_t reserve);

// ---- ballooning ----
// Call once from main.cpp, *after* every other subsystem (WiFi, BT, tasks...)
// has done its allocations: grabs all remaining memory in each region except
// `*_reserve`, as a whole number of BLOCK_SIZE blocks aligned to 64 B, and
// holds it as an arena for the database to build its node pools from.
// The reserves are for allocations that still happen at runtime (buffers,
// new tasks, WiFi packets). The arenas are never given back.
esp_err_t balloon(size_t sram_reserve, size_t psram_reserve);

struct Arena {
    void *base = nullptr;
    size_t size = 0;  // multiple of BLOCK_SIZE
};
Arena arena(Region region);  // empty until balloon() has run

}  // namespace mem
