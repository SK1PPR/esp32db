// shell.cpp — REPL setup. Console is UART0 (CONFIG_ESP_CONSOLE_UART_DEFAULT);
// switch to esp_console_new_repl_usb_serial_jtag if you move the primary
// console to the S3's native USB port.

#include "shell/shell.hpp"
#include "commands.hpp"
#include "esp_console.h"

namespace shell {

esp_err_t start()
{
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "db>";

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    register_db_commands();
    register_sys_commands();

    return esp_console_start_repl(repl);
}

}  // namespace shell
