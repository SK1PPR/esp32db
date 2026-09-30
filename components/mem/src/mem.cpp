// mem.cpp — thin mapping from Region to heap_caps flags.
// If you later want per-region budgets or stats, this is where they go.

#include "mem/mem.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace mem {

static const char *TAG = "mem";
static Arena s_arena[2];  // indexed by Region
static bool s_ballooned = false;

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

size_t spare_bytes(Region region, size_t reserve)
{
    const size_t free = free_bytes(region);
    if (free <= reserve) {
        return 0;
    }
    // Free memory can be split across several blocks; one allocation can
    // only ever get the largest of them.
    const size_t largest = largest_free_block(region);
    return largest < free - reserve ? largest : free - reserve;
}

// One arena per region. The heap needs a few bytes of its own around an
// aligned block, so if the exact size is refused, back off a block at a time.
static Arena grab(Region region, size_t reserve)
{
    size_t blocks = spare_bytes(region, reserve) / BLOCK_SIZE;
    for (int attempt = 0; attempt < 16 && blocks > 0; ++attempt, --blocks) {
        if (void *p = alloc(region, blocks * BLOCK_SIZE, 64)) {
            return Arena{p, blocks * BLOCK_SIZE};
        }
    }
    return Arena{};
}

esp_err_t balloon(size_t sram_reserve, size_t psram_reserve)
{
    if (s_ballooned) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ballooned = true;
    // PSRAM first: it's the big one, and nothing below needs internal RAM.
    s_arena[static_cast<int>(Region::Psram)] = grab(Region::Psram, psram_reserve);
    s_arena[static_cast<int>(Region::Internal)] = grab(Region::Internal, sram_reserve);

    const Arena &ps = s_arena[static_cast<int>(Region::Psram)];
    const Arena &in = s_arena[static_cast<int>(Region::Internal)];
    ESP_LOGI(TAG, "balloon: SRAM %u KB (%u blocks), PSRAM %u KB (%u blocks); left free: SRAM %u KB, PSRAM %u KB",
             static_cast<unsigned>(in.size / 1024), static_cast<unsigned>(in.size / BLOCK_SIZE),
             static_cast<unsigned>(ps.size / 1024), static_cast<unsigned>(ps.size / BLOCK_SIZE),
             static_cast<unsigned>(free_bytes(Region::Internal) / 1024),
             static_cast<unsigned>(free_bytes(Region::Psram) / 1024));
    return ps.size ? ESP_OK : ESP_ERR_NO_MEM;
}

Arena arena(Region region)
{
    return s_arena[static_cast<int>(region)];
}

}  // namespace mem
