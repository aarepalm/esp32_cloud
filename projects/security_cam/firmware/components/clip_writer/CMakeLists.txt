idf_component_register(
    SRCS
        "avi_writer.c"
        "h264_writer.c"
        "clip_writer.c"
    INCLUDE_DIRS "include"
    REQUIRES
        camera_hal
        sdcard
        esp_timer
        fatfs
)
