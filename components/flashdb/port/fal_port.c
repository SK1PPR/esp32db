/* fal_port.c — FAL flash device backed by the `fdb` esp_partition.
 * FlashDB serialises its own calls (kv.cpp installs a lock), so no locking
 * here. FAL treats a negative return as failure. */
#include <fal.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "fal_port";
static const esp_partition_t *s_part;

static int port_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, FDB_ESP_PARTITION_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "partition '%s' not found", FDB_ESP_PARTITION_LABEL);
        return -1;
    }
    if (s_part->size != FDB_FAL_LEN) {
        ESP_LOGE(TAG, "partition '%s' is 0x%" PRIx32 " bytes, fal_cfg.h expects 0x%x", FDB_ESP_PARTITION_LABEL,
                 s_part->size, FDB_FAL_LEN);
        s_part = NULL;
        return -1;
    }
    return 0;
}

static int port_read(long offset, uint8_t *buf, size_t size)
{
    if (!s_part || esp_partition_read(s_part, offset, buf, size) != ESP_OK) {
        return -1;
    }
    return (int)size;
}

static int port_write(long offset, const uint8_t *buf, size_t size)
{
    if (!s_part || esp_partition_write(s_part, offset, buf, size) != ESP_OK) {
        return -1;
    }
    return (int)size;
}

static int port_erase(long offset, size_t size)
{
    const size_t sector = s_part ? s_part->erase_size : 4096;
    const size_t len = (size + sector - 1) / sector * sector;
    if (!s_part || esp_partition_erase_range(s_part, offset, len) != ESP_OK) {
        return -1;
    }
    return (int)size;
}

const struct fal_flash_dev fdb_flash_dev = {
    .name = FDB_FAL_DEV_NAME,
    .addr = 0,
    .len = FDB_FAL_LEN,
    .blk_size = 4096,
    .ops = {port_init, port_read, port_write, port_erase},
    .write_gran = 1,
};
