// mem.cpp — thin mapping from Region to heap_caps flags.
// If you later want per-region budgets or stats, this is where they go.

#include "mem/mem.hpp"
#include "esp_heap_caps.h"

namespace mem {

static uint32_t caps(Region region)
{
    return region == Region::Internal ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                      : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *alloc(Region region, size_t size, size_t align)
{
    return heap_caps_aligned_alloc(align, size, caps(region));
}

void free(void *ptr)
{
    heap_caps_free(ptr);
}

size_t free_bytes(Region region)
{
    return heap_caps_get_free_size(caps(region));
}

size_t largest_free_block(Region region)
{
    return heap_caps_get_largest_free_block(caps(region));
}

}  // namespace mem
