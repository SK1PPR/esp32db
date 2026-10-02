#pragma once
/* sqlite_esp.h — knobs for the ESP-IDF SQLite OS layer (esp_vfs_sqlite.c). */
#ifdef __cplusplus
extern "C" {
#endif

/* Directory for the rare on-disk temp file (e.g. "/sqlite"). Call before
 * sqlite3_initialize(); the string must stay valid. */
void sqlite_esp_set_temp_dir(const char *dir);

#ifdef __cplusplus
}
#endif
