// db.cpp — glues the log and the index together.
//   put: log.append_put(key, value) -> ref;  tree.insert(key, ref)
//   del: log.append_del(key);                tree.erase(key)
//   get: tree.find(key) -> ref;              log.read_value(key, ref)
// The log write always happens first, so anything visible in the tree is
// already durable. Reads never scan the log — one tree walk + one record read.
// A put for a new key checks the tree has room *before* writing to the log,
// so a record is never durable without being indexed.
//
// One instance of each by ownership, not by singleton pattern: they're
// file-private statics here and nothing outside db.cpp can reach them.
//
// Concurrency: a single mutex covers tree + log, so BTree, mem::Pool and
// flash::AppendLog need no locking of their own. get() holds it across the
// flash read too, otherwise compaction could erase the sector in between.
//
// Compaction runs in two places:
//   1. Background task (normal case). Writers notify it once free sectors
//      drop below the low watermark; it compacts one sector per lock hold
//      (~one 4 KB read, a few appends, one erase) so puts/gets interleave,
//      and stops at the high watermark. If a window of sectors yields no net
//      free space (the log is mostly live data) it stalls, and ignores
//      nudges until enough new data has been written to be worth retrying.
//   2. Inline, when a write finds only the compaction reserve left (the task
//      fell behind a write burst). It stops at the first step that frees no
//      net space; then the database is genuinely full: ESP_ERR_NO_MEM.

#include <atomic>
#include <cinttypes>
#include "db/db.hpp"
#include "btree.hpp"
#include "kvlog.hpp"
#include "meta.hpp"
#include "flash/partition.hpp"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace db {

static const char *TAG = "db";

constexpr TickType_t COMPACT_POLL = pdMS_TO_TICKS(5000);  // also check without a nudge
constexpr uint32_t COMPACT_PROGRESS_WINDOW = 16;          // sectors per "made progress?" check
constexpr int INLINE_COMPACT_MAX = 8;                     // sectors a writer compacts before giving up

static Meta s_meta;
static KvLog s_log;
static BTree s_tree;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_compactor;
static std::atomic<bool> s_open{false};

namespace {
struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};
}  // namespace

static void compactor_task(void *)
{
    bool stalled = false;
    uint32_t stalled_at = 0;  // s_log.sectors_opened() when we stalled

    for (;;) {
        ulTaskNotifyTake(pdTRUE, COMPACT_POLL);

        uint32_t window_free;
        {
            Lock lock;
            if (s_log.free_sectors() >= s_log.low_watermark()) {
                stalled = false;
                continue;
            }
            // After a stall, retrying before new data arrives would just copy
            // the same live sectors around again.
            if (stalled && s_log.sectors_opened() - stalled_at < COMPACT_PROGRESS_WINDOW) {
                continue;
            }
            stalled = false;
            window_free = s_log.free_sectors();
        }

        for (uint32_t step = 1;; ++step) {
            esp_err_t err;
            uint32_t free_now;
            {
                Lock lock;
                if (s_log.free_sectors() >= s_log.high_watermark()) {
                    break;
                }
                err = s_log.compact(s_tree);
                free_now = s_log.free_sectors();
                if (err == ESP_OK && step % COMPACT_PROGRESS_WINDOW == 0) {
                    if (free_now <= window_free) {
                        ESP_LOGW(TAG, "log is mostly live data, compaction paused");
                        stalled = true;
                        stalled_at = s_log.sectors_opened();
                        break;
                    }
                    window_free = free_now;
                }
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "background compaction stopped: %s", esp_err_to_name(err));
                break;
            }
        }
    }
}

// Call with the lock held.
static void nudge_compactor()
{
    if (s_compactor && s_log.free_sectors() < s_log.low_watermark()) {
        xTaskNotifyGive(s_compactor);
    }
}

