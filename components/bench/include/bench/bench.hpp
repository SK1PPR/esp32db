#pragma once
// bench.hpp — on-device workload runner for comparing storage engines.
//
// Everything is timed *on the board*, per operation, so the transport the
// host uses to start a run (UART today, WiFi later) never shows up in the
// numbers. Only the kv:: API is used, so the same run means the same thing on
// every engine.
//
// Data model: the workload addresses keys by index i. Key and value are pure
// functions of (i, seed, gen), so a run can verify every value it reads
// without storing anything:
//   key(i)   = i                     (keys=seq)
//            = mix64(i, seed)        (keys=rand; a bijection, so no duplicates)
//   value(i) = vsize pseudo-random bytes from (key(i), gen)
// The host tracks which indices hold which generation (fill = gen 0,
// overwrite = gen 1) and which were deleted, and passes that in as `Expect`.

#include <cstdint>
#include "esp_err.h"

namespace bench {

enum class Op : uint8_t { Put, Get, Del };
enum class KeyMap : uint8_t { Seq, Rand };
enum class Order : uint8_t { Seq, Rand };

// What `get` should find at index i:
//   i < del_below or i >= live  -> absent
//   i < ow_below                -> value of gen 1
//   otherwise                   -> value of gen 0
struct Expect {
    uint32_t del_below = 0;
    uint32_t ow_below = 0;
    uint32_t live = UINT32_MAX;
};

struct Config {
    Op op = Op::Get;
    uint32_t n = 0;      // operations to run
    uint32_t start = 0;  // first index
    uint32_t range = 0;  // order=rand: indices drawn from [start, start+range); 0 = n
    KeyMap keys = KeyMap::Rand;
    Order order = Order::Seq;  // get only; put/del always walk indices in order
    uint32_t vsize = 32;
    uint32_t seed = 1;
    uint32_t gen = 0;    // put: generation of the values written
    uint32_t batch = 1;  // put/del: ops per kv::begin/end_batch (1 = no batching)
    uint32_t max_s = 0;  // stop early after this many seconds (0 = no limit)
    Expect expect;
};

struct Result {
    uint32_t done = 0;       // ops attempted
    uint32_t ok = 0;         // succeeded (get: found the expected value or expected absence)
    uint32_t bad = 0;        // get: wrong value, missing key, or a key that should be gone
    uint32_t err = 0;        // engine returned an error
    esp_err_t first_err = ESP_OK;
    uint32_t first_bad_index = UINT32_MAX;
    bool timed_out = false;
    uint64_t busy_us = 0;    // sum of per-op latencies (yields excluded)
    uint64_t wall_us = 0;
    uint32_t min_us = 0, max_us = 0;
    uint32_t p50_us = 0, p90_us = 0, p99_us = 0, p999_us = 0;
};

// Validates cfg, runs it in a dedicated task (bigger stack, pinned to core 1)
// and blocks until it finishes. ESP_ERR_INVALID_ARG for a bad config.
esp_err_t run(const Config &cfg, Result *out);

bool running();  // for the status LED

}  // namespace bench
