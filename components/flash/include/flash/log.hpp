#pragma once
// log.hpp — append-only log over a whole partition, allocated a sector at a time.
//
// Division of labour with its user (db/kvlog, db/meta):
//   AppendLog: owns the partition, writes records into free sectors, erases
//              sectors when told to, refuses to write when no sector is free.
//              Knows nothing about which records are live.
//   user:      knows liveness, runs compaction, calls free_sector() on sectors
//              that hold no live data any more.
// Only this class writes or erases, so the "never write into a used sector"
// check lives right next to the destructive operations.
//
// Self-describing on flash: every sector starts with a SectorHeader holding a
// sequence number that increases each time a sector is opened. On boot, init()
// reads the headers: blank = free, otherwise seq gives write order. No
// head/tail has to be persisted anywhere, so nothing is updated in place.
//
// After a reboot the log never continues the partly written sector (its last
// record may be torn); it opens a fresh one. Likewise after a failed write the
// current sector is abandoned. So inside a sector, records are always a clean
// prefix: a reader can stop at the first record that doesn't check out.
//
// Sectors aren't consumed strictly in a circle: the next sector is the next
// *free* one after the last opened, so the user can free any sector (e.g. the
// one with the least live data, greedy GC) without the log caring.
//
// Not thread-safe; the owner serialises access (db.cpp holds one mutex).

#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "flash/partition.hpp"

namespace flash {

struct SectorHeader {
    uint32_t magic;
    uint32_t seq;  // 1, 2, 3... per opened sector; uint32 outlasts the flash's erase endurance
    uint32_t crc;  // over magic + seq
};

// Records go after the header and never span sectors.
constexpr size_t SECTOR_DATA_START = sizeof(SectorHeader);
constexpr size_t MAX_RECORD = SECTOR_SIZE - SECTOR_DATA_START;

constexpr uint32_t FREE_SEQ = 0;           // seq table value for an erased sector
constexpr uint32_t NO_SECTOR = UINT32_MAX;

class AppendLog {
public:
    AppendLog() = default;
    ~AppendLog();
    AppendLog(const AppendLog &) = delete;
    AppendLog &operator=(const AppendLog &) = delete;

    // Opens the partition by label and scans it to rebuild the seq table.
    // Sectors that are neither validly used nor fully erased (interrupted
    // erase, old junk) are erased so they're safe to write later.
    esp_err_t init(const char *label);

    // Appends len bytes (<= MAX_RECORD) and returns the partition offset they
    // start at. Opens the next free sector when the current one can't fit len,
    // but only while more than `reserve` sectors are free: normal writes pass
    // a reserve so compaction always has somewhere to copy to.
    // ESP_ERR_NO_MEM -> no sector available, caller must compact.
    esp_err_t append(const void *data, size_t len, uint32_t *out_offset, uint32_t reserve = 0);
    esp_err_t read(uint32_t offset, void *dst, size_t len) const;

    // Caller guarantees the sector holds no live data. Erases it and marks it
    // free. Refuses the sector currently being written.
    // The header is zeroed *before* the erase: an erase cut short by power
    // loss can leave any mix of old bytes behind (e.g. a put whose tombstone
    // got wiped), but with the header already invalid the whole sector is
    // ignored on boot and simply erased again.
    esp_err_t free_sector(uint32_t sector);

    uint32_t sector_count() const { return part_.sector_count(); }
    uint32_t sector_seq(uint32_t sector) const { return seq_[sector]; }  // FREE_SEQ if free
    uint32_t free_sectors() const { return free_count_; }
    uint32_t current_sector() const { return cur_sector_; }
    uint32_t oldest_sector() const;  // lowest seq, excluding current; NO_SECTOR if none
    uint32_t newest_sector() const;  // highest seq; NO_SECTOR if none
    uint32_t sectors_opened() const { return next_seq_; }  // monotonic write-progress counter

    static uint32_t sector_of(uint32_t offset) { return offset / SECTOR_SIZE; }

private:
    esp_err_t open_next_sector();  // pick a free sector, write its SectorHeader

    Partition part_;
    uint32_t *seq_ = nullptr;           // one entry per sector, in PSRAM (~14 KB for "log")
    uint32_t free_count_ = 0;
    uint32_t next_seq_ = 1;
    uint32_t cur_sector_ = NO_SECTOR;   // sector being appended to, none until first append
    uint32_t cur_off_ = 0;              // write position inside cur_sector_
    uint32_t last_opened_ = NO_SECTOR;  // free-sector search starts after this (spreads wear)
};

}  // namespace flash
