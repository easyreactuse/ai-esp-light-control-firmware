#include "led_controller.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "battery_monitor.h"
#include "cJSON.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "sdkconfig.h"

typedef enum {
    MODE_OFF,
    MODE_SOLID,
    MODE_CHASE,
    MODE_FLOW,
} light_mode_t;

typedef enum {
    TRAFFIC_EFFECT_STEADY,
    TRAFFIC_EFFECT_BLINK,
    TRAFFIC_EFFECT_BREATHE,
} traffic_effect_t;

typedef struct {
    light_mode_t mode;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness;
    uint32_t rgb_blink_ms;
    uint8_t traffic_lights;
    uint32_t traffic_blink_ms;
    traffic_effect_t traffic_effect;
    uint32_t traffic_period_ms;
    uint32_t step_ms;
    int64_t rgb_changed_at_ms;
    int64_t traffic_changed_at_ms;
    uint32_t revision;
} light_state_t;

#define TRAFFIC_RED    (1U << 0)
#define TRAFFIC_YELLOW (1U << 1)
#define TRAFFIC_GREEN  (1U << 2)
#define OUTPUT_8_BIT_RGB    (1U << 0)
#define OUTPUT_TRAFFIC_LIGHT (1U << 1)
#define TRAFFIC_PWM_MAX_DUTY ((1U << LEDC_TIMER_13_BIT) - 1U)
#define TRAFFIC_DEFAULT_BREATHE_PERIOD_MS 1800U

static const ledc_channel_t s_traffic_channels[] = {
    LEDC_CHANNEL_0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
};

static const char *TAG = "lights";
static led_strip_handle_t s_onboard;
static led_strip_handle_t s_external;
static SemaphoreHandle_t s_state_lock;
static light_state_t s_state;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint8_t scale_channel(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness + 50) / 100);
}

static uint32_t flow_speed_to_step_ms(uint8_t speed)
{
    return 220U - (uint32_t)speed * 2U;
}

static void hsv_to_rgb(uint16_t hue, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const uint16_t sector = (hue % 360) / 60;
    const uint16_t offset = (hue % 60) * 255 / 60;
    const uint8_t rising = (uint8_t)offset;
    const uint8_t falling = (uint8_t)(255 - offset);

    switch (sector) {
    case 0: *red = 255; *green = rising; *blue = 0; break;
    case 1: *red = falling; *green = 255; *blue = 0; break;
    case 2: *red = 0; *green = 255; *blue = rising; break;
    case 3: *red = 0; *green = falling; *blue = 255; break;
    case 4: *red = rising; *green = 0; *blue = 255; break;
    default: *red = 255; *green = 0; *blue = falling; break;
    }
}

static esp_err_t new_strip(int gpio, uint32_t count, led_strip_handle_t *handle)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, handle);
}

