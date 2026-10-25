idf_component_register(
    SRCS
        "app_main.c"
        # net: USB-C NCM + Wi-Fi + mDNS, the HTTP API, console page + SPIFFS
        "net/network.c"
        "net/http.c"
        "net/console_store.c"
        # fluid: the fluid-of-characters creature (+ speed reader, 5x7 font)
        "fluid/mole_fluid.c"
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
        # hw: board drivers
        "hw/mole_lcd.c"
        "hw/esp_lcd_spd2010.c"
        "hw/mole_touch.c"
        "hw/mole_imu.c"
        "hw/mole_tca9554.c"
        "hw/mole_audio.c"
        # diag: log ring, perf histograms, /api/diag + /api/log
        "diag/mole_diag.c"
    INCLUDE_DIRS
        "."
        "net"
        "fluid"
        "creature"
        "link"
        "hw"
        "diag"
    PRIV_REQUIRES
        esp_wifi
        esp_websocket_client
        mbedtls
        esp_ringbuf
        esp_app_format
        esp_netif
        esp_http_server
        esp_event
        nvs_flash
        lwip
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_lcd
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
    # Fallback page for "/" when no console is stored in SPIFFS.
    EMBED_TXTFILES
        "net/index.html"
    # Default creature for the fluid's "creature" state (creatures/molty/duck.molty).
    # POST /api/creature/file replaces it at runtime without a reflash.
    EMBED_FILES
        "creature/default.molty"
)

# ── Console SPIFFS image ─────────────────────────────────────────────────────
# Ships console/dist/index.html in the "spiffs" partition (partitions.csv), so a
# cable flash always lands the current console. net/console_store.c loads
# /spiffs/index.html at boot if /spiffs/index.size matches its exact size;
# stage_console_html.cmake writes that pair at build time in the same format
# the runtime POST /dev/html path writes.
#
# Build the console FIRST (idf.py has no node/pnpm toolchain wired in):
#   cd console && pnpm install && pnpm run build     (or from the repo root: make console)
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
    "hw/mole_lcd.c"
    "hw/esp_lcd_spd2010.c"
    PROPERTIES COMPILE_OPTIONS "-O2;-fno-math-errno"
)
