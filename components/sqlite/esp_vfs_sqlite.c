/* esp_vfs_sqlite.c — the SQLite OS layer for ESP-IDF (SQLITE_OS_OTHER).
 *
 * Files go through newlib's POSIX calls, which ESP-IDF routes to whatever is
 * mounted at that path (FATFS on wear levelling, see engine_sqlite.cpp).
 *
 * Deliberately minimal:
 *   - One process, one connection: locks are no-ops (SQLITE_THREADSAFE=0, and
 *     kv.cpp serialises calls).
 *   - No shared memory (io_methods v1). WAL mode then requires
 *     locking_mode=EXCLUSIVE, which is the compile-time default here.
 *   - xSync is fsync(), so synchronous=FULL really reaches the flash.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sqlite3.h"

#define MAX_PATHNAME 96

typedef struct {
    sqlite3_file base;
    int fd;
    char *delete_on_close; /* path, or NULL */
} EspFile;

static int esp_close(sqlite3_file *f)
{
    EspFile *p = (EspFile *)f;
    int rc = close(p->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_CLOSE;
    p->fd = -1;
    if (p->delete_on_close) {
        unlink(p->delete_on_close);
        free(p->delete_on_close);
        p->delete_on_close = NULL;
    }
    return rc;
}

static int esp_read(sqlite3_file *f, void *buf, int amt, sqlite3_int64 off)
{
    EspFile *p = (EspFile *)f;
    int got = 0;
    while (got < amt) {
        ssize_t n = pread(p->fd, (char *)buf + got, amt - got, off + got);
        if (n < 0) {
            return SQLITE_IOERR_READ;
        }
        if (n == 0) {
            break;
        }
        got += n;
    }
    if (got < amt) {
        /* SQLite requires the rest of the buffer zeroed on a short read. */
        memset((char *)buf + got, 0, amt - got);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}

static int esp_write(sqlite3_file *f, const void *buf, int amt, sqlite3_int64 off)
{
    EspFile *p = (EspFile *)f;
    int done = 0;
    while (done < amt) {
        ssize_t n = pwrite(p->fd, (const char *)buf + done, amt - done, off + done);
        if (n <= 0) {
            return (n < 0 && errno == ENOSPC) ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        }
        done += n;
    }
    return SQLITE_OK;
}

static int esp_truncate(sqlite3_file *f, sqlite3_int64 size)
{
    return ftruncate(((EspFile *)f)->fd, size) == 0 ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int esp_sync(sqlite3_file *f, int flags)
{
    (void)flags;
    return fsync(((EspFile *)f)->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int esp_file_size(sqlite3_file *f, sqlite3_int64 *size)
{
    struct stat st;
    if (fstat(((EspFile *)f)->fd, &st) != 0) {
        return SQLITE_IOERR_FSTAT;
    }
    *size = st.st_size;
    return SQLITE_OK;
}

static int esp_lock(sqlite3_file *f, int level)
{
    (void)f; (void)level;
    return SQLITE_OK;
}

static int esp_check_reserved_lock(sqlite3_file *f, int *out)
{
    (void)f;
    *out = 0;
    return SQLITE_OK;
}

static int esp_file_control(sqlite3_file *f, int op, void *arg)
{
    (void)f; (void)op; (void)arg;
    return SQLITE_NOTFOUND;
}

static int esp_sector_size(sqlite3_file *f)
{
    (void)f;
    return 4096; /* wear-levelling sector size (CONFIG_WL_SECTOR_SIZE) */
}

static int esp_device_characteristics(sqlite3_file *f)
{
    (void)f;
    return 0;
}

static const sqlite3_io_methods s_io = {
    .iVersion = 1, /* no shared-memory methods */
    .xClose = esp_close,
    .xRead = esp_read,
    .xWrite = esp_write,
    .xTruncate = esp_truncate,
    .xSync = esp_sync,
    .xFileSize = esp_file_size,
    .xLock = esp_lock,
    .xUnlock = esp_lock,
    .xCheckReservedLock = esp_check_reserved_lock,
    .xFileControl = esp_file_control,
    .xSectorSize = esp_sector_size,
    .xDeviceCharacteristics = esp_device_characteristics,
};

static int esp_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f, int flags, int *out_flags)
{
    EspFile *p = (EspFile *)f;
    char tmp[MAX_PATHNAME];
    memset(p, 0, sizeof(*p));
    p->fd = -1;

    if (!name) {
        /* Temp files normally live in memory (SQLITE_TEMP_STORE=3); this is
         * only a fallback. Put them next to the database. */
        unsigned r;
        esp_fill_random(&r, sizeof(r));
        snprintf(tmp, sizeof(tmp), "%s/t%07x.tmp", (const char *)vfs->pAppData, r & 0xFFFFFFF);
        name = tmp;
        flags |= SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_CREATE;
    }

    int oflags = (flags & SQLITE_OPEN_READWRITE) ? O_RDWR : O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) {
        oflags |= O_CREAT;
    }
    if (flags & SQLITE_OPEN_EXCLUSIVE) {
        oflags |= O_EXCL;
    }
    p->fd = open(name, oflags, 0644);
    if (p->fd < 0) {
        return SQLITE_CANTOPEN;
    }
    if (flags & SQLITE_OPEN_DELETEONCLOSE) {
        p->delete_on_close = strdup(name);
    }
    if (out_flags) {
        *out_flags = flags;
    }
    p->base.pMethods = &s_io;
    return SQLITE_OK;
}

static int esp_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    (void)vfs; (void)sync_dir;
    if (unlink(name) == 0) {
        return SQLITE_OK;
    }
    return errno == ENOENT ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE;
}

static int esp_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
    (void)vfs; (void)flags;
    struct stat st;
    *out = stat(name, &st) == 0;
    return SQLITE_OK;
}

static int esp_full_pathname(sqlite3_vfs *vfs, const char *name, int n, char *out)
{
    (void)vfs;
    if (strlen(name) >= (size_t)n) {
        return SQLITE_CANTOPEN;
    }
    strcpy(out, name);
    return SQLITE_OK;
}

static int esp_randomness(sqlite3_vfs *vfs, int n, char *out)
{
    (void)vfs;
    esp_fill_random(out, n);
    return n;
}

static int esp_sleep(sqlite3_vfs *vfs, int us)
{
    (void)vfs;
    vTaskDelay(pdMS_TO_TICKS(us / 1000) + 1);
    return us;
}

static int esp_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
    (void)vfs;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* Julian day number in milliseconds; Unix epoch is JD 2440587.5. */
    *out = (sqlite3_int64)210866760000000LL + (sqlite3_int64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return SQLITE_OK;
}

static int esp_current_time(sqlite3_vfs *vfs, double *out)
{
    sqlite3_int64 ms;
    esp_current_time_int64(vfs, &ms);
    *out = ms / 86400000.0;
    return SQLITE_OK;
}

static int esp_get_last_error(sqlite3_vfs *vfs, int n, char *out)
{
    (void)vfs; (void)n; (void)out;
    return errno;
}

static sqlite3_vfs s_vfs = {
    .iVersion = 2,
    .szOsFile = sizeof(EspFile),
    .mxPathname = MAX_PATHNAME,
    .zName = "esp",
    .pAppData = NULL, /* temp-file directory, set by sqlite_esp_set_temp_dir */
    .xOpen = esp_open,
    .xDelete = esp_delete,
    .xAccess = esp_access,
    .xFullPathname = esp_full_pathname,
    .xRandomness = esp_randomness,
    .xSleep = esp_sleep,
    .xCurrentTime = esp_current_time,
    .xGetLastError = esp_get_last_error,
    .xCurrentTimeInt64 = esp_current_time_int64,
};

void sqlite_esp_set_temp_dir(const char *dir)
{
    s_vfs.pAppData = (void *)dir;
}

int sqlite3_os_init(void)
{
    if (!s_vfs.pAppData) {
        s_vfs.pAppData = (void *)"";
    }
    return sqlite3_vfs_register(&s_vfs, 1);
}

int sqlite3_os_end(void)
{
    return SQLITE_OK;
}