static void render_external(const light_state_t *state, int64_t timestamp_ms)
{
    if (state->mode == MODE_OFF ||
        (state->mode == MODE_SOLID && state->rgb_blink_ms > 0 &&
         ((timestamp_ms - state->rgb_changed_at_ms) /
          state->rgb_blink_ms) % 2 != 0)) {
        ESP_ERROR_CHECK(led_strip_clear(s_external));
        return;
    }

    if (state->mode == MODE_SOLID) {
        const uint8_t r = scale_channel(state->red, state->brightness);
        const uint8_t g = scale_channel(state->green, state->brightness);
        const uint8_t b = scale_channel(state->blue, state->brightness);
        for (uint32_t led = 0; led < CONFIG_EXTERNAL_LED_COUNT; ++led) {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_external, led, r, g, b));
        }
    } else if (state->mode == MODE_CHASE) {
        const uint32_t frame = (timestamp_ms - state->rgb_changed_at_ms) /
                               state->step_ms;
        for (uint32_t led = 0; led < CONFIG_EXTERNAL_LED_COUNT; ++led) {
            uint8_t r, g, b;
            const uint16_t hue = (uint16_t)(((led + frame) * 360U /
                                              CONFIG_EXTERNAL_LED_COUNT) % 360U);
            hsv_to_rgb(hue, &r, &g, &b);
            ESP_ERROR_CHECK(led_strip_set_pixel(s_external, led,
                                                scale_channel(r, state->brightness),
                                                scale_channel(g, state->brightness),
                                                scale_channel(b, state->brightness)));
        }
    } else {
        const uint32_t frame = (timestamp_ms - state->rgb_changed_at_ms) /
                               state->step_ms;
        uint8_t r, g, b;
        hsv_to_rgb((uint16_t)(frame % 360U), &r, &g, &b);
        r = scale_channel(r, state->brightness);
        g = scale_channel(g, state->brightness);
        b = scale_channel(b, state->brightness);
        for (uint32_t led = 0; led < CONFIG_EXTERNAL_LED_COUNT; ++led) {
            ESP_ERROR_CHECK(led_strip_set_pixel(s_external, led, r, g, b));
        }
    }
    ESP_ERROR_CHECK(led_strip_refresh(s_external));
}

static uint32_t traffic_breathe_duty(const light_state_t *state,
                                     int64_t timestamp_ms)
{
    const uint32_t period = state->traffic_period_ms;
    const uint32_t elapsed = (uint32_t)((timestamp_ms -
                                         state->traffic_changed_at_ms) % period);
    const uint32_t half = period / 2U;
    const uint32_t ramp = elapsed < half ? elapsed : period - elapsed;
    const uint32_t x = ramp * 1024U / half;
    // Integer smoothstep removes the sharp corners of a triangular PWM ramp.
    const uint32_t smooth = (uint32_t)(((uint64_t)x * x *
                                        (3072U - 2U * x)) /
                                       (1024U * 1024U));
    return (uint32_t)(((uint64_t)TRAFFIC_PWM_MAX_DUTY * smooth) / 1024U);
}

static void render_traffic_light(const light_state_t *state, int64_t timestamp_ms)
{
    uint32_t duty = TRAFFIC_PWM_MAX_DUTY;
    if (state->traffic_effect == TRAFFIC_EFFECT_BLINK) {
        const bool visible = ((timestamp_ms - state->traffic_changed_at_ms) /
                              state->traffic_blink_ms) % 2 == 0;
        duty = visible ? TRAFFIC_PWM_MAX_DUTY : 0;
    } else if (state->traffic_effect == TRAFFIC_EFFECT_BREATHE) {
        duty = traffic_breathe_duty(state, timestamp_ms);
    }

    const uint8_t masks[] = { TRAFFIC_RED, TRAFFIC_YELLOW, TRAFFIC_GREEN };
    for (size_t i = 0; i < sizeof(s_traffic_channels) /
                            sizeof(s_traffic_channels[0]); ++i) {
        const uint32_t channel_duty = (state->traffic_lights & masks[i])
                                          ? duty
                                          : 0;
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                      s_traffic_channels[i], channel_duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,
                                         s_traffic_channels[i]));
    }
}

static void render_onboard(int64_t timestamp_ms)
{
    if (battery_monitor_is_low() &&
        (timestamp_ms / CONFIG_LOW_BATTERY_BLINK_MS) % 2 == 0) {
        const uint8_t red = scale_channel(255, CONFIG_LOW_BATTERY_LED_BRIGHTNESS);
        ESP_ERROR_CHECK(led_strip_set_pixel(s_onboard, 0, red, 0, 0));
        ESP_ERROR_CHECK(led_strip_refresh(s_onboard));
    } else {
        ESP_ERROR_CHECK(led_strip_clear(s_onboard));
    }
}

