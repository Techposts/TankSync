cmake_minimum_required(VERSION 3.16)

# ─── Single source of truth for firmware version ──────────────────────────────
# Read ../VERSION (e.g. "2.8.5\n"), strip whitespace, set PROJECT_VER which
# ESP-IDF embeds in esp_app_desc_t.version (offset 0x30 of the .bin) and which
# OTA + MQTT + web UI all read at runtime. ALSO split into MAJOR/MINOR/PATCH
# for the generated version_gen.h header that C code includes.
#
# To bump the version: edit ../VERSION. That's it. No other file.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PROJECT_VER_RAW)
string(STRIP "${PROJECT_VER_RAW}" PROJECT_VER)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _ver_match "${PROJECT_VER}")
if(NOT _ver_match)
    message(FATAL_ERROR
        "VERSION file must contain a semver like '2.8.5' (got: '${PROJECT_VER}')")
endif()
set(PROJECT_VER_MAJOR "${CMAKE_MATCH_1}")
set(PROJECT_VER_MINOR "${CMAKE_MATCH_2}")
set(PROJECT_VER_PATCH "${CMAKE_MATCH_3}")
message(STATUS "TankSync Receiver firmware version: ${PROJECT_VER}")

# Generate version_gen.h into the tanksync_version component's include dir.
# That component (components/tanksync_version/CMakeLists.txt) declares
# INCLUDE_DIRS "include" so any consumer that REQUIRES tanksync_version
# automatically picks up version_gen.h on its compile -I path.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tanksync_version/version_gen.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/tanksync_version/include/version_gen.h"
    @ONLY
)

set(EXTRA_COMPONENT_DIRS "components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tanksync_receiver)
