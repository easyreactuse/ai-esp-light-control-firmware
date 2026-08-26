#include "battery_monitor.h"

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_BATTERY_MONITOR_ENABLE
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#endif

static const char *TAG = "battery";
static volatile bool s_battery_low;

#if CONFIG_BATTERY_MONITOR_ENABLE
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static adc_channel_t s_adc_channel;

static bool calibration_init(adc_unit_t unit, adc_channel_t channel)
{
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t curve_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&curve_config, &s_cali_handle);
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (err != ESP_OK) {
        const adc_cali_line_fitting_config_t line_config = {
            .unit_id = unit,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_line_fitting(&line_config, &s_cali_handle);
    }
#endif

    return err == ESP_OK;
}

static int read_battery_mv(void)
{
    int raw_sum = 0;
    for (int sample = 0; sample < 8; ++sample) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) != ESP_OK) {
            return -1;
        }
        raw_sum += raw;
    }

    int pin_mv = 0;
    if (adc_cali_raw_to_voltage(s_cali_handle, raw_sum / 8, &pin_mv) != ESP_OK) {
        return -1;
    }

    return (pin_mv * CONFIG_BATTERY_DIVIDER_NUMERATOR) /
           CONFIG_BATTERY_DIVIDER_DENOMINATOR;
}

static void battery_task(void *arg)
{
    unsigned low_count = 0;

    while (true) {
        const int battery_mv = read_battery_mv();
        if (battery_mv >= 0) {
            if (!s_battery_low) {
                low_count = battery_mv < CONFIG_BATTERY_LOW_MV
                                ? low_count + 1
                                : 0;
                if (low_count >= 3) {
                    s_battery_low = true;
                    ESP_LOGW(TAG, "low battery: %d mV", battery_mv);
                }
            } else if (battery_mv >= CONFIG_BATTERY_LOW_MV +
                                         CONFIG_BATTERY_HYSTERESIS_MV) {
                s_battery_low = false;
                low_count = 0;
                ESP_LOGI(TAG, "battery recovered: %d mV", battery_mv);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_BATTERY_SAMPLE_INTERVAL_MS));
    }
}
#endif

esp_err_t battery_monitor_init(void)
{
    s_battery_low = false;

#if !CONFIG_BATTERY_MONITOR_ENABLE
    ESP_LOGI(TAG, "monitor disabled; onboard RGB will remain off");
    return ESP_OK;
#else
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(CONFIG_BATTERY_ADC_GPIO,
                                                  &unit,
                                                  &s_adc_channel),
                        TAG,
                        "GPIO %d is not ADC capable",
                        CONFIG_BATTERY_ADC_GPIO);

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &s_adc_handle),
                        TAG,
                        "ADC unit init failed");

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle,
                                                   s_adc_channel,
                                                   &channel_config),
                        TAG,
                        "ADC channel config failed");

    if (!calibration_init(unit, s_adc_channel)) {
        ESP_LOGE(TAG, "ADC calibration unavailable; monitor not started");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (xTaskCreate(battery_task, "battery", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "monitoring GPIO %d; low threshold %d mV",
             CONFIG_BATTERY_ADC_GPIO,
             CONFIG_BATTERY_LOW_MV);
    return ESP_OK;
#endif
}

bool battery_monitor_is_low(void)
{
    return s_battery_low;
}

