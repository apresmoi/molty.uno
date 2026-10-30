# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Product version: the repo-root VERSION file is the one source of truth (see
# tools/scripts/version.mjs). PROJECT_VER becomes esp_app_get_description()->version,
# which /api/info reports as "fw" — instead of ESP-IDF's default `git describe`
# (which reads "-dirty" on any local edit and differs between clones).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" PROJECT_VER LIMIT_COUNT 1)
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "VERSION must start with X.Y.Z, got '${PROJECT_VER}'")
endif()
# Re-run cmake when VERSION changes, so a bump is never baked in stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")

# Board: a folder under boards/ (docs/PORTING.md). Pick one with
#   idf.py -DMOLTY_BOARD=waveshare/esp32-s3-touch-lcd-1.28 -B build/<name> -DSDKCONFIG=build/<name>/sdkconfig build
# or tools/scripts/firmware-build.sh <board>. Plain `idf.py build` is the Waveshare 1.46.
# The board's sdkconfig.board (flash, PSRAM, console, its Kconfig MOLTY_BOARD choice)
# is layered over sdkconfig.defaults; main/CMakeLists.txt then compiles that board's
# sources and refuses an sdkconfig made for another board.
set(MOLTY_BOARD "waveshare/esp32-s3-touch-lcd-1.46" CACHE STRING "Board folder under device/firmware/boards/")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${MOLTY_BOARD}/board.json")
    message(FATAL_ERROR "MOLTY_BOARD '${MOLTY_BOARD}': no device/firmware/boards/${MOLTY_BOARD}/board.json")
endif()
if(NOT DEFINED SDKCONFIG_DEFAULTS AND NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${MOLTY_BOARD}/sdkconfig.board")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The board's IDF components (its board.cmake MOLTY_BOARD_REQUIRES), as a build
# property: main/CMakeLists.txt needs them in IDF's early requirements pass, where
# Kconfig values do not exist yet.
function(molty_board_requires out)
    set(MOLTY_BOARD_REQUIRES "")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/${MOLTY_BOARD}/board.cmake")
    set(${out} "${MOLTY_BOARD_REQUIRES}" PARENT_SCOPE)
endfunction()
molty_board_requires(_molty_board_requires)
idf_build_set_property(MOLTY_BOARD_REQUIRES "${_molty_board_requires}")
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(molty)
