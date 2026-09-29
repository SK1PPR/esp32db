#pragma once
// db.hpp — the public key/value API. This is the seam everything else uses.
//
// Keep it tiny and transport-agnostic: the local shell calls it now, and when
// you add consistent hashing, a cluster layer will sit *in front* of it
// (hash key -> pick node -> local db::put or forward over the network).
// Nothing in here should ever know about other nodes.
//
// Keys are fixed-width uint64 to start: fixed-size keys make B+ tree nodes
// fixed-size (-> mem::Pool, predictable fanout, trivial binary search).
// Map string keys onto this later (e.g. 64-bit hash + full key stored with
// the value to resolve collisions), which also lines up with consistent
// hashing wanting a numeric key on the ring.
//
// All calls are thread-safe (one internal mutex). Before open() succeeds
// (or after close()/format()) they return ESP_ERR_INVALID_STATE.

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace db {

using Key = uint64_t;

// A record (24 B header + value) must fit in one 4 KB log sector after its
// 12 B sector header. Checked against the real layout in kvlog.hpp.
constexpr size_t MAX_VALUE_LEN = 4096 - 12 - 24;

esp_err_t open();   // mount dbmeta + log, replay the log to rebuild the index,
                    // start the background compaction task
esp_err_t close();  // stop compaction; no further calls allowed

// ESP_ERR_NO_MEM: database full (log space or index nodes).
esp_err_t put(Key key, const void *value, size_t len);
// On success *out_len is the full value length. If cap was too small the value
// is truncated to cap bytes and ESP_ERR_INVALID_SIZE is returned.
// ESP_ERR_INVALID_CRC: the record on flash is damaged.
esp_err_t get(Key key, void *out, size_t cap, size_t *out_len);
esp_err_t del(Key key);                                  // ESP_ERR_NOT_FOUND if absent

// Compact one log sector right now (normally the background task does this).
esp_err_t compact();

// Recovery: erase the log and dbmeta partitions. Works even if open() failed.
// Leaves the database closed; reboot afterwards.
esp_err_t format();

struct Stats {
    uint32_t keys;
    uint32_t tree_height;
    uint32_t leaf_nodes;         // PSRAM
    uint32_t inner_sram_nodes;   // internal SRAM (hot)
    uint32_t inner_psram_nodes;  // spilled to PSRAM
    uint32_t log_sectors;
    uint32_t log_free_sectors;
};
Stats stats();

}  // namespace db
