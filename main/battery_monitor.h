#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t battery_monitor_init(void);
bool battery_monitor_is_low(void);

