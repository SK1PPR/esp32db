// cmd_db.cpp — put <key> <value> | get <key> | del <key> | compact | dbstat | format
// Keys accept decimal or 0x-hex. Values are taken as text; quote them to
// include spaces: put 7 "hello world".

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "commands.hpp"
#include "esp_console.h"
#include "esp_system.h"
#include "db/db.hpp"
#include "mem/mem.hpp"

namespace shell {

static bool parse_key(const char *s, db::Key *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || end == s || *end) {
        printf("bad key: %s\n", s);
        return false;
    }
    *out = v;
    return true;
}

static int report(esp_err_t err)
{
    if (err != ESP_OK) {
        printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int cmd_put(int argc, char **argv)
{
    db::Key key;
    if (argc != 3) {
        printf("usage: put <key> <value>\n");
        return 1;
    }
    if (!parse_key(argv[1], &key)) {
        return 1;
    }
    int rc = report(db::put(key, argv[2], strlen(argv[2])));
    if (rc == 0) {
        printf("OK\n");
    }
    return rc;
}

static int cmd_get(int argc, char **argv)
{
    db::Key key;
    if (argc != 2) {
        printf("usage: get <key>\n");
        return 1;
    }
    if (!parse_key(argv[1], &key)) {
        return 1;
    }
    // Console task stack is small; keep the 4 KB value buffer off it.
    auto *buf = static_cast<char *>(mem::alloc(mem::Region::Psram, db::MAX_VALUE_LEN));
    if (!buf) {
        return report(ESP_ERR_NO_MEM);
    }
    size_t len = 0;
    esp_err_t err = db::get(key, buf, db::MAX_VALUE_LEN, &len);
    if (err == ESP_OK) {
        printf("%.*s\n", static_cast<int>(len), buf);
    }
    mem::free(buf);
    return report(err);
}

static int cmd_del(int argc, char **argv)
{
    db::Key key;
    if (argc != 2) {
        printf("usage: del <key>\n");
        return 1;
    }
    if (!parse_key(argv[1], &key)) {
        return 1;
    }
    int rc = report(db::del(key));
    if (rc == 0) {
        printf("OK\n");
    }
    return rc;
}

static int cmd_compact(int, char **)
{
    db::Stats before = db::stats();
    int rc = report(db::compact());
    if (rc == 0) {
        printf("free sectors: %" PRIu32 " -> %" PRIu32 "\n", before.log_free_sectors,
               db::stats().log_free_sectors);
    }
    return rc;
}

static int cmd_dbstat(int, char **)
{
    db::Stats s = db::stats();
    printf("keys:        %" PRIu32 "\n", s.keys);
    printf("tree height: %" PRIu32 "\n", s.tree_height);
    printf("tree nodes:  %" PRIu32 " leaves, %" PRIu32 " inner in SRAM, %" PRIu32 " inner in PSRAM\n",
           s.leaf_nodes, s.inner_sram_nodes, s.inner_psram_nodes);
    printf("log sectors: %" PRIu32 " free / %" PRIu32 " total\n", s.log_free_sectors, s.log_sectors);
    return 0;
}

// Recovery path when the database can't open (see main.cpp). Destructive, so
// it wants an explicit confirmation word.
static int cmd_format(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("usage: format yes   (erases ALL data, then reboots)\n");
        return 1;
    }
    printf("erasing, this takes a while...\n");
    if (report(db::format()) != 0) {
        return 1;
    }
    printf("done, rebooting\n");
    esp_restart();
}

static void add(const char *name, const char *help, esp_console_cmd_func_t func)
{
    esp_console_cmd_t cmd = {};
    cmd.command = name;
    cmd.help = help;
    cmd.func = func;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void register_db_commands()
{
    add("put", "put <key> <value>  store a value", &cmd_put);
    add("get", "get <key>  print a value", &cmd_get);
    add("del", "del <key>  delete a key", &cmd_del);
    add("compact", "compact the oldest log sector now", &cmd_compact);
    add("dbstat", "show key count, tree shape, log space", &cmd_dbstat);
    add("format", "format yes  erase the whole database and reboot", &cmd_format);
}

}  // namespace shell
