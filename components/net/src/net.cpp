// net.cpp — see net.hpp.
//
// Reconnects forever. Backoff after a disconnect: 1 s, doubling to 30 s;
// after FAILING_AFTER consecutive failures the state reads Failing (the LED
// shows it) but attempts continue, so the board recovers on its own when the
// AP comes back. Successful DHCP resets the backoff.

#include "net/net.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace net {

static const char *TAG = "net";
static const char *NVS_NS = "net";
constexpr int FAILING_AFTER = 5;
constexpr uint64_t BACKOFF_MIN_US = 1000 * 1000;
constexpr uint64_t BACKOFF_MAX_US = 30 * 1000 * 1000;

static std::atomic<State> s_state{State::Off};
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;
static int s_failures;
static uint64_t s_backoff_us = BACKOFF_MIN_US;
static std::atomic<bool> s_has_creds{false};

static void retry_cb(void *)
{
    esp_wifi_connect();
}

static void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_creds) {
            s_state = State::Connecting;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_has_creds) {
            s_state = State::NoCredentials;
            return;
        }
        auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
        ++s_failures;
        s_state = s_failures >= FAILING_AFTER ? State::Failing : State::Connecting;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %u s", ev->reason,
                 static_cast<unsigned>(s_backoff_us / 1000000));
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, s_backoff_us);
        s_backoff_us = s_backoff_us * 2 > BACKOFF_MAX_US ? BACKOFF_MAX_US : s_backoff_us * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_failures = 0;
        s_backoff_us = BACKOFF_MIN_US;
        s_state = State::Connected;
    }
}

// Loads credentials from NVS into the driver. false if none are stored.
static bool load_credentials()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    wifi_config_t cfg = {};
    size_t ssid_len = sizeof(cfg.sta.ssid);
    size_t pass_len = sizeof(cfg.sta.password);
    bool ok = nvs_get_blob(h, "ssid", cfg.sta.ssid, &ssid_len) == ESP_OK &&
              nvs_get_blob(h, "pass", cfg.sta.password, &pass_len) == ESP_OK && ssid_len > 0;
    nvs_close(h);
    if (ok) {
        // Accept any security mode the AP offers, including open networks.
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable = true;
        ok = esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK;
    }
    return ok;
}

esp_err_t start()
{
    if (s_state != State::Off) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs partition needs erasing (%s); stored credentials are lost", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE: already created
        return err;
    }
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");  // creds are ours, in NVS "net"
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr), TAG, "evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr), TAG, "evt");

    esp_timer_create_args_t targs = {};
    targs.callback = retry_cb;
    targs.name = "wifi_retry";
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "timer");

    s_has_creds = load_credentials();
    s_state = State::NoCredentials;
    if (s_has_creds) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");  // STA_START -> connect
    } else {
        ESP_LOGI(TAG, "no WiFi credentials; set them with: wifi set <ssid> <password>");
    }
    return ESP_OK;
}

State state()
{
    return s_state;
}

esp_err_t set_credentials(const char *ssid, const char *pass)
{
    if (s_state == State::Off) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 63 || (pass_len > 0 && pass_len < 8)) {
        return ESP_ERR_INVALID_ARG;  // 802.11 / WPA2 limits
    }
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(h, "ssid", ssid, ssid_len);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "pass", pass, pass_len);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write");

    esp_timer_stop(s_retry_timer);
    s_failures = 0;
    s_backoff_us = BACKOFF_MIN_US;
    const bool was_started = s_has_creds;
    s_has_creds = false;  // so the disconnect below doesn't schedule a retry
    if (was_started) {
        esp_wifi_disconnect();
    }
    if (!load_credentials()) {
        return ESP_FAIL;
    }
    s_has_creds = true;
    s_state = State::Connecting;
    return was_started ? esp_wifi_connect() : esp_wifi_start();
}

esp_err_t forget()
{
    if (s_state == State::Off) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_timer_stop(s_retry_timer);
    const bool was_started = s_has_creds;
    s_has_creds = false;
    s_state = State::NoCredentials;
    return was_started ? esp_wifi_stop() : ESP_OK;
}

void ssid(char *out, size_t len)
{
    out[0] = '\0';
    wifi_config_t cfg = {};
    if (s_has_creds && esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        snprintf(out, len, "%.*s", static_cast<int>(sizeof(cfg.sta.ssid)), reinterpret_cast<char *>(cfg.sta.ssid));
    }
}

void ip(char *out, size_t len)
{
    out[0] = '\0';
    esp_netif_ip_info_t info;
    if (s_state == State::Connected && s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
        snprintf(out, len, IPSTR, IP2STR(&info.ip));
    }
}

int rssi()
{
    int v = 0;
    if (s_state == State::Connected && esp_wifi_sta_get_rssi(&v) == ESP_OK) {
        return v;
    }
    return 0;
}

}  // namespace net
