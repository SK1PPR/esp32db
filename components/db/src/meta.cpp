// meta.cpp — checkpoints are fixed-size records in dbmeta's AppendLog.
// Every used sector is scanned (there are at most 16), not just the newest:
// if power died right after a new sector's header was written but before its
// checkpoint, the newest sector is empty and the previous one still holds
// the valid copy.

#include <cstddef>
#include <cinttypes>
#include "meta.hpp"
#include "esp_check.h"
#include "esp_rom_crc.h"

namespace db {

static const char *TAG = "meta";
constexpr uint32_t CHECKPOINT_MAGIC = 0x54504B43;  // "CKPT"

static uint32_t checkpoint_crc(const Checkpoint &cp)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&cp), offsetof(Checkpoint, crc));
}

esp_err_t Meta::init()
{
    ESP_RETURN_ON_ERROR(log_.init(flash::PART_META), TAG, "dbmeta init");

    uint32_t best_seq = flash::FREE_SEQ;
    for (uint32_t s = 0; s < log_.sector_count(); ++s) {
        const uint32_t seq = log_.sector_seq(s);
        if (seq == flash::FREE_SEQ || seq < best_seq) {
            continue;
        }
        const uint32_t end = (s + 1) * flash::SECTOR_SIZE;
        for (uint32_t off = s * flash::SECTOR_SIZE + flash::SECTOR_DATA_START;
             off + sizeof(Checkpoint) <= end; off += sizeof(Checkpoint)) {
            Checkpoint cp;
            ESP_RETURN_ON_ERROR(log_.read(off, &cp, sizeof(cp)), TAG, "read checkpoint");
            if (cp.magic != CHECKPOINT_MAGIC || cp.crc != checkpoint_crc(cp)) {
                break;
            }
            cp_ = cp;
            best_seq = seq;
        }
    }
    if (best_seq != flash::FREE_SEQ) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "no checkpoint found, formatting (version %" PRIu32 ")", FORMAT_VERSION);
    Checkpoint cp{};
    cp.format_version = FORMAT_VERSION;
    return write(cp);
}

esp_err_t Meta::write(const Checkpoint &cp)
{
    Checkpoint rec = cp;
    rec.magic = CHECKPOINT_MAGIC;
    rec.crc = checkpoint_crc(rec);

    uint32_t offset;
    ESP_RETURN_ON_ERROR(log_.append(&rec, sizeof(rec), &offset), TAG, "append checkpoint");
    cp_ = rec;

    // The new checkpoint is durable; only now drop the older sectors.
    for (uint32_t s = 0; s < log_.sector_count(); ++s) {
        if (log_.sector_seq(s) != flash::FREE_SEQ && s != log_.current_sector()) {
            ESP_RETURN_ON_ERROR(log_.free_sector(s), TAG, "free old checkpoint sector");
        }
    }
    return ESP_OK;
}

}  // namespace db
