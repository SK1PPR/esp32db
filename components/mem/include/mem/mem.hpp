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

namespace mem {

enum class Region {
    Internal,  // hot data: upper B+ tree levels, log append buffer
    Psram,     // bulk data: B+ tree leaves, value read cache
};

void *alloc(Region region, size_t size, size_t align = alignof(max_align_t));
void free(void *ptr);

size_t free_bytes(Region region);
size_t largest_free_block(Region region);

}  // namespace mem
