# Roadmap

B+ tree key/value store on ESP32-S3 N16R8, growing into a distributed database.

## Done

- [x] Component layout: `mem`, `flash`, `db`, `shell`
- [x] Partition table: `dbmeta` (checkpoints) + `log` (WAL and data in one, ~14 MB)
- [x] `flash::AppendLog`: sector allocation, self-describing sector headers, header invalidated before erase
- [x] `db::KvLog`: CRC'd put/del records, replay, oldest-first compaction, tombstone rule
- [x] `db::Meta`: format-version checkpoints in `dbmeta`
- [x] B+ tree: 512 B nodes, fast path for increasing keys, 25% rebalance threshold, exact `can_insert(key)`
- [x] `mem::balloon()`: index gets all RAM left after startup (`main.cpp`), minus reserves
- [x] Background + inline compaction, stall back-off when the log is mostly live data
- [x] Shell: `put`, `get`, `del`, `compact`, `dbstat`, `format yes`, `mem`
- [x] Host test for the B+ tree vs `std::map` (32/64-bit, ASan/UBSan) in `build/host_btree_test/`
- [x] `kv::` engine seam: one firmware per engine (esp32db / FlashDB / SQLite), same shell + `bench` on every board
- [x] `bench` shell command: on-device timed put/get/del with value verification, p50/p99/max
- [x] `tools/bench.py`: drives all boards in parallel, scale ladder, correctness + reboot durability, report
- [x] WiFi station (`net`), credentials in NVS via `wifi set`, status LEDs

## 1. Make the storage trustworthy

- [ ] Host test for `flash/log`, `kvlog`, `meta` with a fake NOR flash (write 1→0 only, 4 KB erase to 0xFF)
- [ ] Power-cut injection in that test: stop mid-write / mid-erase, reboot, compare against `std::map`
- [ ] Move the host tests somewhere tracked (e.g. `test/host/`) with a small build script
- [ ] Commit the project to git

## 2. Run it on the board

- [x] Flash, check the `mem::balloon` log line and `dbstat`
- [x] Try `put` / `get` / `del` / `compact` / `format yes` from the shell
- [ ] Run `tools/bench.py` on the four boards (esp32db, flashdb, sqlite + one spare) and commit the summary
- [ ] Fill the log past the low watermark and watch background compaction
- [ ] Real power-pull test: write in a loop, unplug a few times, verify acknowledged data survived
- [ ] Check the REPL is on the port you're plugged into (UART0 vs USB-Serial-JTAG)

## 3. Tune (measure with `bench` first)

- [ ] Data cache 64 KB / 64 B lines in menuconfig; compare random `get`s
- [x] `idf.py save-defconfig` so `sdkconfig.defaults` matches the real config
- [ ] Revisit `SRAM_RESERVE` / `PSRAM_RESERVE` in `main.cpp` once WiFi is in
- [ ] Greedy compaction (per-sector live bytes); switch the tombstone rule to "keep while any older sector is used"
- [ ] Faster boot if needed: clean-shutdown flag to skip blank checks, index snapshot, or bulk-load on replay
- [ ] Only if needed: mmap reads, release the lock during erase, group commit

## 4. Before going distributed

- [ ] Range scan API: `db::scan(from, to, callback)` over the leaf chain
- [ ] Key design: string keys hashed to `uint64` (= ring position), full key stored in the record for collisions

## 5. Distributed database

- [ ] Transport over WiFi (request/response over TCP/UDP); WiFi itself is up, before `mem::balloon()`
- [ ] `cluster` component in front of `db`: consistent hash ring with virtual nodes, local vs forward
- [ ] Membership: static node list first, then heartbeats / failure detection
- [ ] Rebalancing: stream affected key ranges when the ring changes (uses scan)
- [ ] Replication: N copies per key, write/read quorum policy
- [ ] Failure handling: hinted handoff / anti-entropy repair
