cmake_minimum_required(VERSION 3.16)

# Version embedded in esp_app_desc_t — extracted by receiver for OTA display
set(PROJECT_VER "2.0.11")

set(EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tanksync_transmitter)
