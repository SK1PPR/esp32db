// engine_sqlite.cpp — kv:: on SQLite, file on FATFS over wear levelling (the
// usual way to run SQLite on an ESP32).
//
// Settings, chosen to be comparable with esp32db rather than fastest:
//   table:  kv(k INTEGER PRIMARY KEY, v BLOB): the key *is* the rowid, so
//           there's a single B-tree and no secondary index.
//   WAL + locking_mode=EXCLUSIVE (no shared memory on this platform).
//   synchronous=FULL: a commit is fsync'd before it returns, so outside a
//           batch every put/del is as durable as esp32db's.
// Batching (begin_batch/end_batch) is SQLite's best case; bench.py runs both.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include "kv/kv.hpp"
#include "engine.hpp"
#include "sqlite3.h"
#include "sqlite_esp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace kv {

static const char *TAG = "kv_sqlite";
static constexpr const char *PART = "sqlite";
static constexpr const char *MOUNT = "/sqlite";
static constexpr const char *DB_PATH = "/sqlite/kv.db";

static sqlite3 *s_db;
static sqlite3_stmt *s_put, *s_get, *s_del;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_lock;
static bool s_open = false;
static bool s_in_batch = false;

namespace {
struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};
}  // namespace

static esp_err_t check(int rc, const char *what)
{
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s: %s (%d)", what, s_db ? sqlite3_errmsg(s_db) : sqlite3_errstr(rc), rc);
    return rc == SQLITE_FULL ? ESP_ERR_NO_MEM : ESP_FAIL;
}

static esp_err_t exec(const char *sql)
{
    return check(sqlite3_exec(s_db, sql, nullptr, nullptr, nullptr), sql);
}

const char *engine_name() { return "sqlite"; }
const char *partition_name() { return PART; }
size_t max_value_len() { return 4096; }

esp_err_t open()
{
    if (s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex())) {
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;  // first boot / after `format yes`
    cfg.max_files = 4;                  // db, wal, journal, spare
    cfg.allocation_unit_size = 4096;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, PART, &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount %s: %s", PART, esp_err_to_name(err));
        return err;
    }

    sqlite_esp_set_temp_dir(MOUNT);
    if (check(sqlite3_initialize(), "sqlite3_initialize") != ESP_OK ||
        check(sqlite3_open_v2(DB_PATH, &s_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr),
              "open") != ESP_OK) {
        return ESP_FAIL;
    }
    // locking_mode must be set before WAL is first used (no shared memory).
    ESP_RETURN_ON_ERROR(exec("PRAGMA locking_mode=EXCLUSIVE"), TAG, "pragma");
    ESP_RETURN_ON_ERROR(exec("PRAGMA journal_mode=WAL"), TAG, "pragma");
    ESP_RETURN_ON_ERROR(exec("PRAGMA synchronous=FULL"), TAG, "pragma");
    ESP_RETURN_ON_ERROR(exec("CREATE TABLE IF NOT EXISTS kv(k INTEGER PRIMARY KEY, v BLOB NOT NULL)"), TAG, "table");

    if (check(sqlite3_prepare_v3(s_db, "INSERT OR REPLACE INTO kv(k, v) VALUES(?1, ?2)", -1,
                                 SQLITE_PREPARE_PERSISTENT, &s_put, nullptr), "prepare put") != ESP_OK ||
        check(sqlite3_prepare_v3(s_db, "SELECT v FROM kv WHERE k = ?1", -1, SQLITE_PREPARE_PERSISTENT, &s_get,
                                 nullptr), "prepare get") != ESP_OK ||
        check(sqlite3_prepare_v3(s_db, "DELETE FROM kv WHERE k = ?1", -1, SQLITE_PREPARE_PERSISTENT, &s_del,
                                 nullptr), "prepare del") != ESP_OK) {
        return ESP_FAIL;
    }
    s_open = true;
    return ESP_OK;
}

bool is_open() { return s_open; }

// SQLite integers are signed; a Key is stored bit-for-bit as int64.
static sqlite3_int64 to_sql(Key key) { return static_cast<sqlite3_int64>(key); }

esp_err_t engine_put(Key key, const void *value, size_t len)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    sqlite3_bind_int64(s_put, 1, to_sql(key));
    sqlite3_bind_blob(s_put, 2, value, static_cast<int>(len), SQLITE_STATIC);
    esp_err_t err = check(sqlite3_step(s_put), "put");
    sqlite3_reset(s_put);
    return err;
}

esp_err_t engine_get(Key key, void *out, size_t cap, size_t *out_len)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    sqlite3_bind_int64(s_get, 1, to_sql(key));
    int rc = sqlite3_step(s_get);
    esp_err_t err;
    if (rc == SQLITE_ROW) {
        const size_t len = sqlite3_column_bytes(s_get, 0);
        memcpy(out, sqlite3_column_blob(s_get, 0), len < cap ? len : cap);
        *out_len = len;
        err = len > cap ? ESP_ERR_INVALID_SIZE : ESP_OK;
    } else if (rc == SQLITE_DONE) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        err = check(rc, "get");
    }
    sqlite3_reset(s_get);
    return err;
}

esp_err_t engine_del(Key key)
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    sqlite3_bind_int64(s_del, 1, to_sql(key));
    esp_err_t err = check(sqlite3_step(s_del), "del");
    sqlite3_reset(s_del);
    if (err == ESP_OK && sqlite3_changes(s_db) == 0) {
        err = ESP_ERR_NOT_FOUND;
    }
    return err;
}

esp_err_t begin_batch()
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    if (s_in_batch) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = exec("BEGIN");
    s_in_batch = err == ESP_OK;
    return err;
}

esp_err_t end_batch()
{
    if (!s_open) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    if (!s_in_batch) {
        return ESP_ERR_INVALID_STATE;
    }
    s_in_batch = false;
    return exec("COMMIT");
}

esp_err_t wipe()
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);  // held for good: we reboot next
    }
    if (s_open) {
        s_open = false;
        sqlite3_finalize(s_put);
        sqlite3_finalize(s_get);
        sqlite3_finalize(s_del);
        sqlite3_close(s_db);
        s_db = nullptr;
    }
    if (s_wl != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(MOUNT, s_wl);
        s_wl = WL_INVALID_HANDLE;
    }
    // Erase everything, wear-levelling metadata included, so every run starts
    // from the same state; the next mount reformats.
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, PART);
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "erasing %s (%u KB)...", part->label, static_cast<unsigned>(part->size / 1024));
    return esp_partition_erase_range(part, 0, part->size);
}

int64_t key_count() { return -1; }  // count(*) is a full scan

void print_stats()
{
    if (!s_open) {
        printf("database not open\n");
        return;
    }
    Lock lock;
    sqlite3_stmt *st = nullptr;
    int64_t rows = -1;
    if (sqlite3_prepare_v2(s_db, "SELECT count(*) FROM kv", -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        rows = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);

    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(MOUNT, &total, &free_bytes);
    printf("engine:      SQLite %s, WAL, synchronous=FULL, on FATFS '%s'\n", sqlite3_libversion(), PART);
    printf("keys:        %" PRId64 "\n", rows);
    printf("filesystem:  %" PRIu64 " KB free / %" PRIu64 " KB\n", free_bytes / 1024, total / 1024);
}

}  // namespace kv
