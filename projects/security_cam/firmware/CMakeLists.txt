cmake_minimum_required(VERSION 3.16)

# IDF_TARGET is stored in CMakeCache.txt by 'idf.py set-target'.
# After 'idf.py fullclean' the cache is wiped; fall back to reading from
# the existing sdkconfig file (which survives fullclean).
if(NOT IDF_TARGET)
    if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
        file(STRINGS "${CMAKE_SOURCE_DIR}/sdkconfig" _sdkconfig_target
             REGEX "^CONFIG_IDF_TARGET=\"")
        foreach(_line IN LISTS _sdkconfig_target)
            string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"([^\"]+)\".*" "\\1"
                   IDF_TARGET "${_line}")
        endforeach()
    endif()
endif()
if(NOT IDF_TARGET)
    set(IDF_TARGET "esp32s3")
    message(STATUS "IDF_TARGET not set — defaulting to ${IDF_TARGET}")
endif()

# Layer order: common → target-specific → local credentials (gitignored)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.${IDF_TARGET}")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.${IDF_TARGET}")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(security_cam)
