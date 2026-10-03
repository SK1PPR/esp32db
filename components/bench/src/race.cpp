// race.cpp — see race.hpp.
//
// Timing: each op is bracketed like in bench.cpp (per-op histograms per op
// type); the race time is wall clock from GO to the last op, so it includes
// value generation, verification and watchdog yields. Those cost the same on
// every engine.

#include "bench/race.hpp"

#include <atomic>
#include <cstdlib>
#include <new>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "workload.hpp"

namespace bench {

constexpr uint32_t COUNTDOWN_MS = 3000;
constexpr uint32_t MAX_KEYS = 65535;  // Keyspace uses 16-bit slots

// ---- LED view ----

static std::atomic<RacePhase> s_phase{RacePhase::Idle};
static std::atomic<uint32_t> s_phase_start_ms{0};
static std::atomic<uint32_t> s_base_puts{0}, s_base_gets{0}, s_base_dels{0};
static std::atomic<uint32_t> s_every_put{1}, s_every_get{1}, s_every_del{1};

static uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static void set_phase(RacePhase p)
{
    s_phase_start_ms = now_ms();
    s_phase = p;
}

RaceView race_view()
{
    RaceView v;
    v.phase = s_phase;
    v.phase_ms = now_ms() - s_phase_start_ms;
    v.base = {s_base_puts, s_base_gets, s_base_dels};
    v.every_put = s_every_put;
    v.every_get = s_every_get;
    v.every_del = s_every_del;
    return v;
}

// ---- which keys exist ----

// perm[0, live) are the indices that exist, perm[live, k) the absent ones, and
// pos[i] is index i's slot in perm, so picking a random existing or absent
// key, inserting and deleting are all O(1). 5 bytes per key.
class Keyspace {
public:
    ~Keyspace()
    {
        free(perm_);
        free(pos_);
        free(gen_);
    }

    bool init(uint32_t k)
    {
        k_ = k;
        perm_ = static_cast<uint16_t *>(malloc(k * sizeof(uint16_t)));
        pos_ = static_cast<uint16_t *>(malloc(k * sizeof(uint16_t)));
        gen_ = static_cast<uint8_t *>(calloc(k, 1));
        if (!perm_ || !pos_ || !gen_) {
            return false;
        }
        for (uint32_t i = 0; i < k; ++i) {
            perm_[i] = pos_[i] = static_cast<uint16_t>(i);
        }
        return true;
    }

    uint32_t live() const { return live_; }
    uint32_t absent() const { return k_ - live_; }
    bool is_live(uint32_t i) const { return pos_[i] < live_; }
    uint32_t pick_live(uint64_t r) const { return perm_[r % live_]; }
    uint32_t pick_absent(uint64_t r) const { return perm_[live_ + r % (k_ - live_)]; }
    uint8_t &gen(uint32_t i) { return gen_[i]; }  // generation of the value it holds

    void set_live(uint32_t i)
    {
        if (!is_live(i)) {
            move(i, live_);
            ++live_;
        }
    }

    void set_absent(uint32_t i)
    {
        if (is_live(i)) {
            --live_;
            move(i, live_);
        }
    }

private:
    void move(uint32_t i, uint32_t slot)
    {
        const uint16_t other = perm_[slot];
        const uint16_t from = pos_[i];
        perm_[from] = other;
        pos_[other] = from;
        perm_[slot] = static_cast<uint16_t>(i);
        pos_[i] = static_cast<uint16_t>(slot);
    }

