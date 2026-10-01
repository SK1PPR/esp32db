/* fdb_cfg.h — FlashDB build configuration for the benchmark board.
 * KVDB only, on raw flash through FAL (FlashDB's intended mode on NOR).
 * Cache sizes are left at FlashDB's defaults (64 KV entries, 8 sectors):
 * the benchmark measures FlashDB as shipped. */
#ifndef _FDB_CFG_H_
#define _FDB_CFG_H_

#define FDB_USING_KVDB
#define FDB_USING_FAL_MODE
#define FDB_WRITE_GRAN 1 /* NOR flash: bit-level writes */

#endif /* _FDB_CFG_H_ */
