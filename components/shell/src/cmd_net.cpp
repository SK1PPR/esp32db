// cmd_net.cpp — wifi set <ssid> [password] | wifi status | wifi forget
// Credentials go to NVS (see net.hpp). Quote an SSID or password with spaces:
//   wifi set "My Network" "my password"

#include <cstdio>
#include <cstring>
#include "commands.hpp"
#include "esp_console.h"
#include "net/net.hpp"

namespace shell {

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        char ssid[33], ip[16];
        net::ssid(ssid, sizeof(ssid));
        net::ip(ip, sizeof(ip));
        printf("state: %s\n", net::state_name(net::state()));
        printf("ssid:  %s\n", ssid[0] ? ssid : "-");
        printf("ip:    %s\n", ip[0] ? ip : "-");
        printf("rssi:  %d dBm\n", net::rssi());
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "set") == 0) {
        esp_err_t err = net::set_credentials(argv[2], argc == 4 ? argv[3] : "");
        if (err != ESP_OK) {
            printf("error: %s%s\n", esp_err_to_name(err),
                   err == ESP_ERR_INVALID_ARG       ? " (ssid 1-32 chars, password empty or 8-63 chars)"
                   : err == ESP_ERR_NOT_SUPPORTED ? " (WiFi is disabled in this build: menuconfig NET_WIFI)"
                                                  : "");
            return 1;
        }
        printf("saved, connecting (watch `wifi status` or the WiFi LED)\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        esp_err_t err = net::forget();
        printf(err == ESP_OK ? "credentials erased\n" : "error: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: wifi status | wifi set <ssid> [password] | wifi forget\n");
    return 1;
}

void register_net_commands()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "wifi";
    cmd.help = "wifi status | wifi set <ssid> [password] | wifi forget";
    cmd.func = &cmd_wifi;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace shell
