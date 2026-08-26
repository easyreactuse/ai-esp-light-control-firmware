#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t led_controller_init(void);
esp_err_t led_controller_start(void);
esp_err_t led_controller_handle_command(const char *json,
                                        char *response,
                                        size_t response_size);
