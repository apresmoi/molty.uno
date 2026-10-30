# ── Board ────────────────────────────────────────────────────────────────────
# Exactly one folder under device/firmware/boards/ is compiled in: the Kconfig MOLTY_BOARD
# choice (CONFIG_MOLTY_BOARD_DIR). Its board.cmake lists the sources, the -O2 ones,
# include dirs and IDF components. See docs/PORTING.md. Kconfig values do not exist
# yet in IDF's early requirements pass, so that pass skips this; the components come
# from the MOLTY_BOARD_REQUIRES build property (device/firmware/CMakeLists.txt) instead.
idf_build_get_property(MOLTY_BOARD_REQUIRES MOLTY_BOARD_REQUIRES)
set(MOLTY_BOARD_SRCS "")
set(MOLTY_BOARD_FAST_SRCS "")
set(MOLTY_BOARD_INCLUDE_DIRS "")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_MOLTY_BOARD_DIR)
        message(FATAL_ERROR "CONFIG_MOLTY_BOARD_DIR is empty: no board selected")
    endif()
    if(DEFINED MOLTY_BOARD AND NOT MOLTY_BOARD STREQUAL CONFIG_MOLTY_BOARD_DIR)
        message(FATAL_ERROR
            "sdkconfig is for board '${CONFIG_MOLTY_BOARD_DIR}' but -DMOLTY_BOARD=${MOLTY_BOARD}: "
            "its flash/PSRAM settings would be wrong. Build each board with its own sdkconfig "
            "(tools/scripts/firmware-build.sh ${MOLTY_BOARD}).")
    endif()
    get_filename_component(MOLTY_BOARD_PATH "${CMAKE_CURRENT_LIST_DIR}/../boards/${CONFIG_MOLTY_BOARD_DIR}" ABSOLUTE)
    if(NOT EXISTS "${MOLTY_BOARD_PATH}/board.cmake")
        message(FATAL_ERROR "board '${CONFIG_MOLTY_BOARD_DIR}': ${MOLTY_BOARD_PATH}/board.cmake not found")
    endif()
    include("${MOLTY_BOARD_PATH}/board.cmake")   # also re-sets MOLTY_BOARD_REQUIRES (same list)
    list(TRANSFORM MOLTY_BOARD_SRCS PREPEND "${MOLTY_BOARD_PATH}/")
    list(TRANSFORM MOLTY_BOARD_FAST_SRCS PREPEND "${MOLTY_BOARD_PATH}/")
    list(TRANSFORM MOLTY_BOARD_INCLUDE_DIRS PREPEND "${MOLTY_BOARD_PATH}/")
    message(STATUS "molty board: ${CONFIG_MOLTY_BOARD_DIR}")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        # net: USB-C NCM + Wi-Fi + mDNS, the HTTP API, console page + SPIFFS
        "net/network.c"
        "net/http.c"
        "net/console_store.c"
        "net/improv_proto.c"     # Wi-Fi setup over a serial console (boards with MOLTY_CAP_SERIAL)
        "net/improv_serial.c"
        # fluid: the fluid-of-characters creature (+ speed reader, 5x7 font)
        "fluid/mole_fluid.c"
        "fluid/fluid_director.c"  # what is shown when: states, moods, idle life, status
        "fluid/fluid_ask.c"       # a question's sectors + label layout (link ask)
        "fluid/fluid_sim.c"
        "fluid/fluid_target.c"
        "fluid/fluid_draw.c"
        "fluid/glyph_rsvp.c"
        "fluid/glyph_font.c"
        # creature: registry + the .molty cute-kit runtime
        "creature/mole_creature.c"
        "creature/cute_spec.c"
        "creature/cute_rig.c"
        # link: agent link to the relay (WSS, pairing, audio)
        "link/link_protocol.c"
        "link/link_identity.c"
        "link/link_client.c"
        "link/link_cmd.c"
        "link/link_audio.c"
        "link/link_pair.c"
        "link/link_ask.c"        # ask: a question answered by a tap
        # update: firmware self-update (link `update`, /api/update, the mark-valid guard)
        "update/update_core.c"   # the rules (plain C, host-tested)
        "update/self_update.c"
        # audio: listen end-of-speech detection (plain C, host-tested)
        "audio/vad.c"
        "audio/chirp.c"          # the turn acknowledgement blip (link_audio)
        # sense: device events (IMU shake, taps, buttons) for the agent + the fluid
        "sense/gesture.c"
        "sense/sense.c"
        # board: the seam to the hardware (molty_board.h) + the selected board's drivers
        "board/molty_board.c"
        ${MOLTY_BOARD_SRCS}
        # diag: log ring, perf histograms, /api/diag + /api/log
        "diag/mole_diag.c"
        "diag/log_upload.c"     # activity log: job tags, upload filter + frames, persist policy (host-tested)
    INCLUDE_DIRS
        "."
        "net"
        "fluid"
        "creature"
        "link"
        "update"
        "audio"
        "sense"
        "board"
        "diag"
        ${MOLTY_BOARD_INCLUDE_DIRS}
    PRIV_REQUIRES
        esp_wifi
        esp_websocket_client
        esp_http_client  # self-update download
        esp-tls
        mbedtls
        esp_ringbuf
        esp_app_format
        esp_netif
        esp_http_server
        esp_event
        nvs_flash
        lwip
        esp_timer
        esp_hw_support
        esp_psram
        heap
        spi_flash
        spiffs
        json
        app_update
        espcoredump    # GET /api/coredump
        esp_partition  # coredump partition reads
        esp_driver_uart  # Improv Wi-Fi setup on the console UART (net/improv_serial.c)
        ${MOLTY_BOARD_REQUIRES}   # the board's drivers (its board.cmake)
    # Fallback page for "/" when no console is stored in SPIFFS.
    EMBED_TXTFILES
        "net/index.html"
    # Default creature for the fluid's "creature" state (device/creatures/molty/duck.molty).
    # POST /api/creature/file replaces it at runtime without a reflash.
    EMBED_FILES
        "creature/default.molty"
)

