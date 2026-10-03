#include <atomic>
#include <cinttypes>
#include <initializer_list>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_psram.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "bench/bench.hpp"
#include "bench/race.hpp"
#include "kv/kv.hpp"
#include "mem/mem.hpp"
#include "net/net.hpp"
#include "shell/shell.hpp"

// Status LEDs:
//   LED1 heartbeat  short blink every second: firmware is alive
//   LED2 WiFi       off: no credentials | fast blink: connecting |
//                   solid: connected | slow blink: failing (still retrying)
//   LED3 storage    solid: a benchmark is running | fast blink: database failed to open
// On top of that, each kv operation flashes one LED (inverts it for 60 ms):
//   put -> LED1, get -> LED2, del -> LED3
// A steady stream of ops (bench) shows as a 10 Hz flicker over the status pattern.
//
// During a `race` (bench/race.hpp) the status patterns are off and:
//   green           3 blinks (3-2-1), on for 1 s at GO, on for 3 s at the finish
//   LED1/2/3        one blink per blink_put / blink_get / blink_del ops
// After the finish everything stays dark until the next race or a reboot.
constexpr gpio_num_t LED_HEARTBEAT = GPIO_NUM_40;
constexpr gpio_num_t LED_WIFI = GPIO_NUM_41;
constexpr gpio_num_t LED_STORAGE = GPIO_NUM_42;
constexpr gpio_num_t LED_GREEN = GPIO_NUM_39;

constexpr uint32_t HIGH = 1;
constexpr uint32_t LOW = 0;

#if CONFIG_KV_ENGINE_ESP32DB
// RAM left free when the database balloons into the rest. It covers memory
// allocated *while running* (shell buffers, log replay, new tasks, WiFi
// packets). Raise it if anything starts failing to allocate.
constexpr size_t SRAM_RESERVE = 160 * 1024;   // task stacks, DMA, WiFi/BT internal buffers
constexpr size_t PSRAM_RESERVE = 512 * 1024;  // large mallocs, lwIP/WiFi buffers in PSRAM
#endif

static std::atomic<bool> s_storage_failed{false};

constexpr int TICK_MS = 20;

// Turns an event counter into flashes: 3 ticks inverted, then 2 ticks of
// plain level before the next flash can start, so continuous activity still
// blinks (at most 10 Hz) instead of just inverting the LED.
class OpFlash {
public:
    uint32_t apply(uint32_t count, uint32_t level)
    {
        if (ticks_ == 0 && count != seen_) {
            seen_ = count;
            ticks_ = 5;
        }
        if (ticks_ == 0) {
            return level;
        }
        return ticks_-- > 2 ? !level : level;
    }

private:
    uint32_t seen_ = 0;
    int ticks_ = 0;
};

static void show_race(const bench::RaceView &race, const kv::OpCounts &ops, OpFlash flash[3])
{
    uint32_t green = LOW;
    switch (race.phase) {
    case bench::RacePhase::Countdown: green = race.phase_ms % 1000 < 300; break;
    case bench::RacePhase::Running: green = race.phase_ms < 1000; break;
    case bench::RacePhase::Finished: green = race.phase_ms < 3000; break;
    case bench::RacePhase::Idle: break;
    }
    gpio_set_level(LED_GREEN, green);

    if (race.phase != bench::RacePhase::Running) {
        for (int i = 0; i < 3; ++i) {
            flash[i] = OpFlash{};  // the next race starts counting from 0
        }
        gpio_set_level(LED_HEARTBEAT, LOW);
        gpio_set_level(LED_WIFI, LOW);
        gpio_set_level(LED_STORAGE, LOW);
        return;
    }
    gpio_set_level(LED_HEARTBEAT, flash[0].apply((ops.puts - race.base.puts) / race.every_put, LOW));
    gpio_set_level(LED_WIFI, flash[1].apply((ops.gets - race.base.gets) / race.every_get, LOW));
    gpio_set_level(LED_STORAGE, flash[2].apply((ops.dels - race.base.dels) / race.every_del, LOW));
}

static void status_task(void *)
{
    OpFlash put_flash, get_flash, del_flash;
    OpFlash race_flash[3];
    for (uint32_t t = 0;; ++t) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        const uint32_t ms = t * TICK_MS;
        const kv::OpCounts ops = kv::op_counts();

        const bench::RaceView race = bench::race_view();
        if (race.phase != bench::RacePhase::Idle) {
            show_race(race, ops, race_flash);
            continue;
        }

        gpio_set_level(LED_HEARTBEAT, put_flash.apply(ops.puts, ms % 1000 < 100 ? HIGH : LOW));

        uint32_t wifi = LOW;
        switch (net::state()) {
        case net::State::Connecting: wifi = ms % 250 < 125; break;
        case net::State::Connected: wifi = HIGH; break;
        case net::State::Failing: wifi = ms % 2000 < 1000; break;
        default: break;
        }
        gpio_set_level(LED_WIFI, get_flash.apply(ops.gets, wifi));

        uint32_t storage = LOW;
        if (s_storage_failed) {
            storage = ms % 200 < 100;
        } else if (bench::running()) {
            storage = HIGH;
        }
        gpio_set_level(LED_STORAGE, del_flash.apply(ops.dels, storage));
    }
}

extern "C" void app_main(void)
{
    char *ourTaskName = pcTaskGetName(NULL);

    ESP_LOGI(ourTaskName, "Hello starting up! engine: %s", kv::engine_name());
    ESP_LOGI(ourTaskName, "The size of psram is: %zu bytes ", esp_psram_get_size());

    for (gpio_num_t led : {LED_HEARTBEAT, LED_WIFI, LED_STORAGE, LED_GREEN}) {
        gpio_reset_pin(led);
        gpio_set_direction(led, GPIO_MODE_OUTPUT);
    }
    xTaskCreate(status_task, "status", 2048, nullptr, tskIDLE_PRIORITY + 1, nullptr);

    // WiFi allocates its buffers here, so it has to come before the balloon.
    esp_err_t err = net::start();
    if (err != ESP_OK) {
        ESP_LOGE(ourTaskName, "WiFi failed to start: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(shell::start());  // REPL runs in its own task

#if CONFIG_KV_ENGINE_ESP32DB
    // Last: hand all RAM that's still free (minus the reserves) to the
    // database, then open it on top of that memory.
    err = mem::balloon(SRAM_RESERVE, PSRAM_RESERVE);
    if (err != ESP_OK) {
        ESP_LOGE(ourTaskName, "balloon failed: %s", esp_err_to_name(err));
    }
#endif
    // Don't abort on a failed open: data on flash that can't be replayed would
    // otherwise reboot-loop the board. The shell stays up so it can be
    // inspected or wiped with `format yes`.
    const int64_t t0 = esp_timer_get_time();
    err = kv::open();
    const int64_t open_us = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        s_storage_failed = true;
        ESP_LOGE(ourTaskName, "database failed to open: %s (shell still available, `format yes` wipes it)",
                 esp_err_to_name(err));
    }
    // tools/bench.py waits for this line after every boot; open_us is the
    // mount + replay cost for whatever is on flash.
    printf("READY {\"engine\":\"%s\",\"ok\":%s,\"err\":\"%s\",\"open_us\":%" PRId64 ",\"keys\":%" PRId64 "}\n",
           kv::engine_name(), err == ESP_OK ? "true" : "false", esp_err_to_name(err), open_us, kv::key_count());
}
