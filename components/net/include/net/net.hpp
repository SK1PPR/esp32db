#pragma once
// net.hpp — WiFi station. Credentials live in NVS (set from the shell with
// `wifi set`), never in the source or sdkconfig. Without credentials the
// driver is initialised but the radio stays off.
//
// With NET_WIFI=n in menuconfig (the default for now) all of this is stubs:
// state() is Off and set_credentials()/forget() return ESP_ERR_NOT_SUPPORTED.
//
// Call start() before mem::balloon(): the WiFi driver allocates its buffers
// at init and the balloon must leave them alone.

#include <cstddef>
#include "esp_err.h"

namespace net {

enum class State {
    Off,            // start() not called / failed
    NoCredentials,  // driver up, nothing to connect to
    Connecting,     // associating or waiting for DHCP (retries forever)
    Connected,      // have an IP
    Failing,        // several attempts in a row failed (bad password, AP gone); still retrying, slower
};

esp_err_t start();
State state();
const char *state_name(State s);

// Store credentials and (re)connect with them. pass may be "" for open networks.
esp_err_t set_credentials(const char *ssid, const char *pass);
esp_err_t forget();  // erase credentials and disconnect

// Current SSID / IP as text ("" if none), and RSSI in dBm (0 if not associated).
void ssid(char *out, size_t len);
void ip(char *out, size_t len);
int rssi();

}  // namespace net
