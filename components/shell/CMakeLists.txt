# shell: interactive REPL over UART using IDF's esp_console (linenoise +
# argtable3). It's a *client* of kv/bench/net/mem — it only parses text and
# calls public APIs, so it can be dropped/replaced without touching the engine.
# `db` is only used for the esp32db-specific `compact` command.
idf_component_register(SRC_DIRS "src"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES console kv db bench net mem heap esp_psram esp_app_format
                                     spi_flash esp_partition esp_hw_support esp_system)
