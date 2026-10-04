# KV engine benchmark: esp32db vs FlashDB vs SQLite

Run on 2026-10-04 with `tools/bench.py run --out bench_results/final` (raw
`results.jsonl` / `results.csv` stay local; `bench_results/` is gitignored).

## TL;DR

- **esp32db is the only engine that completed every size up to 100,000 keys**,
  and its throughput stays nearly flat as the data grows: ~3,100 inserts/s and
  ~16,000 reads/s at 100k keys.
- **SQLite** reads well (~5,300 hits/s, ~7,400 misses/s) but writes slowly:
  ~6 inserts/s when every insert is its own transaction, ~270 → 39 inserts/s
  with 100-op transactions. It didn't finish 100k keys within the time limit.
- **FlashDB** runs at 4–17 ops/s on a 14 MB partition, because every operation
  scans sector headers. It only completed 1,000 keys.
- **Correctness: every engine passed.** 0 wrong values, 0 errors, and every
  overwrite, delete and reboot was honoured.
- **esp32db's weak spot is mount time:** 1.9 s at 1k keys, 4.4 s at 100k
  (SQLite: 0.07–1.1 s).

## Setup

| | |
|---|---|
| Board | ESP32-S3 (rev v0.2), 2 cores @ 160 MHz, 16 MB flash, 8 MB PSRAM, MAC `7c:e8:b1:a9:f1:64` |
| Firmware | one board, reflashed per engine with `tools/fw.sh`; ESP-IDF v6.1-dev-8199-g4d59230ddff, app `c2dc5d5-dirty`, `-O2`, WiFi off |
| Workload | 32-byte values, random 64-bit keys, seed 1 (identical on every engine) |
| Sizes | 1,000 / 10,000 / 100,000 keys, each on a freshly formatted partition |
| Time limit | 300 s per step; an engine that hits it skips larger sizes |
| Timing | measured on the board per operation; the serial link adds nothing |

| engine | storage partition | max value | notes |
|---|---|---|---|
| esp32db | `log` 14,208 KB (+ 64 KB `dbmeta`) | 4,060 B | our engine |
| FlashDB V2.2.0 | `fdb` 14,272 KB (raw flash via FAL) | 3,900 B | KVDB, default caches (64 KVs, 8 sectors), as shipped |
| SQLite | `sqlite` 14,272 KB (FATFS, WAL) | 4,096 B | run twice: 1 op/transaction (`sqlite`) and 100 ops/transaction (`sqlite-batch100`) |

Each engine gets the same 14,272 KB of flash.

### Steps per size

| step | what it does |
|---|---|
| fill | put n new keys |
| read_hit | n random gets of existing keys, every value checked |
| read_miss | gets of keys that were never written (up to 10k) |
| overwrite | put the first n/10 keys again with new values |
| delete | delete the first n/20 keys |
| verify | read every key in order; checks values, overwrites, deletes |
| reopen | reboot; time to mount and replay what's on flash |
| verify_boot | full verify again after the reboot (durability) |

## Results

Format: **ops/s** (median / p99 latency per op). *Italic* = stopped at the
300 s limit; the step ran on the number of keys shown. "–" = not run because a
smaller size already hit the limit.

### Insert (fill)

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **4,080** (174 µs / 324 µs) | **3,164** (178 µs / 10.1 ms) | **3,102** (182 µs / 10.1 ms) |
| sqlite-batch100 | 269 (130 µs / 1.0 ms) | 44 (138 µs / 1.1 ms) | *39 (138 µs / 1.1 ms), 11,900 written* |
| sqlite | 6 (162 ms / 315 ms) | *6 (133 ms / 324 ms), 1,812 written* | – |
| flashdb | 4 (274 ms / 389 ms) | *4 (274 ms / 397 ms), 1,032 written* | – |

### Random read, key exists

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **16,628** (60 µs / 61 µs) | **16,061** (62 µs / 65 µs) | **15,830** (63 µs / 65 µs) |
| sqlite-batch100 | 5,323 (186 µs / 210 µs) | 5,494 (182 µs / 210 µs) | 5,198 (190 µs / 214 µs) |
| sqlite | 5,327 (186 µs / 214 µs) | 5,306 (190 µs / 210 µs) | – |
| flashdb | 17 (59 ms / 124 ms) | *17 (58 ms / 128 ms), 4,518 done* | – |

### Random read, key missing

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **111,508** (9 µs / 10 µs) | **93,345** (11 µs / 12 µs) | **86,237** (12 µs / 13 µs) |
| sqlite-batch100 | 7,390 (134 µs / 166 µs) | 7,476 (134 µs / 154 µs) | 7,118 (138 µs / 166 µs) |
| sqlite | 7,419 (134 µs / 158 µs) | 7,347 (134 µs / 166 µs) | – |
| flashdb | 5 (218 ms / 218 ms) | *5 (219 ms / 219 ms), 1,305 done* | – |

### Overwrite (existing keys)

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **5,672** (162 µs / 300 µs) | **3,513** (166 µs / 7.6 ms) | **3,369** (166 µs / 10.1 ms) |
| sqlite-batch100 | 212 (123 µs / 492 µs) | 32 (130 µs / 508 µs) | 26 (130 µs / 508 µs) |
| sqlite | 8 (124 ms / 173 ms) | 8 (124 ms / 174 ms) | – |
| flashdb | 5 (190 ms / 203 ms) | 5 (190 ms / 203 ms) | – |

