cmake_minimum_required(VERSION 3.16)

# ─── Single source of truth for firmware version ──────────────────────────────
# Read ./VERSION, set PROJECT_VER (embedded into esp_app_desc.version at flash
# offset 0x30), split into MAJOR/MINOR/PATCH, and generate version_gen.h via
# configure_file. The tanksync_version component then exposes the header.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VER_RAW)
string(STRIP "${PROJECT_VER_RAW}" PROJECT_VER)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _ver_match "${PROJECT_VER}")
if(NOT _ver_match)
    message(FATAL_ERROR
        "VERSION file must contain a semver like '2.0.13' (got: '${PROJECT_VER}')")
endif()
set(PROJECT_VER_MAJOR "${CMAKE_MATCH_1}")
set(PROJECT_VER_MINOR "${CMAKE_MATCH_2}")
set(PROJECT_VER_PATCH "${CMAKE_MATCH_3}")
message(STATUS "TankSync Transmitter firmware version: ${PROJECT_VER}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tanksync_version/version_gen.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tanksync_version/include/version_gen.h"
    @ONLY
)

set(EXTRA_COMPONENT_DIRS "components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tanksync_transmitter)
