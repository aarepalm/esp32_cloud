idf_component_register(
    SRCS        "sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_sdmmc
        sdmmc
        fatfs
        vfs
)
