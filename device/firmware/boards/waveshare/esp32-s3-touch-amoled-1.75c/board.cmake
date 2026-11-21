# This board's sources, included by main/CMakeLists.txt (paths relative to this
# folder), and by device/firmware/CMakeLists.txt for MOLTY_BOARD_REQUIRES.
set(MOLTY_BOARD_SRCS
    "board.c"
    "lcd_co5300.c"
    "lcd_q.c"
    "touch_cst9217.c"
    "audio_es.c"
    "../../common/qmi8658.c"
)
# Built at -O2 like the fluid: the LCD flush path runs every frame.
set(MOLTY_BOARD_FAST_SRCS
    "lcd_co5300.c"
    "lcd_q.c"
)
set(MOLTY_BOARD_INCLUDE_DIRS "." "../../common")
# IDF components the drivers use (added to main's PRIV_REQUIRES). esp_codec_dev is a
# managed component (main/idf_component.yml): the ES8311 / ES7210 register drivers.
set(MOLTY_BOARD_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_codec_dev)
