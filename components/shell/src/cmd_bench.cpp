// cmd_bench.cpp — bench <put|get|del> [name=value ...]
//                 race [name=value ...]
//
// Runs a workload on the board (components/bench) and prints one line:
//   BENCH {"engine":"esp32db","op":"put",...}
// tools/bench.py drives this; the line format is its contract. Options
// (all optional except n):
//   n=<ops> start=<index> range=<indices> keys=seq|rand order=seq|rand
//   vsize=<bytes> seed=<u32> gen=<u32> batch=<ops per transaction>
//   max_s=<seconds> del_below=<index> ow_below=<index> live=<index>
// See bench.hpp for what they mean.
//
// `race` runs the mixed workload in race.hpp with an LED countdown and prints
//   RACE {"engine":"esp32db","wall_us":...,"put":{...},"get":{...},"del":{...}}
// Options: ops= keys= vsize= seed= blink_put= blink_get= blink_del=

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "bench/bench.hpp"
#include "bench/race.hpp"
#include "commands.hpp"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "kv/kv.hpp"
#include "net/net.hpp"

namespace shell {

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno || end == s || *end || v > UINT32_MAX) {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

static bool parse_option(bench::Config &cfg, const char *name, const char *val)
{
    struct U32Opt {
        const char *name;
        uint32_t *field;
    };
    const U32Opt u32_opts[] = {
        {"n", &cfg.n},           {"start", &cfg.start},           {"range", &cfg.range},
        {"vsize", &cfg.vsize},   {"seed", &cfg.seed},             {"gen", &cfg.gen},
        {"batch", &cfg.batch},   {"max_s", &cfg.max_s},           {"del_below", &cfg.expect.del_below},
        {"ow_below", &cfg.expect.ow_below}, {"live", &cfg.expect.live},
    };
    for (const U32Opt &o : u32_opts) {
        if (strcmp(name, o.name) == 0) {
            return parse_u32(val, o.field);
        }
    }
    if (strcmp(name, "keys") == 0 || strcmp(name, "order") == 0) {
        const bool rand = strcmp(val, "rand") == 0;
        if (!rand && strcmp(val, "seq") != 0) {
            return false;
        }
        if (name[0] == 'k') {
            cfg.keys = rand ? bench::KeyMap::Rand : bench::KeyMap::Seq;
        } else {
            cfg.order = rand ? bench::Order::Rand : bench::Order::Seq;
        }
        return true;
    }
    return false;
}

static const char *op_name(bench::Op op)
{
    switch (op) {
    case bench::Op::Put: return "put";
    case bench::Op::Get: return "get";
    case bench::Op::Del: return "del";
    }
    return "?";
}

static int cmd_bench(int argc, char **argv)
{
    bench::Config cfg;
    if (argc < 3) {
        printf("usage: bench <put|get|del> n=<ops> [start= range= keys=seq|rand order=seq|rand vsize= seed= gen= "
               "batch= max_s= del_below= ow_below= live=]\n");
        return 1;
    }
    if (strcmp(argv[1], "put") == 0) {
        cfg.op = bench::Op::Put;
    } else if (strcmp(argv[1], "get") == 0) {
        cfg.op = bench::Op::Get;
    } else if (strcmp(argv[1], "del") == 0) {
        cfg.op = bench::Op::Del;
    } else {
        printf("BENCH_ERR {\"error\":\"unknown op %s\"}\n", argv[1]);
        return 1;
    }
    for (int a = 2; a < argc; ++a) {
        char *eq = strchr(argv[a], '=');
        if (!eq) {
            printf("BENCH_ERR {\"error\":\"expected name=value, got %s\"}\n", argv[a]);
            return 1;
        }
        *eq = '\0';
        if (!parse_option(cfg, argv[a], eq + 1)) {
            printf("BENCH_ERR {\"error\":\"bad option %s=%s\"}\n", argv[a], eq + 1);
            return 1;
        }
    }

    bench::Result r;
    esp_err_t err = bench::run(cfg, &r);
    if (err != ESP_OK) {
        printf("BENCH_ERR {\"error\":\"%s\"}\n", esp_err_to_name(err));
        return 1;
    }
    const double busy_s = r.busy_us / 1e6;
    printf("BENCH {\"engine\":\"%s\",\"op\":\"%s\",\"n\":%" PRIu32 ",\"done\":%" PRIu32 ",\"start\":%" PRIu32
           ",\"keys\":\"%s\",\"order\":\"%s\",\"vsize\":%" PRIu32 ",\"batch\":%" PRIu32 ",\"ok\":%" PRIu32
           ",\"bad\":%" PRIu32 ",\"err\":%" PRIu32 ",\"first_err\":\"%s\",\"first_bad_index\":%" PRId64
           ",\"timed_out\":%s,\"busy_us\":%" PRIu64 ",\"wall_us\":%" PRIu64 ",\"ops_per_s\":%.1f"
           ",\"mean_us\":%.1f,\"min_us\":%" PRIu32 ",\"p50_us\":%" PRIu32 ",\"p90_us\":%" PRIu32
           ",\"p99_us\":%" PRIu32 ",\"p999_us\":%" PRIu32 ",\"max_us\":%" PRIu32
           ",\"free_internal\":%zu,\"free_psram\":%zu,\"wifi\":\"%s\"}\n",
           kv::engine_name(), op_name(cfg.op), cfg.n, r.done, cfg.start,
           cfg.keys == bench::KeyMap::Rand ? "rand" : "seq", cfg.order == bench::Order::Rand ? "rand" : "seq",
           cfg.vsize, cfg.batch, r.ok, r.bad, r.err, esp_err_to_name(r.first_err),
           r.first_bad_index == UINT32_MAX ? static_cast<int64_t>(-1) : static_cast<int64_t>(r.first_bad_index),
           r.timed_out ? "true" : "false", r.busy_us, r.wall_us, busy_s > 0 ? r.done / busy_s : 0.0,
           r.done ? static_cast<double>(r.busy_us) / r.done : 0.0, r.min_us, r.p50_us, r.p90_us, r.p99_us,
           r.p999_us, r.max_us, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM), net::state_name(net::state()));
    return (r.err || r.bad) ? 1 : 0;
}

