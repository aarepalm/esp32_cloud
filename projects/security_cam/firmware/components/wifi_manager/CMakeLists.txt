if(IDF_TARGET STREQUAL "esp32s3")
    set(SRCS "esp32s3/wifi_manager_s3.c")
    set(REQS esp_wifi esp_event esp_netif)
elseif(IDF_TARGET STREQUAL "esp32p4")
    set(SRCS "esp32p4/wifi_manager_p4.c")
    # Phase 2: add esp_hosted here + esp_wifi esp_event esp_netif
    # For now the stub only needs log
    set(REQS log)
else()
    message(FATAL_ERROR "wifi_manager: unsupported target '${IDF_TARGET}'")
endif()

idf_component_register(
    SRCS        ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES    ${REQS}
)
