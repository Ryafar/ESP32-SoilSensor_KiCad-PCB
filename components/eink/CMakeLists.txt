# Only the controller for the panel that is actually fitted gets compiled.
#
# This works on SRCS but would NOT work on REQUIRES: component requirements are
# resolved in CMake's early-expansion pass, before Kconfig values exist, so an
# if(CONFIG_...) around a REQUIRES silently takes the wrong branch. SRCS is read
# in the second pass, where CONFIG_ is available.
set(srcs "eink.c" "eink_canvas.c" "epd_bus.c")
if(CONFIG_EINK_PANEL_JD79660)
    list(APPEND srcs "jd79660.c")
else()
    list(APPEND srcs "ssd1681.c")
endif()

# eink.h is the only header anything outside this component may include. The
# controller drivers, the transport and the canvas are private on purpose: the
# application talks to a display, not to an SSD1681.
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES esp_driver_spi esp_driver_gpio)
