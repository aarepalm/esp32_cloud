idf_component_register(
    SRCS        "boot_console.c"
    INCLUDE_DIRS "include"
    REQUIRES
        sdcard
        nvs_flash
        esp_timer
        esp_hw_support
        spi_flash
        heap
        freertos
        vfs
        fatfs
        esp_driver_usb_serial_jtag
)