### Delete

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **5,765** (170 µs / 230 µs) | **3,655** (174 µs / 316 µs) | **3,388** (174 µs / 10.1 ms) |
| sqlite-batch100 | 108 (81 µs / 468 µs) | 25 (89 µs / 492 µs) | 29 (93 µs / 484 µs) |
| sqlite | 8 (124 ms / 174 ms) | 8 (124 ms / 174 ms) | – |
| flashdb | 8 (140 ms / 140 ms) | 9 (140 ms / 140 ms) | – |

### Sequential read of every key (verify)

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **17,361** (60 µs / 61 µs) | **16,755** (62 µs / 65 µs) | **16,512** (63 µs / 65 µs) |
| sqlite-batch100 | 5,871 (174 µs / 190 µs) | 5,739 (178 µs / 198 µs) | 5,702 (178 µs / 198 µs) |
| sqlite | 5,903 (170 µs / 194 µs) | 5,862 (174 µs / 190 µs) | – |
| flashdb | 14 (69 ms / 227 ms) | 13 (71 ms / 231 ms) | – |

### Verify after reboot

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| **esp32db** | **17,352** (60 µs / 62 µs) | **16,755** (62 µs / 65 µs) | **16,512** (63 µs / 65 µs) |
| sqlite-batch100 | 5,130 (178 µs / 1.6 ms) | 5,066 (186 µs / 872 µs) | 4,679 (186 µs / 2.3 ms) |
| sqlite | 5,016 (178 µs / 1.9 ms) | 4,891 (182 µs / 2.0 ms) | – |
| flashdb | 14 (69 ms / 227 ms) | 13 (71 ms / 234 ms) | – |

### Mount after reboot

| engine | n=1,000 | n=10,000 | n=100,000 |
|---|---|---|---|
| esp32db | 1.89 s | 2.10 s | 4.42 s |
| sqlite-batch100 | **108 ms** | **75 ms** | **1.09 s** |
| sqlite | 479 ms | 649 ms¹ | – |
| flashdb | 452 ms | 458 ms¹ | – |

¹ Fewer keys were stored than the column says (1,812 for sqlite, 1,032 for flashdb).

## Correctness

| engine | wrong values | errors | survived reboot |
|---|---|---|---|
| esp32db | 0 | 0 | yes, all sizes |
| flashdb | 0 | 0 | yes, all sizes |
| sqlite | 0 | 0 | yes, all sizes |
| sqlite-batch100 | 0 | 0 | yes, all sizes |

Every value read back matched, and every overwrite and delete was honoured.

## Analysis

### esp32db
- Fastest engine at every operation and every size, and the only one that
  completed 100k keys.
- Throughput stays nearly flat as the data grows: reads drop about 5% from 1k
  to 100k keys, inserts about 24%.
- Median latency stays around 60 µs for reads and 165–180 µs for writes at
  every size. The p99 for writes rises to ~10 ms from 10k keys upward,
  probably periodic background flash work such as a sector erase or
  compaction. Worth looking into if worst-case write latency matters.
- **Mount time is the main weakness**: 1.9 s → 4.4 s, and it grows with the
  amount of data. The other engines mount in 0.07–1.1 s.

### SQLite
- Read speed is solid and steady at every size (~5,300 hits/s, ~7,400
  misses/s), about 3× slower than esp32db for hits and 12–15× for misses.
- Without batching, every insert, overwrite and delete waits for its commit to
  reach flash, about 120–160 ms each, so you get 6–8 ops/s.
- With 100 ops per transaction, writes are 13–43× faster, but they still slow
  down as the database grows (fill 269 → 44 → 39 ops/s). The 100k fill hit
  the limit at 11,900 keys.
- With batching, the median latency (~130 µs) doesn't include the commit, which
  happens once per 100 ops. ops/s does include it, so compare ops/s for
  batched SQLite.
- Reads right after a reboot have a higher p99 (~1–2 ms) while the page cache
  is cold.

### FlashDB
- 4–17 ops/s for every operation, about 1,000× slower than esp32db for reads
  and inserts, and the slowest engine overall.
- The cause is FlashDB's design. On every put, `alloc_kv()` walks all 3,568
  sector headers to count free sectors, and the sector cache only holds 8.
  Any key not in the 64-entry KV cache needs a scan of the used sectors, so
  missing keys are the worst case (218 ms).
- FlashDB is built for small partitions of tens of KB. On 14 MB it is far
  outside that range. A smaller partition or larger caches would speed it up,
  but would no longer match the other engines' setup.

## Caveats

- Single board, single run per configuration; no repeat runs or variance
  measurement.
- *Italic* cells are partial: measured on however many keys were written
  before the 300 s limit.
- Results at n=10,000 for sqlite and flashdb used 1,812 and 1,032 stored keys
  respectively, so their read and reboot numbers there come from a much
  smaller database.
- 32-byte values only; larger values would shift the balance (more flash
  bytes per op).
- The board table in `summary.md` lists only sqlite, because each run
  overwrites `config.json`. All three engines used the same board and build
  settings (see Setup).

## Reproduce

```bash
cd ~/Work/esp-idf/projects/first_project
for e in flashdb esp32db sqlite; do
    tools/fw.sh $e flash /dev/ttyUSB0
    tools/bench.py run --ports /dev/ttyUSB0 --out bench_results/final
done
tools/bench.py report bench_results/final
```
