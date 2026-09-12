# Both controller files are listed, but each one's body is behind the #if in
# include/eink_panel.h, so only the fitted panel's driver produces any code.
# CMake cannot read that header, which is why the selection is done by the
# preprocessor rather than here.
set(srcs "eink.c" "eink_canvas.c" "epd_bus.c" "ssd1681.c" "jd79660.c")

# Two public headers: eink.h for the lifecycle and the refresh, eink_canvas.h
# for drawing. The controller drivers and the transport stay private - a caller
# talks to a display, not to an SSD1681 - which is also what keeps this
# component reusable: nothing outside can reach a register.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES esp_driver_spi esp_driver_gpio)
