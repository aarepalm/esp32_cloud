idf_component_register(
    SRCS "button_adc.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_adc
        freertos
)
