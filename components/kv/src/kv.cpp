// kv.cpp — engine-independent part of kv::. Counts put/get/del so main.cpp
// can flash an LED per operation; a relaxed atomic add costs nothing next to
// a flash write, so bench numbers are unaffected.

#include <atomic>
#include "kv/kv.hpp"
#include "engine.hpp"

namespace kv {

static std::atomic<uint32_t> s_puts{0};
static std::atomic<uint32_t> s_gets{0};
static std::atomic<uint32_t> s_dels{0};

esp_err_t put(Key key, const void *value, size_t len)
{
    s_puts.fetch_add(1, std::memory_order_relaxed);
    return engine_put(key, value, len);
}

esp_err_t get(Key key, void *out, size_t cap, size_t *out_len)
{
    s_gets.fetch_add(1, std::memory_order_relaxed);
    return engine_get(key, out, cap, out_len);
}

esp_err_t del(Key key)
{
    s_dels.fetch_add(1, std::memory_order_relaxed);
    return engine_del(key);
}

OpCounts op_counts()
{
    return {s_puts.load(std::memory_order_relaxed), s_gets.load(std::memory_order_relaxed),
            s_dels.load(std::memory_order_relaxed)};
}

}  // namespace kv
