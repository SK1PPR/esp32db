// cmd_sys.cpp — system introspection. `mem` is the first thing you'll want
// while tuning how much of the index fits in internal SRAM vs PSRAM; `info`
// is what tools/bench.py uses to identify a board.

#include <cinttypes>
#include <cstdio>
#include "commands.hpp"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "kv/kv.hpp"
#include "mem/mem.hpp"
#include "net/net.hpp"
#include "sdkconfig.h"

namespace shell {

static int cmd_mem(int, char **)
{
    printf("internal: %zu free (largest %zu)\n",
           mem::free_bytes(mem::Region::Internal), mem::largest_free_block(mem::Region::Internal));
    printf("psram:    %zu free (largest %zu)\n",
           mem::free_bytes(mem::Region::Psram), mem::largest_free_block(mem::Region::Psram));
    printf("balloon:  internal %zu KB, psram %zu KB (held by the database)\n",
           mem::arena(mem::Region::Internal).size / 1024, mem::arena(mem::Region::Psram).size / 1024);
    return 0;
}

static const char *opt_level()
{
#if CONFIG_COMPILER_OPTIMIZATION_PERF
    return "O2";
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
    return "Os";
#elif CONFIG_COMPILER_OPTIMIZATION_NONE
    return "O0";
#else
    return "Og";
#endif
}

// One machine-readable line: everything needed to tell boards apart and to
// spot two boards built or configured differently.
static int cmd_info(int, char **)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_BASE);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, kv::partition_name());

    printf("INFO {\"engine\":\"%s\",\"open\":%s,\"keys\":%" PRId64 ",\"max_value\":%u,\"partition\":\"%s\""
           ",\"partition_kb\":%u,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"chip_rev\":%u,\"cores\":%u"
           ",\"cpu_mhz\":%d,\"flash_mb\":%u,\"psram_mb\":%u,\"opt\":\"%s\",\"idf\":\"%s\",\"app\":\"%s\""
           ",\"built\":\"%s %s\",\"wifi\":\"%s\",\"free_internal\":%zu,\"free_psram\":%zu}\n",
           kv::engine_name(), kv::is_open() ? "true" : "false", kv::key_count(),
           static_cast<unsigned>(kv::max_value_len()), kv::partition_name(),
           part ? static_cast<unsigned>(part->size / 1024) : 0u, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           chip.revision, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, static_cast<unsigned>(flash_size >> 20),
           static_cast<unsigned>(esp_psram_get_size() >> 20), opt_level(), app->idf_ver, app->version, app->date,
           app->time, net::state_name(net::state()), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_reboot(int, char **)
{
    printf("rebooting\n");
    fflush(stdout);
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

void register_sys_commands()
{
    add("mem", "Show free internal SRAM / PSRAM", &cmd_mem);
    add("info", "Board, build and engine details as one INFO {json} line", &cmd_info);
    add("reboot", "Restart the board (data on flash is kept)", &cmd_reboot);
}

}  // namespace shell
