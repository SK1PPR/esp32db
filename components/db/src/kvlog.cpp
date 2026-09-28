// kvlog.cpp — record encoding, replay and compaction over flash::AppendLog.
// Sectors are always read whole into sector_buf_: one 4 KB flash read is much
// cheaper than dozens of small ones, and CRCs can be checked in RAM.

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include "kvlog.hpp"
#include "esp_check.h"
#include "esp_rom_crc.h"
#include "mem/mem.hpp"

namespace db {

static const char *TAG = "kvlog";
constexpr uint32_t RECORD_MAGIC = 0x4352564B;  // "KVRC"

static uint32_t record_crc(const RecordHeader &h, const void *value)
{
    RecordHeader tmp = h;
    tmp.crc = 0;
    uint32_t crc = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&tmp), sizeof(tmp));
    return esp_rom_crc32_le(crc, static_cast<const uint8_t *>(value), h.value_len);
}

// Calls fn(offset, header, record_bytes) for each valid record of the sector
// held in buf, in write order; stops at the first blank or corrupt one (see
// log.hpp: valid records always form a prefix). fn returns false to stop early.
template <typename Fn>
static void for_each_record(const uint8_t *buf, uint32_t sector, Fn fn)
{
    size_t pos = flash::SECTOR_DATA_START;
    while (pos + sizeof(RecordHeader) <= flash::SECTOR_SIZE) {
        RecordHeader h;
        memcpy(&h, buf + pos, sizeof(h));
        if (h.magic != RECORD_MAGIC || h.value_len > flash::SECTOR_SIZE - pos - sizeof(h)) {
            return;
        }
        if (record_crc(h, buf + pos + sizeof(h)) != h.crc) {
            return;
        }
        if (!fn(sector * flash::SECTOR_SIZE + pos, h, buf + pos)) {
            return;
        }
        pos += sizeof(h) + h.value_len;
    }
}

KvLog::~KvLog()
{
    mem::free(rec_buf_);
    mem::free(sector_buf_);
}

