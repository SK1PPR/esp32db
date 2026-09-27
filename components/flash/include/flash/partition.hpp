#pragma once
// partition.hpp — handle to one *whole* data partition from partitions.csv
// (e.g. all 14 MB of "log"), not a sector. Sectors are just 4 KB ranges of
// offsets inside it: sector n starts at n * SECTOR_SIZE.
//
// Wraps esp_partition_* so the rest of the code works in partition-relative
// offsets and can't accidentally scribble over the app image. NOR flash
// rules to keep in mind for everything built on top:
//   - erase granularity is a 4 KB sector (erase sets bits to 1)
//   - writes can only flip 1 -> 0, so "overwrite" means erase first
//   - each sector survives ~100k erase cycles -> spread erases around

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "esp_partition.h"

namespace flash {

constexpr size_t SECTOR_SIZE = 4096;

// Must match the labels in partitions.csv.
constexpr const char *PART_META = "dbmeta";
constexpr const char *PART_LOG  = "log";

class Partition {
public:
    esp_err_t open(const char *label);

    esp_err_t read(uint32_t offset, void *dst, size_t len) const;
    esp_err_t write(uint32_t offset, const void *src, size_t len);
    // Erase is sector-only by construction: there is no byte-range erase, so a
    // partial-sector erase can't even be written. (esp_partition_erase_range
    // would reject unaligned ranges anyway with ESP_ERR_INVALID_ARG.)
    esp_err_t erase_sector(uint32_t sector);
    esp_err_t erase_all();  // whole partition; recovery only

    size_t size() const;
    uint32_t sector_count() const { return size() / SECTOR_SIZE; }
    const esp_partition_t *raw() const { return part_; }

private:
    const esp_partition_t *part_ = nullptr;
};

}  // namespace flash