# ── Console SPIFFS image ─────────────────────────────────────────────────────
# Ships device/console/dist/index.html in the "spiffs" partition (partitions.csv), so a
# cable flash always lands the current console. net/console_store.c loads
# /spiffs/index.html at boot if /spiffs/index.size matches its exact size;
# stage_console_html.cmake writes that pair at build time in the same format
# the runtime POST /dev/html path writes.
#
# Build the console FIRST (idf.py has no node/pnpm toolchain wired in):
#   cd device/console && pnpm install && pnpm run build     (or from the repo root: make console)
# stage_console_html.cmake fails the build with that instruction if dist/index.html is missing.
set(CONSOLE_SPIFFS_STAGE_DIR "${CMAKE_BINARY_DIR}/console_spiffs_staging")
get_filename_component(CONSOLE_DIST
    "${CMAKE_CURRENT_LIST_DIR}/../../console/dist/index.html" ABSOLUTE)

# Runs at BUILD time, every build (ALL, no OUTPUT), so a `pnpm build` between two
# `idf.py build`s is picked up without reconfiguring.
add_custom_target(stage_console_html ALL
    COMMAND ${CMAKE_COMMAND}
        "-DDEVICE_CONSOLE_DIST=${CONSOLE_DIST}"
        "-DCONSOLE_SPIFFS_STAGE_DIR=${CONSOLE_SPIFFS_STAGE_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/stage_console_html.cmake"
    COMMENT "console: staging dist/index.html -> spiffs image"
    VERBATIM
)

# Partition name "spiffs" must match partitions.csv exactly.
spiffs_create_partition_image(spiffs "${CONSOLE_SPIFFS_STAGE_DIR}"
    FLASH_IN_PROJECT
    DEPENDS stage_console_html
)
add_dependencies(spiffs_spiffs_bin stage_console_html)

# The project builds at -Og (CONFIG_COMPILER_OPTIMIZATION_DEBUG). The fluid's
# per-cell / per-pixel loops and the LCD flush path run every frame, so build
# them at -O2 (≈1.4× on the fluid sim, measured on device). Float divide and
# sqrtf stay libgcc/libm calls on this toolchain; the hot loops avoid them.
set_source_files_properties(
    "fluid/fluid_sim.c"
    "fluid/fluid_target.c"
    "fluid/fluid_draw.c"
    "fluid/mole_fluid.c"
    "creature/cute_rig.c"
    ${MOLTY_BOARD_FAST_SRCS}
    PROPERTIES COMPILE_OPTIONS "-O2;-fno-math-errno"
)
