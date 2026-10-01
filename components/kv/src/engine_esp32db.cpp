// engine_esp32db.cpp — kv:: on top of this project's own database. A straight
// pass-through: db:: already has the kv:: error contract and its own lock.
// main.cpp must run mem::balloon() before open().

#include <cinttypes>
#include <cstdio>
#include "kv/kv.hpp"
#include "engine.hpp"
#include "db/db.hpp"
#include "flash/partition.hpp"

namespace kv {

const char *engine_name() { return "esp32db"; }
const char *partition_name() { return flash::PART_LOG; }
size_t max_value_len() { return db::MAX_VALUE_LEN; }

esp_err_t open() { return db::open(); }
bool is_open() { return db::stats().log_sectors != 0; }  // stats() is all-zero until open

esp_err_t engine_put(Key key, const void *value, size_t len) { return db::put(key, value, len); }
esp_err_t engine_get(Key key, void *out, size_t cap, size_t *out_len) { return db::get(key, out, cap, out_len); }
esp_err_t engine_del(Key key) { return db::del(key); }

esp_err_t begin_batch() { return ESP_OK; }
esp_err_t end_batch() { return ESP_OK; }

esp_err_t wipe() { return db::format(); }

int64_t key_count()
{
    return is_open() ? db::stats().keys : -1;
}

void print_stats()
{
    db::Stats s = db::stats();
    printf("keys:        %" PRIu32 "\n", s.keys);
    printf("tree height: %" PRIu32 "\n", s.tree_height);
    printf("tree nodes:  %" PRIu32 " leaves, %" PRIu32 " inner in SRAM, %" PRIu32 " inner in PSRAM\n",
           s.leaf_nodes, s.inner_sram_nodes, s.inner_psram_nodes);
    printf("node pools:  SRAM %" PRIu32 "/%" PRIu32 ", PSRAM %" PRIu32 "/%" PRIu32 " used\n",
           s.inner_sram_nodes, s.sram_node_capacity, s.leaf_nodes + s.inner_psram_nodes,
           s.psram_node_capacity);
    printf("log sectors: %" PRIu32 " free / %" PRIu32 " total\n", s.log_free_sectors, s.log_sectors);
}

}  // namespace kv
