# net: WiFi station with credentials in NVS. Knows nothing about the
# database; a cluster/transport layer will sit on top of it later.
# NET_WIFI=n (Kconfig) builds stubs instead. Requirements can't depend on
# Kconfig, so the WiFi components stay listed; with nothing referencing them
# the linker drops them.
set(srcs "src/state_name.cpp")
if(CONFIG_NET_WIFI)
    list(APPEND srcs "src/net.cpp")
else()
    list(APPEND srcs "src/net_off.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash)
