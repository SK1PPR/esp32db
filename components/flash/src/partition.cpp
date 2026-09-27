// partition.cpp — straight pass-through to esp_partition_*.
// Using esp_partition_find_first by label so offsets can change in the CSV
// without touching code.

#include "flash/partition.hpp"

namespace flash {

esp_err_t Partition::open(const char *label)
{
    part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    return part_ ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t Partition::read(uint32_t offset, void *dst, size_t len) const
{
    return esp_partition_read(part_, offset, dst, len);
}

esp_err_t Partition::write(uint32_t offset, const void *src, size_t len)
{
    return esp_partition_write(part_, offset, src, len);
}

esp_err_t Partition::erase_sector(uint32_t sector)
{
    return esp_partition_erase_range(part_, sector * SECTOR_SIZE, SECTOR_SIZE);
}

esp_err_t Partition::erase_all()
{
    return esp_partition_erase_range(part_, 0, part_->size);
}

size_t Partition::size() const
{
    return part_ ? part_->size : 0;
}

}  // namespace flash
