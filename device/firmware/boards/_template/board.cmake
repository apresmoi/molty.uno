# This board's sources, included by main/CMakeLists.txt (paths relative to this
# folder), and by firmware/CMakeLists.txt for MOLTY_BOARD_REQUIRES.
set(MOLTY_BOARD_SRCS
    "board.c"
    # "lcd_mypanel.c"            your display driver
    # "touch_mychip.c"           your touch driver
    # "../../common/qmi8658.c"   shared drivers live in boards/common/
)
# The display flush runs every frame: build it at -O2 like the fluid.
set(MOLTY_BOARD_FAST_SRCS
    # "lcd_mypanel.c"
)
set(MOLTY_BOARD_INCLUDE_DIRS "." "../../common")
# IDF components your drivers use (added to main's PRIV_REQUIRES).
set(MOLTY_BOARD_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd)
