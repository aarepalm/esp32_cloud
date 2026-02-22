idf_component_register(
    SRCS        "cloud_client.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        mbedtls
        json
        fatfs
        esp_timer
        esp_hw_support
)
