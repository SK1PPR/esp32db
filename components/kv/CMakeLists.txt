# kv: one storage engine behind a common API, picked by Kconfig KV_ENGINE_*.
# Requirements can't depend on Kconfig (they're resolved before sdkconfig is
# loaded), so every engine is a requirement and only the selected engine's
# source file is compiled. The flashdb/sqlite components build nothing unless
# they're selected.
set(srcs "src/kv.cpp")
if(CONFIG_KV_ENGINE_ESP32DB)
    list(APPEND srcs "src/engine_esp32db.cpp")
elseif(CONFIG_KV_ENGINE_FLASHDB)
    list(APPEND srcs "src/engine_flashdb.cpp")
elseif(CONFIG_KV_ENGINE_SQLITE)
    list(APPEND srcs "src/engine_sqlite.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES db flash flashdb sqlite esp_partition fatfs freertos)
