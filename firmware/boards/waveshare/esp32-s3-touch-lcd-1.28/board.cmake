# This board's sources, included by main/CMakeLists.txt (paths relative to this
# folder), and by firmware/CMakeLists.txt for MOLTY_BOARD_REQUIRES.
set(MOLTY_BOARD_SRCS
    "board.c"
    "lcd_gc9a01.c"
    "touch_cst816s.c"
    "../../common/qmi8658.c"
)
# Built at -O2 like the fluid: the LCD flush path runs every frame.
set(MOLTY_BOARD_FAST_SRCS
    "lcd_gc9a01.c"
)
set(MOLTY_BOARD_INCLUDE_DIRS "." "../../common")
# IDF components the drivers use (added to main's PRIV_REQUIRES).
set(MOLTY_BOARD_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_adc)
