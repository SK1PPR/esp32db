#pragma once
// shell.hpp — start the REPL. esp_console runs it in its own FreeRTOS task,
// so app_main can keep doing other things (LEDs, later networking).

#include "esp_err.h"

namespace shell {

esp_err_t start();

}  // namespace shell
