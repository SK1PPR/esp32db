// net_off.cpp — net:: when WiFi is disabled in menuconfig (NET_WIFI=n).
// Nothing here references the WiFi driver, so it isn't linked.

#include "net/net.hpp"

#include "esp_log.h"

namespace net {

esp_err_t start()
{
    ESP_LOGI("net", "WiFi disabled in this build (menuconfig: Networking > WiFi station)");
    return ESP_OK;
}

State state() { return State::Off; }
esp_err_t set_credentials(const char *, const char *) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t forget() { return ESP_ERR_NOT_SUPPORTED; }

void ssid(char *out, size_t len)
{
    if (len) {
        out[0] = '\0';
    }
}

void ip(char *out, size_t len)
{
    if (len) {
        out[0] = '\0';
    }
}

int rssi() { return 0; }

}  // namespace net