static void led_task(void *arg)
{
    (void)arg;
    light_state_t state;
    while (true) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        state = s_state;
        xSemaphoreGive(s_state_lock);

        const int64_t timestamp_ms = now_ms();
        render_external(&state, timestamp_ms);
        render_traffic_light(&state, timestamp_ms);
        render_onboard(timestamp_ms);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool parse_traffic_lights(const cJSON *root, uint8_t *lights)
{
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "light");
    if (!cJSON_IsArray(array)) {
        return false;
    }

    uint8_t result = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || item->valuestring == NULL) {
            return false;
        }
        if (strcmp(item->valuestring, "RED") == 0) {
            result |= TRAFFIC_RED;
        } else if (strcmp(item->valuestring, "YELLOW") == 0) {
            result |= TRAFFIC_YELLOW;
        } else if (strcmp(item->valuestring, "GREEN") == 0) {
            result |= TRAFFIC_GREEN;
        } else {
            return false;
        }
    }
    *lights = result;
    return true;
}

static bool json_int(const cJSON *root, const char *name, int min, int max,
                     bool required, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (item == NULL) {
        return !required;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
        item->valueint < min || item->valueint > max) {
        return false;
    }
    *value = item->valueint;
    return true;
}

static esp_err_t fail(char *response, size_t size, const char *message)
{
    snprintf(response, size, "{\"ok\":false,\"error\":\"%s\"}", message);
    return ESP_ERR_INVALID_ARG;
}

static const char *parse_command(const cJSON *root, light_state_t *next,
                                 uint8_t *outputs)
{
    if (!cJSON_IsObject(root)) {
        return "command must be an object";
    }

    const cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    bool is_traffic_light = false;
    if (output != NULL) {
        if (!cJSON_IsString(output) || output->valuestring == NULL) {
            return "output must be a string";
        }
        if (strcmp(output->valuestring, "8_BIT_RGB") == 0) {
            is_traffic_light = false;
        } else if (strcmp(output->valuestring, "TRAFFIC_LIGHT") == 0) {
            is_traffic_light = true;
        } else {
            return "unknown output";
        }
    }

    if (is_traffic_light) {
        int blink_ms = 0;
        int period_ms = TRAFFIC_DEFAULT_BREATHE_PERIOD_MS;
        uint8_t lights = 0;
        if (!json_int(root, "blink_ms", 0, 60000, false, &blink_ms) ||
            !json_int(root, "period_ms", 400, 20000, false, &period_ms) ||
            !parse_traffic_lights(root, &lights)) {
            return "invalid traffic light parameter";
        }
        traffic_effect_t effect = blink_ms > 0 ? TRAFFIC_EFFECT_BLINK
                                               : TRAFFIC_EFFECT_STEADY;
        const cJSON *effect_item = cJSON_GetObjectItemCaseSensitive(root,
                                                                    "effect");
        if (effect_item != NULL) {
            if (!cJSON_IsString(effect_item) || effect_item->valuestring == NULL) {
                return "traffic effect must be a string";
            }
            if (strcmp(effect_item->valuestring, "steady") == 0) {
                effect = TRAFFIC_EFFECT_STEADY;
            } else if (strcmp(effect_item->valuestring, "blink") == 0) {
                if (blink_ms == 0) {
                    return "blink effect requires blink_ms";
                }
                effect = TRAFFIC_EFFECT_BLINK;
            } else if (strcmp(effect_item->valuestring, "breathe") == 0) {
                effect = TRAFFIC_EFFECT_BREATHE;
            } else {
                return "unknown traffic effect";
            }
        }
        next->traffic_lights = lights;
        next->traffic_blink_ms = blink_ms;
        next->traffic_effect = effect;
        next->traffic_period_ms = period_ms;
        *outputs |= OUTPUT_TRAFFIC_LIGHT;
        return NULL;
    }

    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(command) || command->valuestring == NULL) {
        return "cmd must be a string";
    }
    if (strcmp(command->valuestring, "off") == 0) {
        next->mode = MODE_OFF;
    } else if (strcmp(command->valuestring, "solid") == 0) {
        int r = 0, g = 0, b = 0;
        int brightness = next->brightness;
        int blink_ms = 0;
        if (!json_int(root, "r", 0, 255, true, &r) ||
            !json_int(root, "g", 0, 255, true, &g) ||
            !json_int(root, "b", 0, 255, true, &b) ||
            !json_int(root, "brightness", 0, 100, true, &brightness) ||
            !json_int(root, "blink_ms", 0, 60000, false, &blink_ms)) {
            return "invalid solid parameter";
        }
        next->mode = MODE_SOLID;
        next->red = r;
        next->green = g;
        next->blue = b;
        next->brightness = brightness;
        next->rgb_blink_ms = blink_ms;
    } else if (strcmp(command->valuestring, "chase") == 0) {
        int brightness = CONFIG_DEFAULT_EXTERNAL_BRIGHTNESS;
        int step_ms = CONFIG_DEFAULT_CHASE_STEP_MS;
        if (!json_int(root, "brightness", 0, 100, false, &brightness) ||
            !json_int(root, "step_ms", 20, 5000, false, &step_ms)) {
            return "invalid chase parameter";
        }
        next->mode = MODE_CHASE;
        next->brightness = brightness;
        next->step_ms = step_ms;
    } else if (strcmp(command->valuestring, "flow") == 0) {
        int brightness = CONFIG_DEFAULT_EXTERNAL_BRIGHTNESS;
        int speed = CONFIG_DEFAULT_FLOW_SPEED;
        if (!json_int(root, "brightness", 0, 100, false, &brightness) ||
            !json_int(root, "speed", 1, 100, false, &speed)) {
            return "invalid flow parameter";
        }
        next->mode = MODE_FLOW;
        next->brightness = brightness;
        next->step_ms = flow_speed_to_step_ms((uint8_t)speed);
    } else {
        return "unknown cmd";
    }
    *outputs |= OUTPUT_8_BIT_RGB;
    return NULL;
}