static void print_op_stats(const char *name, const bench::RaceOpStats &s, bool last)
{
    const double busy_s = s.busy_us / 1e6;
    printf("\"%s\":{\"n\":%" PRIu32 ",\"ok\":%" PRIu32 ",\"bad\":%" PRIu32 ",\"err\":%" PRIu32
           ",\"busy_us\":%" PRIu64 ",\"ops_per_s\":%.1f,\"p50_us\":%" PRIu32 ",\"p99_us\":%" PRIu32
           ",\"max_us\":%" PRIu32 "}%s",
           name, s.n, s.ok, s.bad, s.err, s.busy_us, busy_s > 0 ? s.n / busy_s : 0.0, s.p50_us, s.p99_us,
           s.max_us, last ? "" : ",");
}

static int cmd_race(int argc, char **argv)
{
    bench::RaceConfig cfg;
    struct U32Opt {
        const char *name;
        uint32_t *field;
    };
    const U32Opt opts[] = {
        {"ops", &cfg.ops},   {"keys", &cfg.keys},           {"vsize", &cfg.vsize},         {"seed", &cfg.seed},
        {"blink_put", &cfg.every_put}, {"blink_get", &cfg.every_get}, {"blink_del", &cfg.every_del},
    };
    for (int a = 1; a < argc; ++a) {
        char *eq = strchr(argv[a], '=');
        bool ok = false;
        if (eq) {
            *eq = '\0';
            for (const U32Opt &o : opts) {
                if (strcmp(argv[a], o.name) == 0) {
                    ok = parse_u32(eq + 1, o.field);
                }
            }
        }
        if (!ok) {
            printf("RACE_ERR {\"error\":\"bad option %s (ops= keys= vsize= seed= blink_put= blink_get= "
                   "blink_del=)\"}\n", argv[a]);
            return 1;
        }
    }
    if (!kv::is_open()) {
        printf("RACE_ERR {\"error\":\"database is not open\"}\n");
        return 1;
    }
    if (kv::key_count() > 0) {
        printf("RACE_ERR {\"error\":\"database not empty: run format yes first\"}\n");
        return 1;
    }

    bench::RaceResult r;
    esp_err_t err = bench::race(cfg, &r);
    if (err != ESP_OK) {
        printf("RACE_ERR {\"error\":\"%s\"}\n", esp_err_to_name(err));
        return 1;
    }
    const uint32_t ops = r.put.n + r.get.n + r.del.n;
    printf("RACE {\"engine\":\"%s\",\"ops\":%" PRIu32 ",\"keys\":%" PRIu32 ",\"vsize\":%" PRIu32 ",\"seed\":%" PRIu32
           ",\"wall_us\":%" PRIu64 ",\"ops_per_s\":%.1f,\"bursts\":%" PRIu32 ",\"live_end\":%" PRIu32
           ",\"first_err\":\"%s\",\"wifi\":\"%s\",",
           kv::engine_name(), ops, r.keys, cfg.vsize, cfg.seed,
           r.wall_us, r.wall_us ? ops / (r.wall_us / 1e6) : 0.0, r.bursts, r.live_end, esp_err_to_name(r.first_err),
           net::state_name(net::state()));
    print_op_stats("put", r.put, false);
    print_op_stats("get", r.get, false);
    print_op_stats("del", r.del, true);
    printf("}\n");
    const uint32_t bad = r.put.bad + r.get.bad + r.del.bad, errs = r.put.err + r.get.err + r.del.err;
    return (bad || errs) ? 1 : 0;
}

void register_bench_commands()
{
    esp_console_cmd_t cmd = {};
    cmd.command = "bench";
    cmd.help = "bench <put|get|del> n=<ops> [options]  timed on-device workload (see cmd_bench.cpp)";
    cmd.func = &cmd_bench;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

    cmd.command = "race";
    cmd.help = "race [ops= keys= vsize= seed= blink_put= blink_get= blink_del=]  mixed workload with LED countdown";
    cmd.func = &cmd_race;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace shell
