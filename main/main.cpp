#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_psram.h"
#include "esp_err.h"
#include "driver/gpio.h"

#include "db/db.hpp"
#include "shell/shell.hpp"

constexpr gpio_num_t LED1 = GPIO_NUM_40;
constexpr gpio_num_t LED2 = GPIO_NUM_41;
constexpr gpio_num_t LED3 = GPIO_NUM_42;

constexpr uint32_t HIGH = 1;
constexpr uint32_t LOW = 0;

void led_blink(int ms)
{
    gpio_set_level(LED1, HIGH);
    vTaskDelay(ms / portTICK_PERIOD_MS);
    gpio_set_level(LED1, LOW);
    gpio_set_level(LED2, HIGH);
    vTaskDelay(ms / portTICK_PERIOD_MS);
    gpio_set_level(LED2, LOW);
    gpio_set_level(LED3, HIGH);
    vTaskDelay(ms / portTICK_PERIOD_MS);
    gpio_set_level(LED3, LOW);
}

extern "C" void app_main(void)
{
    char *ourTaskName = pcTaskGetName(NULL);

    ESP_LOGI(ourTaskName, "Hello starting up!");
    ESP_LOGI(ourTaskName, "The size of psram is: %zu bytes ", esp_psram_get_size());

    gpio_reset_pin(LED1);
    gpio_reset_pin(LED2);
    gpio_reset_pin(LED3);
    gpio_set_direction(LED1, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED2, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED3, GPIO_MODE_OUTPUT);

    // Don't abort on a failed open: data on flash that can't be replayed would
    // otherwise reboot-loop the board. Keep the shell up so it can be
    // inspected or wiped with `format yes`.
    esp_err_t err = db::open();
    if (err != ESP_OK) {
        ESP_LOGE(ourTaskName, "database failed to open: %s (shell still available, `format yes` wipes it)",
                 esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(shell::start());  // REPL runs in its own task

    while (1)
    {
        led_blink(1000);
        led_blink(500);
    }

}