esp_err_t KvLog::init()
{
    ESP_RETURN_ON_ERROR(log_.init(flash::PART_LOG), TAG, "log init");
    rec_buf_ = static_cast<uint8_t *>(mem::alloc(mem::Region::Internal, flash::MAX_RECORD));
    sector_buf_ = static_cast<uint8_t *>(mem::alloc(mem::Region::Psram, flash::SECTOR_SIZE));
    return (rec_buf_ && sector_buf_) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t KvLog::append_record(LogOp op, Key key, const void *value, size_t len, uint32_t *out_offset)
{
    if (len > MAX_VALUE) {
        return ESP_ERR_INVALID_SIZE;
    }
    RecordHeader h{};
    h.magic = RECORD_MAGIC;
    h.op = op;
    h.key = key;
    h.value_len = len;
    h.crc = record_crc(h, value);

    memcpy(rec_buf_, &h, sizeof(h));
    if (len) {
        memcpy(rec_buf_ + sizeof(h), value, len);
    }
    return log_.append(rec_buf_, sizeof(h) + len, out_offset, COMPACTION_RESERVE);
}

esp_err_t KvLog::append_put(Key key, const void *value, size_t len, ValueRef *out)
{
    uint32_t offset;
    esp_err_t err = append_record(LogOp::Put, key, value, len, &offset);
    if (err == ESP_OK) {
        *out = ValueRef{offset};
    }
    return err;
}

esp_err_t KvLog::append_del(Key key)
{
    uint32_t offset;
    return append_record(LogOp::Del, key, nullptr, 0, &offset);
}

esp_err_t KvLog::read_value(Key key, const ValueRef &ref, void *out, size_t cap, size_t *out_len)
{
    RecordHeader h;
    ESP_RETURN_ON_ERROR(log_.read(ref.offset, &h, sizeof(h)), TAG, "read header");
    if (h.magic != RECORD_MAGIC || h.op != LogOp::Put || h.key != key || h.value_len > MAX_VALUE) {
        ESP_LOGE(TAG, "bad record header at 0x%" PRIx32, ref.offset);
        return ESP_ERR_INVALID_CRC;
    }
    // The whole value is needed for the CRC, so read it into rec_buf_ first.
    if (h.value_len) {
        ESP_RETURN_ON_ERROR(log_.read(ref.offset + sizeof(h), rec_buf_, h.value_len), TAG, "read value");
    }
    if (record_crc(h, rec_buf_) != h.crc) {
        ESP_LOGE(TAG, "CRC mismatch at 0x%" PRIx32, ref.offset);
        return ESP_ERR_INVALID_CRC;
    }
    if (out_len) {
        *out_len = h.value_len;
    }
    const size_t n = std::min<size_t>(cap, h.value_len);
    memcpy(out, rec_buf_, n);
    return n < h.value_len ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t KvLog::load_sector(uint32_t sector)
{
    return log_.read(sector * flash::SECTOR_SIZE, sector_buf_, flash::SECTOR_SIZE);
}

esp_err_t KvLog::replay(BTree &tree)
{
    const uint32_t n = log_.sector_count();
    auto *order = static_cast<uint32_t *>(mem::alloc(mem::Region::Psram, n * sizeof(uint32_t)));
    if (!order) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t used = 0;
    for (uint32_t s = 0; s < n; ++s) {
        if (log_.sector_seq(s) != flash::FREE_SEQ) {
            order[used++] = s;
        }
    }
    std::sort(order, order + used,
              [this](uint32_t a, uint32_t b) { return log_.sector_seq(a) < log_.sector_seq(b); });

    esp_err_t err = ESP_OK;
    uint32_t records = 0;
    for (uint32_t i = 0; i < used && err == ESP_OK; ++i) {
        err = load_sector(order[i]);
        if (err != ESP_OK) {
            break;
        }
        for_each_record(sector_buf_, order[i], [&](uint32_t offset, const RecordHeader &h, const uint8_t *) {
            if (h.op == LogOp::Put) {
                err = tree.insert(h.key, ValueRef{offset});
            } else if (h.op == LogOp::Del) {
                tree.erase(h.key);
            } else {
                // Unknown type (newer format?): don't guess, stop this sector.
                ESP_LOGW(TAG, "unknown record op %u at 0x%" PRIx32, static_cast<unsigned>(h.op), offset);
                return false;
            }
            ++records;
            return err == ESP_OK;
        });
    }
    mem::free(order);

    ESP_LOGI(TAG, "replayed %" PRIu32 " records from %" PRIu32 " sectors, %" PRIu32 " keys",
             records, used, tree.size());
    return err;
}

esp_err_t KvLog::compact(BTree &tree)
{
    const uint32_t victim = log_.oldest_sector();
    if (victim == flash::NO_SECTOR) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(load_sector(victim), TAG, "load sector %" PRIu32, victim);

    esp_err_t err = ESP_OK;
    for_each_record(sector_buf_, victim, [&](uint32_t offset, const RecordHeader &h, const uint8_t *rec) {
        if (h.op != LogOp::Put) {
            return true;  // tombstone in the oldest sector: always dead (see kvlog.hpp)
        }
        ValueRef cur;
        if (!tree.find(h.key, &cur) || cur.offset != offset) {
            return true;  // superseded or deleted
        }
        uint32_t new_offset;
        // reserve = 0: compaction is what the reserve is kept for.
        err = log_.append(rec, sizeof(RecordHeader) + h.value_len, &new_offset, 0);
        if (err == ESP_OK) {
            err = tree.insert(h.key, ValueRef{new_offset});
        }
        return err == ESP_OK;
    });
    // On error the victim is kept: records already moved point at their copy,
    // the rest still point into the victim, so the tree stays consistent.
    ESP_RETURN_ON_ERROR(err, TAG, "move live records out of sector %" PRIu32, victim);
    return log_.free_sector(victim);
}

}  // namespace db
