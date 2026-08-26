#include "battery_monitor.h"
#include "ble_control.h"
#include "esp_err.h"
#include "esp_log.h"
#include "led_controller.h"
#include "sdkconfig.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_ERROR_CHECK(led_controller_init());
    ESP_ERROR_CHECK(battery_monitor_init());
    ESP_ERROR_CHECK(led_controller_start());
    ESP_ERROR_CHECK(ble_control_start());

    ESP_LOGI(TAG, "RGB controller ready; BLE name: %s", CONFIG_BLE_DEVICE_NAME);
}
