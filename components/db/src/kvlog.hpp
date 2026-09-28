#pragma once
// kvlog.hpp — the WAL and the data store in one (log-structured, Bitcask-style).
//
// Every put/del is appended as one self-contained record:
//     [RecordHeader | value bytes...]
// and the B+ tree maps key -> offset of its latest record. Consequences:
//   - one flash write per mutation; nothing is updated in place
//   - crash safety = CRC: a torn record fails its check and is ignored
//   - an update just appends; the old record becomes garbage
//   - a del appends a tombstone (needed so replay knows the key is gone)
//
// KvLog owns liveness, so it owns compaction and decides which sectors get
// freed; flash::AppendLog only performs the erase.
//
// Replay (boot): take the used sectors, sort by SectorHeader seq (= write
// order), scan each sector's records until the first bad/blank header, apply
// Put -> tree.insert, Del -> tree.erase. Later records win.
//
// Compaction, one sector per call: victim = oldest used sector. For each
// record in it:
//   Put is live  <=> tree.find(key) points at exactly this offset
//                    -> re-append the record bytes verbatim, repoint the tree.
//   Del is dead  always. A tombstone only matters while an *older* sector
//                    might still hold a put for its key, and nothing is older
//                    than the oldest sector. (If victim selection ever becomes
//                    greedy/least-live, this rule has to change.)
// Then the victim is erased via log_.free_sector(). A crash at any point is
// safe: until the erase, the victim still holds everything, and replaying a
// record twice (original + copy) gives the same result.
//
// Not thread-safe; db.cpp serialises access.

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "flash/log.hpp"
#include "btree.hpp"

namespace db {

enum class LogOp : uint8_t { Put = 1, Del = 2 };

struct RecordHeader {
    uint32_t magic;
    LogOp op;
    uint8_t reserved[3];
    Key key;
    uint32_t value_len;
    uint32_t crc;  // over header (crc=0) + value; esp_rom_crc32_le
};
static_assert(sizeof(RecordHeader) == 24, "on-flash layout, no hidden padding");

constexpr size_t MAX_VALUE = flash::MAX_RECORD - sizeof(RecordHeader);
static_assert(MAX_VALUE == MAX_VALUE_LEN, "keep db.hpp in sync with the record layout");

// Free sectors only compaction may use; normal writes stop above this, so a
// full log can still be compacted (it needs somewhere to copy live data to).
constexpr uint32_t COMPACTION_RESERVE = 2;

class KvLog {
public:
    KvLog() = default;
    ~KvLog();
    KvLog(const KvLog &) = delete;
    KvLog &operator=(const KvLog &) = delete;

    esp_err_t init();

    // ESP_ERR_NO_MEM: only the compaction reserve is left, compact first.
    // (Returned silently: it's an expected condition that db.cpp handles.)
    esp_err_t append_put(Key key, const void *value, size_t len, ValueRef *out);
    esp_err_t append_del(Key key);
    // Reads the record at ref, checks it is an intact put for `key` (magic,
    // CRC), sets *out_len to the full value length and copies min(cap, len)
    // bytes. ESP_ERR_INVALID_SIZE if that truncated, ESP_ERR_INVALID_CRC if
    // the record is damaged.
    esp_err_t read_value(Key key, const ValueRef &ref, void *out, size_t cap, size_t *out_len);

    esp_err_t replay(BTree &tree);
    // Compacts the oldest sector. ESP_ERR_NOT_FOUND: nothing to compact.
    esp_err_t compact(BTree &tree);

    uint32_t free_sectors() const { return log_.free_sectors(); }
    uint32_t sector_count() const { return log_.sector_count(); }
    bool writable() const { return free_sectors() > COMPACTION_RESERVE; }
    uint32_t sectors_opened() const { return log_.sectors_opened(); }

    // Background compaction hysteresis: start below ~10% free, stop at ~12.5%.
    uint32_t low_watermark() const { return sector_count() / 10 + COMPACTION_RESERVE; }
    uint32_t high_watermark() const { return sector_count() / 8 + COMPACTION_RESERVE; }

private:
    esp_err_t append_record(LogOp op, Key key, const void *value, size_t len, uint32_t *out_offset);
    esp_err_t load_sector(uint32_t sector);  // into sector_buf_

    flash::AppendLog log_;
    uint8_t *rec_buf_ = nullptr;     // record being appended / value being read (internal SRAM)
    uint8_t *sector_buf_ = nullptr;  // whole sector for replay/compaction (PSRAM)
};

}  // namespace db
