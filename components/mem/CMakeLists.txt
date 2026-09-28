# mem: the only place in the project that decides *which RAM* something lives in.
# Public headers only need <cstddef>, so heap/psram stay private dependencies.
idf_component_register(SRC_DIRS "src"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES heap esp_psram)
