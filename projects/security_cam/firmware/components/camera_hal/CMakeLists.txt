if(IDF_TARGET STREQUAL "esp32s3")
    set(SRCS "esp32s3/camera_hal_s3.c")
    set(REQS esp_timer driver espressif__esp32-camera)
elseif(IDF_TARGET STREQUAL "esp32p4")
    set(SRCS "esp32p4/camera_hal_p4.c")
    set(REQS esp_driver_cam esp_driver_isp esp_driver_i2c esp_timer)
else()
    message(FATAL_ERROR "camera_hal: unsupported target '${IDF_TARGET}'. "
                        "Supported: esp32s3, esp32p4")
endif()

idf_component_register(
    SRCS        ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES    ${REQS}
)
