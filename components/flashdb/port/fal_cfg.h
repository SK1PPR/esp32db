/* fal_cfg.h — FAL sees one flash "device": the `fdb` partition from
 * partitions_flashdb.csv, addressed from 0. The whole device is one FAL
 * partition, "kvdb", which FlashDB's KVDB owns. */
#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

#define FAL_PART_HAS_TABLE_CFG
#define FDB_ESP_PARTITION_LABEL "fdb"
#define FDB_FAL_DEV_NAME "esp_fdb"
#define FDB_FAL_PART_NAME "kvdb"
/* Must equal the `fdb` size in partitions_flashdb.csv (checked at init). */
#define FDB_FAL_LEN (0xDF0000)

extern const struct fal_flash_dev fdb_flash_dev;

#define FAL_FLASH_DEV_TABLE \
    {                       \
        &fdb_flash_dev,     \
    }

#define FAL_PART_TABLE                                                                         \
    {                                                                                          \
        {FAL_PART_MAGIC_WORD, FDB_FAL_PART_NAME, FDB_FAL_DEV_NAME, 0, FDB_FAL_LEN, 0},         \
    }

#endif /* _FAL_CFG_H_ */
