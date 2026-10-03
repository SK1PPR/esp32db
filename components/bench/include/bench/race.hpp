#pragma once
// race.hpp — a scripted mixed workload for filming engines side by side.
//
// A race is `ops` operations on a key space of `keys` indices, run as bursts:
//   insert     put keys that don't exist yet
//   overwrite  put keys that exist, with a new value
//   read       get: 90% existing keys (value checked), 10% absent ones
//   delete     del existing keys
//   mixed      every op picked at random: 40% get, 35% put, 25% del
//   read+del   get a key, check it, then delete it
// Which burst comes next and how long it is (32-255 ops) are drawn from the
// seed, weighted by how far the race is: early on mostly inserts, then a mix,
// then read/delete churn. Same seed = the exact same sequence on every engine,
// so separately filmed runs line up.
//
// The board tracks which keys exist and which value each one holds, so every
// get is verified, and a delete of a key that should exist must succeed.
// Starts from an empty database (`format yes` first).
//
// LEDs (main.cpp reads race_view()): 3-2-1 countdown on the green LED, GO
// lights it for a second as the race starts, op LEDs blink once per
// `every_*` ops, and the green LED stays on for 3 s at the finish.

#include <cstdint>
#include "esp_err.h"
#include "kv/kv.hpp"

namespace bench {

struct RaceConfig {
    uint32_t ops = 10000;
    uint32_t keys = 0;  // key space; 0 = ops / 4. At most 65535.
    uint32_t vsize = 64;
    uint32_t seed = 1;
    uint32_t every_put = 10;  // LED: one blink per this many puts
    uint32_t every_get = 100;
    uint32_t every_del = 10;
};

struct RaceOpStats {
    uint32_t n = 0;
    uint32_t ok = 0;
    uint32_t bad = 0;  // wrong value, missing key, or a key that should be gone
    uint32_t err = 0;  // engine returned an error
    uint64_t busy_us = 0;
    uint32_t p50_us = 0, p99_us = 0, max_us = 0;
};

struct RaceResult {
    uint64_t wall_us = 0;  // GO to finish
    uint32_t keys = 0;     // key space actually used (cfg.keys or its default)
    RaceOpStats put, get, del;
    uint32_t bursts = 0;
    uint32_t live_end = 0;  // keys that exist at the finish
    esp_err_t first_err = ESP_OK;
};

// Blocks for the countdown (3 s) plus the race. ESP_ERR_INVALID_STATE if the
// database isn't open, isn't empty, or a bench is already running.
esp_err_t race(const RaceConfig &cfg, RaceResult *out);

enum class RacePhase : uint8_t {
    Idle,       // no race since boot: normal status LEDs
    Countdown,  // 3 s
    Running,
    Finished,   // stays until the next race or a reboot, so the LEDs stay quiet
};

struct RaceView {
    RacePhase phase;
    uint32_t phase_ms;  // time spent in this phase so far
    kv::OpCounts base;  // kv::op_counts() when the race started
    uint32_t every_put, every_get, every_del;
};

RaceView race_view();

}  // namespace bench
