# stage_console_html.cmake — build-time staging step for the console
# SPIFFS image (see the "Console SPIFFS image" block in main/CMakeLists.txt).
#
# Run via `cmake -P` from an ALL custom target (not at configure time) so it
# re-stages on every `idf.py build`, picking up whatever is CURRENTLY in
# console/dist/index.html even if cmake was never reconfigured —
# same reasoning as spiffs_create_partition_image()'s own image-generation
# target (components/spiffs/project_include.cmake): "this always executes as
# there is no way to specify for CMake to watch for contents of the base dir
# changing."
#
# Inputs (passed via -D on the command line):
#   DEVICE_CONSOLE_DIST      — absolute path to console/dist/index.html
#   CONSOLE_SPIFFS_STAGE_DIR — absolute path to the staging directory that
#                              becomes the SPIFFS image's base_dir
#
# Produces, inside CONSOLE_SPIFFS_STAGE_DIR:
#   index.html — verbatim copy of the built console
#   index.size — the EXACT-SIZE ASCII marker spiffs_load_html() in
#                net/console_store.c requires: the decimal byte count of
#                index.html, written with NO trailing newline. This must
#                match spiffs_write_size_marker()'s on-device format
#                (snprintf(digits, sizeof(digits), "%u", (unsigned)value),
#                writing exactly that many bytes, no newline) byte-for-byte —
#                spiffs_load_html() deletes the whole pair on ANY mismatch
#                (missing marker, unparseable marker, or a marker value that
#                disagrees with the real on-disk file size), so it would
#                silently discard a shipped image whose marker doesn't match
#                the same convention the runtime POST /dev/html path writes.

if(NOT DEFINED DEVICE_CONSOLE_DIST OR NOT DEFINED CONSOLE_SPIFFS_STAGE_DIR)
    message(FATAL_ERROR "stage_console_html.cmake: DEVICE_CONSOLE_DIST and CONSOLE_SPIFFS_STAGE_DIR must both be set")
endif()

if(NOT EXISTS "${DEVICE_CONSOLE_DIST}")
    message(FATAL_ERROR
"\n"
"================================================================================\n"
" console/dist/index.html not found:\n"
"   ${DEVICE_CONSOLE_DIST}\n"
"\n"
" The firmware SPIFFS image ships whatever is CURRENTLY built there — idf.py\n"
" has no pnpm/node toolchain wired in to build it for you. Build the console\n"
" first, then re-run idf.py build:\n"
"\n"
"   cd console && pnpm install && pnpm run build\n"
"\n"
" (or from the repo root: make console)\n"
"================================================================================\n"
    )
endif()

file(MAKE_DIRECTORY "${CONSOLE_SPIFFS_STAGE_DIR}")

# file(COPY) copies content-if-different (not just mtime), so a `pnpm build`
# that reproduces byte-identical output is a no-op here, and any real change
# is picked up every time regardless of cmake's configure-time cache.
file(COPY "${DEVICE_CONSOLE_DIST}" DESTINATION "${CONSOLE_SPIFFS_STAGE_DIR}")

set(STAGED_HTML "${CONSOLE_SPIFFS_STAGE_DIR}/index.html")
file(SIZE "${STAGED_HTML}" CONSOLE_HTML_SIZE)

# NO trailing newline: file(WRITE) writes the string verbatim with nothing
# appended, matching spiffs_write_size_marker()'s snprintf("%u", ...) which
# also writes no newline.
file(WRITE "${CONSOLE_SPIFFS_STAGE_DIR}/index.size" "${CONSOLE_HTML_SIZE}")

message(STATUS "console: staged ${CONSOLE_HTML_SIZE} bytes (index.html + index.size marker) -> ${CONSOLE_SPIFFS_STAGE_DIR}")
