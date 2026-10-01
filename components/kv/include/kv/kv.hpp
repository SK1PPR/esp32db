#pragma once
// kv.hpp — the storage engine seam. Exactly one engine is compiled in
// (Kconfig KV_ENGINE_*): our own `db`, or FlashDB / SQLite for benchmarking.
// The shell and the bench runner only ever talk to this API, so every board
// runs identical commands and workloads whatever engine it was built with.
//
// Error contract (same for every engine, mirrors db.hpp):
//   ESP_ERR_NOT_FOUND     get/del of an absent key
//   ESP_ERR_NO_MEM        storage full
//   ESP_ERR_INVALID_SIZE  get: value truncated to cap (out_len is the full size)
//   ESP_ERR_INVALID_STATE called before open() succeeded
// All calls are thread-safe.

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace kv {

using Key = uint64_t;

const char *engine_name();     // "esp32db", "flashdb", "sqlite"
const char *partition_name();  // the data partition the engine owns
size_t max_value_len();

esp_err_t open();
bool is_open();

esp_err_t put(Key key, const void *value, size_t len);
esp_err_t get(Key key, void *out, size_t cap, size_t *out_len);
esp_err_t del(Key key);

// put/get/del calls since boot, whatever their result (wraps at 2^32).
// main.cpp watches these to flash an LED per operation.
struct OpCounts {
    uint32_t puts, gets, dels;
};
OpCounts op_counts();

// Group the writes between begin and end into one transaction. Only SQLite
// does anything with this; for the others every write is already durable on
// return and these are no-ops. A put inside a batch is NOT durable until
// end_batch() returns.
esp_err_t begin_batch();
esp_err_t end_batch();

// Erase all of the engine's storage. The database is unusable afterwards;
// the caller reboots.
esp_err_t wipe();

int64_t key_count();  // -1 when the engine can't count cheaply
void print_stats();   // human-readable, for the `dbstat` shell command

}  // namespace kv
