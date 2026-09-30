// cmd_sys.cpp — system introspection. `mem` is the first thing you'll want
// while tuning how much of the index fits in internal SRAM vs PSRAM.

#include <cstdio>
#include "commands.hpp"
#include "esp_console.h"
#include "mem/mem.hpp"

namespace shell {

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("internal: %zu free (largest %zu)\n",
           mem::free_bytes(mem::Region::Internal), mem::largest_free_block(mem::Region::Internal));
    printf("psram:    %zu free (largest %zu)\n",
           mem::free_bytes(mem::Region::Psram), mem::largest_free_block(mem::Region::Psram));
    return 0;
}

void register_sys_commands()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "mem";
    cmd.help = "Show free internal SRAM / PSRAM";
    cmd.func = &cmd_mem;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace shell
