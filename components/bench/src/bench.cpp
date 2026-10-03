// bench.cpp — see bench.hpp.
//
// Timing: each op is bracketed by esp_timer_get_time() (1 us resolution).
// Value generation and verification happen outside the brackets. For batched
// runs, BEGIN is charged to the first op of a batch and COMMIT to the last,
// which is where an application would pay for them.
//
// The runner yields one tick every YIELD_EVERY_US of wall time so the idle
// task can feed the task watchdog; that pause is outside every bracket.
// Latencies go into the Histogram in workload.hpp.

#include "bench/bench.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kv/kv.hpp"
#include "workload.hpp"

namespace bench {

static std::atomic<bool> s_running{false};
constexpr uint32_t TASK_STACK = 16 * 1024;  // SQLite needs more than the console's 4 KB
constexpr BaseType_t TASK_CORE = 1;         // WiFi and the console stay on core 0

static kv::Key key_of(const Config &cfg, uint32_t i)
{
    return cfg.keys == KeyMap::Seq ? i : splitmix64((static_cast<uint64_t>(cfg.seed) << 32) | i);
}

// ---- the run ----

struct Job {
    const Config *cfg;
    Result *res;
    Histogram *hist;
    uint8_t *val;  // value written / expected
    uint8_t *got;  // value read back
};

static void note_err(Result &r, esp_err_t err)
{
    ++r.err;
    if (r.first_err == ESP_OK) {
        r.first_err = err;
    }
}

static void note_bad(Result &r, uint32_t i)
{
    ++r.bad;
    if (r.first_bad_index == UINT32_MAX) {
        r.first_bad_index = i;
    }
}

// What get(i) should return: -1 absent, else the generation.
static int expected_gen(const Expect &e, uint32_t i)
{
    if (i < e.del_below || i >= e.live) {
        return -1;
    }
    return i < e.ow_below ? 1 : 0;
}

static void run_job(Job &job)
{
    const Config &cfg = *job.cfg;
    Result &r = *job.res;
    Histogram &h = *job.hist;
    const uint32_t range = cfg.range ? cfg.range : cfg.n;
    const size_t cap = kv::max_value_len();
    const bool batched = cfg.batch > 1 && cfg.op != Op::Get;
    uint64_t rng = splitmix64(cfg.seed ^ 0xA5A5A5A5ull);

    const int64_t t_start = esp_timer_get_time();
    int64_t last_yield = t_start;
    bool in_batch = false;

    for (uint32_t k = 0; k < cfg.n; ++k) {
        int64_t now = esp_timer_get_time();
        if (now - last_yield > YIELD_EVERY_US) {
            vTaskDelay(1);
            last_yield = esp_timer_get_time();
        }
        // Only stop on a batch boundary, so an open transaction is never left behind.
        if (cfg.max_s && !in_batch && now - t_start > static_cast<int64_t>(cfg.max_s) * 1000000) {
            r.timed_out = true;
            break;
        }

        uint32_t i;
        if (cfg.op == Op::Get && cfg.order == Order::Rand) {
            rng = splitmix64(rng);
            i = cfg.start + static_cast<uint32_t>(rng % range);
        } else {
            i = cfg.start + k;
        }
        const kv::Key key = key_of(cfg, i);
        if (cfg.op == Op::Put) {
            fill_value(job.val, cfg.vsize, key, cfg.gen);
        }
        const bool batch_first = batched && !in_batch;
        const bool batch_last = batched && ((k + 1) % cfg.batch == 0 || k + 1 == cfg.n);

        // ---- timed ----
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = ESP_OK;
        if (batch_first) {
            err = kv::begin_batch();
            in_batch = err == ESP_OK;
        }
        size_t got_len = 0;
        if (err == ESP_OK) {
            switch (cfg.op) {
            case Op::Put: err = kv::put(key, job.val, cfg.vsize); break;
            case Op::Get: err = kv::get(key, job.got, cap, &got_len); break;
            case Op::Del: err = kv::del(key); break;
            }
        }
        esp_err_t commit_err = ESP_OK;
        if (batch_last && in_batch) {
            commit_err = kv::end_batch();
            in_batch = false;
        }
        const int64_t t1 = esp_timer_get_time();
        // ---- end timed ----

        const uint32_t us = static_cast<uint32_t>(t1 - t0);
        h.add(us);
        r.busy_us += us;
        ++r.done;

        if (commit_err != ESP_OK) {
            note_err(r, commit_err);
        }
        if (cfg.op != Op::Get) {
            if (err == ESP_OK) {
                ++r.ok;
            } else {
                note_err(r, err);
            }
            continue;
        }
        const int want = expected_gen(cfg.expect, i);
        if (err == ESP_ERR_NOT_FOUND) {
            if (want < 0) {
                ++r.ok;
            } else {
                note_bad(r, i);
            }
        } else if (err != ESP_OK) {
            note_err(r, err);
        } else if (want < 0 || got_len != cfg.vsize) {
            note_bad(r, i);
        } else {
            fill_value(job.val, cfg.vsize, key, static_cast<uint32_t>(want));
            if (memcmp(job.val, job.got, cfg.vsize) == 0) {
                ++r.ok;
            } else {
                note_bad(r, i);
            }
        }
    }
    if (in_batch) {  // only reachable if begin_batch succeeded on the final op
        esp_err_t err = kv::end_batch();
        if (err != ESP_OK) {
            note_err(r, err);
        }
    }

    r.wall_us = esp_timer_get_time() - t_start;
    r.min_us = h.min();
    r.max_us = h.max();
    r.p50_us = h.percentile(0.50);
    r.p90_us = h.percentile(0.90);
    r.p99_us = h.percentile(0.99);
    r.p999_us = h.percentile(0.999);
}

struct TaskCall {
    void (*fn)(void *);
    void *arg;
    TaskHandle_t waiter;
};

static void call_task(void *p)
{
    TaskCall &c = *static_cast<TaskCall *>(p);
    c.fn(c.arg);
    xTaskNotifyGive(c.waiter);
    vTaskDelete(nullptr);
}

esp_err_t run_in_task(void (*fn)(void *), void *arg)
{
    TaskCall call{fn, arg, xTaskGetCurrentTaskHandle()};
    if (xTaskCreatePinnedToCore(call_task, "bench", TASK_STACK, &call, uxTaskPriorityGet(nullptr), nullptr,
                                TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

bool claim()
{
    bool expected = false;
    return s_running.compare_exchange_strong(expected, true);
}

void release()
{
    s_running = false;
}

esp_err_t run(const Config &cfg, Result *out)
{
    if (!out || cfg.n == 0 || cfg.batch == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.op != Op::Get && cfg.order == Order::Rand) {
        return ESP_ERR_INVALID_ARG;  // put/del walk indices in order (see bench.hpp)
    }
    if (cfg.op == Op::Put && (cfg.vsize == 0 || cfg.vsize > kv::max_value_len())) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (static_cast<uint64_t>(cfg.start) + cfg.n > UINT32_MAX ||
        static_cast<uint64_t>(cfg.start) + (cfg.range ? cfg.range : cfg.n) > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kv::is_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim()) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = Result{};
    auto *hist = new (std::nothrow) Histogram();
    auto *val = static_cast<uint8_t *>(malloc(kv::max_value_len()));
    auto *got = static_cast<uint8_t *>(malloc(kv::max_value_len()));
    esp_err_t err = ESP_OK;
    if (!hist || !val || !got) {
        err = ESP_ERR_NO_MEM;
    } else {
        Job job{&cfg, out, hist, val, got};
        err = run_in_task([](void *j) { run_job(*static_cast<Job *>(j)); }, &job);
    }
    delete hist;
    free(val);
    free(got);
    release();
    return err;
}

bool running()
{
    return s_running;
}

}  // namespace bench
