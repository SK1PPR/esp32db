# flash: raw access to the dbmeta/log partitions. Knows about sectors,
# erase-before-write and wear, knows nothing about keys or B+ trees.
# esp_partition is public because partition.hpp exposes esp_partition_t;
# mem holds the per-sector seq table in PSRAM; esp_rom provides crc32.
idf_component_register(SRC_DIRS "src"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_partition
                       PRIV_REQUIRES mem esp_rom)