    uint16_t *perm_ = nullptr;
    uint16_t *pos_ = nullptr;
    uint8_t *gen_ = nullptr;
    uint32_t k_ = 0;
    uint32_t live_ = 0;
};

// ---- the race ----

enum class Burst : uint8_t { Insert, Overwrite, Read, Delete, Mixed, ReadDelete };

struct Weight {
    Burst burst;
    uint8_t weight;  // out of 100
};

// Burst mix by stage: load up, then a general mix, then read/delete churn.
static constexpr Weight LOAD[] = {
    {Burst::Insert, 50}, {Burst::Mixed, 25}, {Burst::Read, 15}, {Burst::Overwrite, 10}, {Burst::Delete, 0},
};
static constexpr Weight MIX[] = {
    {Burst::Mixed, 30}, {Burst::Read, 25}, {Burst::Overwrite, 15}, {Burst::Insert, 15}, {Burst::Delete, 15},
};
static constexpr Weight CHURN[] = {
    {Burst::ReadDelete, 30}, {Burst::Read, 20}, {Burst::Insert, 20}, {Burst::Delete, 15}, {Burst::Mixed, 15},
};

class Racer {
public:
    Racer(const RaceConfig &cfg, RaceResult &r, Keyspace &ks, Histogram *hist, uint8_t *val, uint8_t *got)
        : cfg_(cfg), r_(r), ks_(ks), hist_(hist), val_(val), got_(got), rng_(splitmix64(cfg.seed ^ 0x5A5A5A5Aull))
    {
    }

    void run()
    {
        const int64_t t_start = esp_timer_get_time();
        last_yield_ = t_start;
        while (done_ < cfg_.ops) {
            const Burst b = pick_burst();
            const uint32_t len = 32 + static_cast<uint32_t>(next() % 224);
            for (uint32_t k = 0; k < len && done_ < cfg_.ops; ++k) {
                step(b);
            }
            ++r_.bursts;
        }
        r_.wall_us = esp_timer_get_time() - t_start;
        r_.live_end = ks_.live();
        finish(r_.put, hist_[0]);
        finish(r_.get, hist_[1]);
        finish(r_.del, hist_[2]);
    }

private:
    uint64_t next()
    {
        rng_ = splitmix64(rng_);
        return rng_;
    }

    kv::Key key(uint32_t i) const { return splitmix64((static_cast<uint64_t>(cfg_.seed) << 32) | i); }

    Burst pick_burst()
    {
        const uint64_t pct = static_cast<uint64_t>(done_) * 100 / cfg_.ops;
        const Weight *table = pct < 30 ? LOAD : (pct < 65 ? MIX : CHURN);
        uint32_t roll = static_cast<uint32_t>(next() % 100);
        for (int w = 0; w < 5; ++w) {
            if (roll < table[w].weight) {
                return table[w].burst;
            }
            roll -= table[w].weight;
        }
        return table[0].burst;
    }

    // One op of burst b (two for read+delete), falling back to an insert when
    // there is nothing to read or delete, and to an overwrite when every key
    // already exists.
    void step(Burst b)
    {
        if (b == Burst::Mixed) {
            const uint32_t roll = static_cast<uint32_t>(next() % 100);
            b = roll < 40 ? Burst::Read : (roll < 75 ? (next() & 1 ? Burst::Insert : Burst::Overwrite) : Burst::Delete);
        }
        if (b != Burst::Insert && ks_.live() == 0) {
            b = Burst::Insert;
        }
        if (b == Burst::Insert && ks_.absent() == 0) {
            b = Burst::Overwrite;
        }
        switch (b) {
        case Burst::Insert: put(ks_.pick_absent(next())); break;
        case Burst::Overwrite: put(ks_.pick_live(next())); break;
        case Burst::Read:
            get(next() % 10 == 0 && ks_.absent() ? ks_.pick_absent(next()) : ks_.pick_live(next()));
            break;
        case Burst::Delete: del(ks_.pick_live(next())); break;
        case Burst::ReadDelete: {
            const uint32_t i = ks_.pick_live(next());
            get(i);
            if (done_ < cfg_.ops) {
                del(i);
            }
            break;
        }
        case Burst::Mixed: break;  // resolved above
        }
    }

    void put(uint32_t i)
    {
        maybe_yield();
        const kv::Key k = key(i);
        const uint8_t g = ks_.gen(i) + 1;
        fill_value(val_, cfg_.vsize, k, g);
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = kv::put(k, val_, cfg_.vsize);
        record(r_.put, hist_[0], t0);
        if (err == ESP_OK) {
            ++r_.put.ok;
            ks_.gen(i) = g;
            ks_.set_live(i);
        } else {
            note_err(r_.put, err);
        }
    }

