# db: all database logic. Only include/db/db.hpp is public; btree/kvlog
# headers live in src/ so nothing outside this component can reach past the
# KV API (the shell today, a network/cluster layer tomorrow).
idf_component_register(SRC_DIRS "src"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES mem flash esp_rom freertos)
