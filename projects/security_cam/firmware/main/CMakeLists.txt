idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        freertos
        camera_hal
        wifi_manager
        clip_writer
        motion_detect
        cloud_client
        sdcard
        boot_console
        lcd_ui
        button_adc
)
