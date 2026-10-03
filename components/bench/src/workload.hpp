#pragma once
// workload.hpp — pieces shared by bench.cpp (single-op runs) and race.cpp
// (mixed race): deterministic values, the latency histogram, and the task the
// workload runs in.

#include <cstdint>
#include <cstring>
#include "esp_err.h"
#include "kv/kv.hpp"

namespace bench {

// The runner yields one tick every YIELD_EVERY_US of wall time so the idle
// task can feed the task watchdog.
constexpr int64_t YIELD_EVERY_US = 50 * 1000;

// ---- deterministic keys and values ----

inline uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline void fill_value(uint8_t *buf, uint32_t len, kv::Key key, uint32_t gen)
{
    uint64_t state = key ^ (0xD1B54A32D192ED03ull * (gen + 1));
    for (uint32_t off = 0; off < len; off += 8) {
        state = splitmix64(state);
        const uint32_t n = len - off < 8 ? len - off : 8;
        memcpy(buf + off, &state, n);
    }
}

// ---- latency histogram ----
// Log-linear (32 sub-buckets per power of two, <= ~3% error, 4.7 KB) instead
// of a per-op array: the esp32db board has ballooned nearly all its PSRAM
// into the index.

class Histogram {
public:
    static constexpr int SUB_BITS = 5;  // 32 sub-buckets per power of two
    static constexpr int LINEAR = 2 << SUB_BITS;  // values below this get exact buckets
    static constexpr int MAX_EXP = 40;            // ~12 days in us
    static constexpr int BUCKETS = LINEAR + (MAX_EXP - SUB_BITS - 1) * (1 << SUB_BITS);

    void add(uint32_t us)
    {
        ++counts_[index(us)];
        ++total_;
        if (total_ == 1 || us < min_) {
            min_ = us;
        }
        if (us > max_) {
            max_ = us;
        }
    }

    uint32_t percentile(double p) const
    {
        if (total_ == 0) {
            return 0;
        }
        const uint64_t rank = static_cast<uint64_t>(p * (total_ - 1)) + 1;
        uint64_t seen = 0;
        for (int b = 0; b < BUCKETS; ++b) {
            seen += counts_[b];
            if (seen >= rank) {
                const uint32_t v = midpoint(b);
                return v < min_ ? min_ : (v > max_ ? max_ : v);
            }
        }
        return max_;
    }

    uint32_t min() const { return min_; }
    uint32_t max() const { return max_; }

private:
    static int index(uint32_t v)
    {
        if (v < static_cast<uint32_t>(LINEAR)) {
            return static_cast<int>(v);
        }
        const int exp = 31 - __builtin_clz(v);  // >= SUB_BITS + 1
        const int sub = static_cast<int>(v >> (exp - SUB_BITS)) & ((1 << SUB_BITS) - 1);
        const int b = LINEAR + (exp - SUB_BITS - 1) * (1 << SUB_BITS) + sub;
        return b < BUCKETS ? b : BUCKETS - 1;
    }

    static uint32_t midpoint(int b)
    {
        if (b < LINEAR) {
            return static_cast<uint32_t>(b);
        }
        const int exp = (b - LINEAR) / (1 << SUB_BITS) + SUB_BITS + 1;
        const int sub = (b - LINEAR) % (1 << SUB_BITS);
        const uint64_t lo = (1ull << exp) + (static_cast<uint64_t>(sub) << (exp - SUB_BITS));
        const uint64_t mid = lo + (1ull << (exp - SUB_BITS)) / 2;
        return mid > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(mid);
    }

    uint32_t counts_[BUCKETS] = {};
    uint64_t total_ = 0;
    uint32_t min_ = 0, max_ = 0;
};

// One workload at a time (bench or race): claim() fails while another runs.
bool claim();
void release();

// Runs fn(arg) in a task with a big stack pinned to core 1 (WiFi and the
// console stay on core 0) and blocks until it returns.
esp_err_t run_in_task(void (*fn)(void *), void *arg);

}  // namespace bench
