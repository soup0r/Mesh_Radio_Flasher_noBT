idf_component_register(
    SRCS "main.c" "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        swd
        power
        web
        nvs_flash
        esp_wifi
        driver
        freertos
        esp_timer
)