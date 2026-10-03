# bench: on-device workload runner behind the `bench` shell command. Talks to
# storage only through kv::, so it means the same thing on every engine.
idf_component_register(SRC_DIRS "src"
                       INCLUDE_DIRS "include"
                       REQUIRES kv
                       PRIV_REQUIRES esp_timer freertos)
