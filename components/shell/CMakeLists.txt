# shell: interactive REPL over UART using IDF's esp_console (linenoise +
# argtable3). It's a *client* of db/mem — it only parses text and calls
# public APIs, so it can be dropped/replaced without touching the engine.
idf_component_register(SRC_DIRS "src"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES console db mem)
