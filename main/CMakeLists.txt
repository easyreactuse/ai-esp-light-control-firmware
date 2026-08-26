idf_component_register(
    SRCS "rgb_flow.c"
         "led_controller.c"
         "ble_control.c"
         "battery_monitor.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_adc esp_timer esp_driver_gpio esp_driver_ledc
)
