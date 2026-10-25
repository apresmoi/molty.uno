# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Product version: the repo-root VERSION file is the one source of truth (see
# scripts/version.mjs). PROJECT_VER becomes esp_app_get_description()->version,
# which /api/info reports as "fw" — instead of ESP-IDF's default `git describe`
# (which reads "-dirty" on any local edit and differs between clones).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PROJECT_VER LIMIT_COUNT 1)
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "VERSION must start with X.Y.Z, got '${PROJECT_VER}'")
endif()
# Re-run cmake when VERSION changes, so a bump is never baked in stale.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../VERSION")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(molty)
