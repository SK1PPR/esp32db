// log.cpp — see log.hpp for the on-flash layout and the rules it relies on.
//
// Boot cost: init() reads every free sector in full to prove it's really
// erased (~14 MB on an empty log, well under a second). If that ever matters,
// persist a "clean shutdown" flag in dbmeta and skip the check when it's set.

#include <cstddef>
#include <cinttypes>
#include "flash/log.hpp"
#include "esp_check.h"
#include "esp_rom_crc.h"
#include "mem/mem.hpp"

namespace flash {

static const char *TAG = "flash_log";
constexpr uint32_t SECTOR_MAGIC = 0x4C474553;  // "SEGL"

static uint32_t header_crc(const SectorHeader &h)
{
    return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&h), offsetof(SectorHeader, crc));
}

static bool is_erased(const uint32_t *words, size_t len)
{
    for (size_t i = 0; i < len / sizeof(uint32_t); ++i) {
        if (words[i] != UINT32_MAX) {
            return false;
        }
    }
    return true;
}

AppendLog::~AppendLog()
{
    mem::free(seq_);
}

esp_err_t AppendLog::init(const char *label)
{
    ESP_RETURN_ON_ERROR(part_.open(label), TAG, "partition '%s' not found", label);

    const uint32_t n = part_.sector_count();
    seq_ = static_cast<uint32_t *>(mem::alloc(mem::Region::Psram, n * sizeof(uint32_t)));
    auto *buf = static_cast<uint32_t *>(mem::alloc(mem::Region::Psram, SECTOR_SIZE));
    if (!seq_ || !buf) {
        mem::free(buf);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    uint32_t newest_seq = 0;
    for (uint32_t s = 0; s < n && err == ESP_OK; ++s) {
        SectorHeader h;
        err = part_.read(s * SECTOR_SIZE, &h, sizeof(h));
        if (err != ESP_OK) {
            break;
        }
        if (h.magic == SECTOR_MAGIC && h.seq != FREE_SEQ && h.crc == header_crc(h)) {
            seq_[s] = h.seq;
            if (h.seq > newest_seq) {
                newest_seq = h.seq;
                last_opened_ = s;
            }
            continue;
        }
        // Not a used sector. A blank header alone isn't proof: an interrupted
        // erase can leave 0xFF at the start and junk further in.
        err = part_.read(s * SECTOR_SIZE, buf, SECTOR_SIZE);
        if (err == ESP_OK && !is_erased(buf, SECTOR_SIZE)) {
            ESP_LOGW(TAG, "%s: sector %" PRIu32 " not blank, erasing", label, s);
            err = part_.erase_sector(s);
        }
        seq_[s] = FREE_SEQ;
        ++free_count_;
    }
    mem::free(buf);

    next_seq_ = newest_seq + 1;
    ESP_LOGI(TAG, "%s: %" PRIu32 "/%" PRIu32 " sectors free", label, free_count_, n);
    return err;
}

esp_err_t AppendLog::append(const void *data, size_t len, uint32_t *out_offset, uint32_t reserve)
{
    if (len == 0 || len > MAX_RECORD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (cur_sector_ == NO_SECTOR || cur_off_ + len > SECTOR_SIZE) {
        if (free_count_ <= reserve) {
            return ESP_ERR_NO_MEM;
        }
        ESP_RETURN_ON_ERROR(open_next_sector(), TAG, "open sector");
    }

    const uint32_t offset = cur_sector_ * SECTOR_SIZE + cur_off_;
    esp_err_t err = part_.write(offset, data, len);
    if (err != ESP_OK) {
        cur_off_ = SECTOR_SIZE;  // bytes here are now unknown: never write after them
        return err;
    }
    *out_offset = offset;
    cur_off_ += len;
    return ESP_OK;
}

esp_err_t AppendLog::read(uint32_t offset, void *dst, size_t len) const
{
    return part_.read(offset, dst, len);
}

esp_err_t AppendLog::free_sector(uint32_t sector)
{
    if (sector >= part_.sector_count() || sector == cur_sector_) {
        return ESP_ERR_INVALID_ARG;
    }
    if (seq_[sector] == FREE_SEQ) {
        return ESP_OK;
    }
    const SectorHeader dead{};  // all zeros: programming 1 -> 0 is always allowed
    ESP_RETURN_ON_ERROR(part_.write(sector * SECTOR_SIZE, &dead, sizeof(dead)), TAG,
                        "invalidate sector %" PRIu32, sector);
    ESP_RETURN_ON_ERROR(part_.erase_sector(sector), TAG, "erase sector %" PRIu32, sector);
    seq_[sector] = FREE_SEQ;
    ++free_count_;
    return ESP_OK;
}

uint32_t AppendLog::oldest_sector() const
{
    uint32_t best = NO_SECTOR;
    for (uint32_t s = 0; s < part_.sector_count(); ++s) {
        if (seq_[s] != FREE_SEQ && s != cur_sector_ && (best == NO_SECTOR || seq_[s] < seq_[best])) {
            best = s;
        }
    }
    return best;
}

uint32_t AppendLog::newest_sector() const
{
    uint32_t best = NO_SECTOR;
    for (uint32_t s = 0; s < part_.sector_count(); ++s) {
        if (seq_[s] != FREE_SEQ && (best == NO_SECTOR || seq_[s] > seq_[best])) {
            best = s;
        }
    }
    return best;
}

esp_err_t AppendLog::open_next_sector()
{
    const uint32_t n = part_.sector_count();
    const uint32_t start = (last_opened_ == NO_SECTOR) ? 0 : last_opened_ + 1;

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t s = (start + i) % n;
        if (seq_[s] != FREE_SEQ) {
            continue;
        }
        SectorHeader h{SECTOR_MAGIC, next_seq_, 0};
        h.crc = header_crc(h);

        // Mark used even if the write fails: the sector is no longer known to
        // be blank, so it must go through free_sector() (an erase) before reuse.
        seq_[s] = next_seq_++;
        --free_count_;
        last_opened_ = s;
        cur_sector_ = s;
        cur_off_ = SECTOR_SIZE;

        ESP_RETURN_ON_ERROR(part_.write(s * SECTOR_SIZE, &h, sizeof(h)), TAG, "write header");
        cur_off_ = SECTOR_DATA_START;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

}  // namespace flash