    void get(uint32_t i)
    {
        maybe_yield();
        const kv::Key k = key(i);
        size_t len = 0;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = kv::get(k, got_, kv::max_value_len(), &len);
        record(r_.get, hist_[1], t0);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            note_err(r_.get, err);
        } else if (!ks_.is_live(i)) {
            ++(err == ESP_ERR_NOT_FOUND ? r_.get.ok : r_.get.bad);
        } else if (err == ESP_ERR_NOT_FOUND || len != cfg_.vsize) {
            ++r_.get.bad;
        } else {
            fill_value(val_, cfg_.vsize, k, ks_.gen(i));
            ++(memcmp(val_, got_, cfg_.vsize) == 0 ? r_.get.ok : r_.get.bad);
        }
    }

    // Only called on keys that should exist, so NOT_FOUND is a correctness bug.
    void del(uint32_t i)
    {
        maybe_yield();
        const kv::Key k = key(i);
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = kv::del(k);
        record(r_.del, hist_[2], t0);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            ++(err == ESP_OK ? r_.del.ok : r_.del.bad);
            ks_.set_absent(i);
        } else {
            note_err(r_.del, err);
        }
    }

    void record(RaceOpStats &s, Histogram &h, int64_t t0)
    {
        const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
        h.add(us);
        s.busy_us += us;
        ++s.n;
        ++done_;
    }

    void note_err(RaceOpStats &s, esp_err_t err)
    {
        ++s.err;
        if (r_.first_err == ESP_OK) {
            r_.first_err = err;
        }
    }

    void maybe_yield()
    {
        if (esp_timer_get_time() - last_yield_ > YIELD_EVERY_US) {
            vTaskDelay(1);
            last_yield_ = esp_timer_get_time();
        }
    }

    static void finish(RaceOpStats &s, const Histogram &h)
    {
        s.p50_us = h.percentile(0.50);
        s.p99_us = h.percentile(0.99);
        s.max_us = h.max();
    }

    const RaceConfig &cfg_;
    RaceResult &r_;
    Keyspace &ks_;
    Histogram *hist_;  // put, get, del
    uint8_t *val_;
    uint8_t *got_;
    uint64_t rng_;
    uint32_t done_ = 0;
    int64_t last_yield_ = 0;
};

esp_err_t race(const RaceConfig &cfg_in, RaceResult *out)
{
    RaceConfig cfg = cfg_in;
    if (cfg.keys == 0) {
        cfg.keys = cfg.ops / 4 ? cfg.ops / 4 : 1;
    }
    if (!out || cfg.ops == 0 || cfg.keys > MAX_KEYS || !cfg.every_put || !cfg.every_get || !cfg.every_del) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.vsize == 0 || cfg.vsize > kv::max_value_len()) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!kv::is_open() || kv::key_count() > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!claim()) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = RaceResult{};
    out->keys = cfg.keys;
    Keyspace ks;
    auto *hist = new (std::nothrow) Histogram[3];
    auto *val = static_cast<uint8_t *>(malloc(kv::max_value_len()));
    auto *got = static_cast<uint8_t *>(malloc(kv::max_value_len()));
    esp_err_t err = ESP_OK;
    if (!ks.init(cfg.keys) || !hist || !val || !got) {
        err = ESP_ERR_NO_MEM;
    } else {
        s_every_put = cfg.every_put;
        s_every_get = cfg.every_get;
        s_every_del = cfg.every_del;
        set_phase(RacePhase::Countdown);
        vTaskDelay(pdMS_TO_TICKS(COUNTDOWN_MS));

        const kv::OpCounts base = kv::op_counts();
        s_base_puts = base.puts;
        s_base_gets = base.gets;
        s_base_dels = base.dels;
        set_phase(RacePhase::Running);
        Racer racer(cfg, *out, ks, hist, val, got);
        err = run_in_task([](void *p) { static_cast<Racer *>(p)->run(); }, &racer);
        set_phase(RacePhase::Finished);
    }
    delete[] hist;
    free(val);
    free(got);
    release();
    return err;
}

}  // namespace bench