esp_err_t led_controller_handle_command(const char *json, char *response,
                                        size_t response_size)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return fail(response, response_size, "invalid JSON");
    }

    light_state_t next;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    next = s_state;
    xSemaphoreGive(s_state_lock);

    const bool is_batch = cJSON_IsArray(root);
    uint8_t outputs = 0;
    int command_count = 1;
    const char *error = NULL;
    if (is_batch) {
        command_count = cJSON_GetArraySize(root);
        if (command_count == 0) {
            error = "command array must not be empty";
        } else {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, root) {
                error = parse_command(item, &next, &outputs);
                if (error != NULL) {
                    break;
                }
            }
        }
    } else {
        error = parse_command(root, &next, &outputs);
    }
    if (error != NULL) {
        cJSON_Delete(root);
        return fail(response, response_size, error);
    }

    const int64_t changed_at_ms = now_ms();
    if ((outputs & OUTPUT_8_BIT_RGB) != 0) {
        next.rgb_changed_at_ms = changed_at_ms;
    }
    if ((outputs & OUTPUT_TRAFFIC_LIGHT) != 0) {
        next.traffic_changed_at_ms = changed_at_ms;
    }
    ++next.revision;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    s_state = next;
    xSemaphoreGive(s_state_lock);

    if (is_batch) {
        snprintf(response, response_size,
                 "{\"ok\":true,\"count\":%d,\"outputs\":%u}",
                 command_count, outputs);
        ESP_LOGI(TAG, "BLE command batch: %d commands, outputs=%u",
                 command_count, outputs);
    } else if ((outputs & OUTPUT_TRAFFIC_LIGHT) != 0) {
        snprintf(response, response_size,
                 "{\"ok\":true,\"output\":\"TRAFFIC_LIGHT\"}");
        ESP_LOGI(TAG, "BLE output: TRAFFIC_LIGHT");
    } else {
        const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        snprintf(response, response_size, "{\"ok\":true,\"mode\":\"%s\"}",
                 command->valuestring);
        ESP_LOGI(TAG, "BLE command: %s", command->valuestring);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t led_controller_init(void)
{
    if (CONFIG_RGB_LED_GPIO == CONFIG_EXTERNAL_LED_GPIO) {
        ESP_LOGE(TAG, "onboard and external LEDs cannot share GPIO %d",
                 CONFIG_RGB_LED_GPIO);
        return ESP_ERR_INVALID_ARG;
    }
    const int traffic_gpios[] = {
        CONFIG_TRAFFIC_RED_GPIO,
        CONFIG_TRAFFIC_YELLOW_GPIO,
        CONFIG_TRAFFIC_GREEN_GPIO,
    };
    for (size_t i = 0; i < sizeof(traffic_gpios) / sizeof(traffic_gpios[0]); ++i) {
        if (traffic_gpios[i] == CONFIG_RGB_LED_GPIO ||
            traffic_gpios[i] == CONFIG_EXTERNAL_LED_GPIO) {
            ESP_LOGE(TAG, "traffic light GPIO %d conflicts with an RGB output",
                     traffic_gpios[i]);
            return ESP_ERR_INVALID_ARG;
        }
#if CONFIG_BATTERY_MONITOR_ENABLE
        if (traffic_gpios[i] == CONFIG_BATTERY_ADC_GPIO) {
            ESP_LOGE(TAG, "traffic light GPIO %d conflicts with battery ADC",
                     traffic_gpios[i]);
            return ESP_ERR_INVALID_ARG;
        }
#endif
        for (size_t j = i + 1;
             j < sizeof(traffic_gpios) / sizeof(traffic_gpios[0]); ++j) {
            if (traffic_gpios[i] == traffic_gpios[j]) {
                ESP_LOGE(TAG, "traffic light GPIOs must be unique");
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    s_state_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_state = (light_state_t){
        .mode = MODE_OFF,
        .brightness = CONFIG_DEFAULT_EXTERNAL_BRIGHTNESS,
        .step_ms = CONFIG_DEFAULT_CHASE_STEP_MS,
        .traffic_effect = TRAFFIC_EFFECT_STEADY,
        .traffic_period_ms = TRAFFIC_DEFAULT_BREATHE_PERIOD_MS,
        .rgb_changed_at_ms = now_ms(),
        .traffic_changed_at_ms = now_ms(),
    };
    ESP_RETURN_ON_ERROR(new_strip(CONFIG_RGB_LED_GPIO, 1, &s_onboard), TAG,
                        "onboard LED init failed");
    ESP_RETURN_ON_ERROR(new_strip(CONFIG_EXTERNAL_LED_GPIO,
                                  CONFIG_EXTERNAL_LED_COUNT, &s_external), TAG,
                        "external LED init failed");
    const ledc_timer_config_t traffic_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&traffic_timer), TAG,
                        "traffic light PWM timer init failed");
    const int traffic_pwm_gpios[] = {
        CONFIG_TRAFFIC_RED_GPIO,
        CONFIG_TRAFFIC_YELLOW_GPIO,
        CONFIG_TRAFFIC_GREEN_GPIO,
    };
    for (size_t i = 0; i < sizeof(s_traffic_channels) /
                            sizeof(s_traffic_channels[0]); ++i) {
        const ledc_channel_config_t traffic_channel = {
            .gpio_num = traffic_pwm_gpios[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = s_traffic_channels[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&traffic_channel), TAG,
                            "traffic light PWM channel init failed");
    }
    ESP_RETURN_ON_ERROR(led_strip_clear(s_onboard), TAG, "onboard clear failed");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_external), TAG, "external clear failed");
    return ESP_OK;
}

esp_err_t led_controller_start(void)
{
    return xTaskCreate(led_task, "led_control", 4096, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