// Call with the lock held. Runs append(); if the log is out of free sectors,
// compacts inline and retries once.
template <typename Append>
static esp_err_t append_with_space(Append append)
{
    esp_err_t err = append();
    if (err != ESP_ERR_NO_MEM) {
        return err;
    }
    for (int i = 0; i < INLINE_COMPACT_MAX && !s_log.writable(); ++i) {
        const uint32_t before = s_log.free_sectors();
        if (s_log.compact(s_tree) != ESP_OK || s_log.free_sectors() <= before) {
            break;  // no net space gained: the log is full of live data
        }
    }
    return s_log.writable() ? append() : ESP_ERR_NO_MEM;
}

esp_err_t open()
{
    if (s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex())) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(s_meta.init(), TAG, "dbmeta");
    if (s_meta.current().format_version != FORMAT_VERSION) {
        ESP_LOGE(TAG, "on-flash format v%" PRIu32 ", firmware expects v%" PRIu32,
                 s_meta.current().format_version, FORMAT_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    ESP_RETURN_ON_ERROR(s_log.init(), TAG, "log");
    ESP_RETURN_ON_ERROR(s_tree.init(), TAG, "tree");
    ESP_RETURN_ON_ERROR(s_log.replay(s_tree), TAG, "replay");

    if (xTaskCreate(compactor_task, "db_compact", 4096, nullptr, tskIDLE_PRIORITY + 1, &s_compactor) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    Lock lock;
    s_open = true;
    nudge_compactor();
    return ESP_OK;
}

esp_err_t close()
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;  // compactor is never mid-step while we hold this
    s_open = false;
    if (s_compactor) {
        vTaskDelete(s_compactor);
        s_compactor = nullptr;
    }
    return ESP_OK;
}

esp_err_t put(Key key, const void *value, size_t len)
{
    if (!value && len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    // Make sure the index can take the key before the record becomes durable
    // (updates of existing keys always can: they never allocate tree nodes).
    if (!s_tree.can_insert(key)) {
        return ESP_ERR_NO_MEM;
    }
    ValueRef ref;
    esp_err_t err = append_with_space([&] { return s_log.append_put(key, value, len, &ref); });
    if (err != ESP_OK) {
        return err;
    }
    err = s_tree.insert(key, ref);  // can't fail after the can_insert(key) check
    nudge_compactor();
    return err;
}

esp_err_t get(Key key, void *out, size_t cap, size_t *out_len)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    ValueRef ref;
    if (!s_tree.find(key, &ref)) {
        return ESP_ERR_NOT_FOUND;
    }
    return s_log.read_value(key, ref, out, cap, out_len);
}

esp_err_t del(Key key)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    if (!s_tree.find(key, nullptr)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = append_with_space([&] { return s_log.append_del(key); });
    if (err != ESP_OK) {
        return err;
    }
    s_tree.erase(key);
    nudge_compactor();
    return ESP_OK;
}

esp_err_t compact()
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    return s_log.compact(s_tree);
}

esp_err_t format()
{
    // Take the lock for good (we reboot after this) so nothing else touches
    // the partitions while they're being wiped.
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_open = false;
    if (s_compactor) {
        vTaskDelete(s_compactor);
        s_compactor = nullptr;
    }
    for (const char *label : {flash::PART_LOG, flash::PART_META}) {
        flash::Partition part;
        ESP_RETURN_ON_ERROR(part.open(label), TAG, "open %s", label);
        ESP_LOGI(TAG, "erasing %s (%u KB)...", label, static_cast<unsigned>(part.size() / 1024));
        ESP_RETURN_ON_ERROR(part.erase_all(), TAG, "erase %s", label);
    }
    return ESP_OK;
}

Stats stats()
{
    if (!s_open) {
        return Stats{};
    }
    Lock lock;
    Stats s{};
    s.keys = s_tree.size();
    s.tree_height = s_tree.height();
    s.leaf_nodes = s_tree.leaf_nodes();
    s.inner_sram_nodes = s_tree.inner_sram_nodes();
    s.inner_psram_nodes = s_tree.inner_psram_nodes();
    s.sram_node_capacity = s_tree.sram_node_capacity();
    s.psram_node_capacity = s_tree.psram_node_capacity();
    s.log_sectors = s_log.sector_count();
    s.log_free_sectors = s_log.free_sectors();
    return s;
}

}  // namespace db
