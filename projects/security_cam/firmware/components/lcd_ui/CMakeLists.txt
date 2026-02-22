idf_component_register(
    SRCS "lcd_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_lcd
        esp_timer
        fatfs
        esp_driver_gpio
        freertos
)
