// engine_flashdb.cpp — kv:: on FlashDB's KVDB (raw flash via FAL, see
// components/flashdb). FlashDB keys are strings, so a Key becomes 16 hex
// digits. FlashDB has its own lock hooks; we hand it a FreeRTOS mutex.

#include <cinttypes>
#include <cstdio>
#include "kv/kv.hpp"
#include "engine.hpp"
#include "flashdb.h"
#include "fal.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace kv {

static const char *TAG = "kv_flashdb";
static fdb_kvdb s_kvdb;
static SemaphoreHandle_t s_lock;
static bool s_open = false;

namespace {
struct KeyName {
    char s[17];
    explicit KeyName(Key key) { snprintf(s, sizeof(s), "%016" PRIx64, key); }
};
}  // namespace

static void lock_cb(fdb_db_t) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock_cb(fdb_db_t) { xSemaphoreGive(s_lock); }

static esp_err_t to_esp(fdb_err_t err)
{
    switch (err) {
    case FDB_NO_ERR: return ESP_OK;
    case FDB_KV_NAME_ERR: return ESP_ERR_NOT_FOUND;
    case FDB_SAVED_FULL: return ESP_ERR_NO_MEM;
    default: return ESP_FAIL;
    }
}

const char *engine_name() { return "flashdb"; }
const char *partition_name() { return FDB_ESP_PARTITION_LABEL; }
// A KV must fit in one 4 KB sector with its header and name.
size_t max_value_len() { return 3900; }

esp_err_t open()
{
    if (s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex())) {
        return ESP_ERR_NO_MEM;
    }
    fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_LOCK, reinterpret_cast<void *>(lock_cb));
    fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_UNLOCK, reinterpret_cast<void *>(unlock_cb));
    fdb_err_t err = fdb_kvdb_init(&s_kvdb, "kv", FDB_FAL_PART_NAME, nullptr, nullptr);
    if (err != FDB_NO_ERR) {
        ESP_LOGE(TAG, "fdb_kvdb_init: %d", static_cast<int>(err));
        return ESP_FAIL;
    }
    s_open = true;
    return ESP_OK;
}

bool is_open() { return s_open; }

esp_err_t engine_put(Key key, const void *value, size_t len)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    fdb_blob blob;
    return to_esp(fdb_kv_set_blob(&s_kvdb, KeyName(key).s, fdb_blob_make(&blob, value, len)));
}

esp_err_t engine_get(Key key, void *out, size_t cap, size_t *out_len)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    fdb_blob blob;
    fdb_blob_make(&blob, out, cap);
    blob.saved.len = 0;
    fdb_kv_get_blob(&s_kvdb, KeyName(key).s, &blob);
    // FlashDB reports "not found" as a zero saved length; we never store
    // empty values, so that's unambiguous here.
    if (blob.saved.len == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_len = blob.saved.len;
    return blob.saved.len > cap ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t engine_del(Key key)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    return to_esp(fdb_kv_del(&s_kvdb, KeyName(key).s));
}

esp_err_t begin_batch() { return ESP_OK; }
esp_err_t end_batch() { return ESP_OK; }

esp_err_t wipe()
{
    if (s_open) {
        xSemaphoreTake(s_lock, portMAX_DELAY);  // held for good: we reboot next
        s_open = false;
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
                                                           FDB_ESP_PARTITION_LABEL);
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "erasing %s (%u KB)...", part->label, static_cast<unsigned>(part->size / 1024));
    return esp_partition_erase_range(part, 0, part->size);
}

int64_t key_count() { return -1; }

void print_stats()
{
    printf("engine:      FlashDB KVDB (FAL partition '%s', %u KB)\n", FDB_FAL_PART_NAME,
           static_cast<unsigned>(FDB_FAL_LEN / 1024));
    printf("sector size: %" PRIu32 " B\n", s_kvdb.parent.sec_size);
    printf("caches:      %d KV entries, %d sectors (FlashDB defaults)\n", FDB_KV_CACHE_TABLE_SIZE,
           FDB_SECTOR_CACHE_TABLE_SIZE);
    printf("keys:        not tracked by FlashDB\n");
}

}  // namespace kv
