# Only the half selected by RUN_BRINGUP in main.c is compiled. The switch is
# read out of the source rather than duplicated here, because two switches that
# have to agree eventually disagree - and the failure mode is an image that
# contains the bring-up tasks it was supposed to have shed.
set(MAIN_SOURCE "${CMAKE_CURRENT_LIST_DIR}/main.c")

# Flipping the switch has to re-run cmake: SRC_DIRS globs at configure time, so
# without this the build would keep compiling the previous half from a stale
# glob and the switch would look broken.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MAIN_SOURCE}")

file(READ "${MAIN_SOURCE}" main_source_text)
if(NOT main_source_text MATCHES "#define[ \t]+RUN_BRINGUP[ \t]+([01])")
    message(FATAL_ERROR
            "main.c has no '#define RUN_BRINGUP 0' or '#define RUN_BRINGUP 1' - "
            "the build cannot tell which half of the firmware to compile.")
endif()
if(CMAKE_MATCH_1)
    set(FIRMWARE_HALF "bringup")
else()
    set(FIRMWARE_HALF "app")
endif()
message(STATUS "Firmware half: main/${FIRMWARE_HALF}/ (RUN_BRINGUP=${CMAKE_MATCH_1})")

# secrets.h is gitignored, so a fresh clone does not have one. Caught here with
# a message that says what to do, rather than as a missing-header error in the
# middle of app_net.c. Only the application half needs it - the bring-up checks
# bring their own WiFi scan with no credentials.
if(FIRMWARE_HALF STREQUAL "app" AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR
            "main/secrets.h is missing. Copy main/secrets.h.example to "
            "main/secrets.h and fill in your WiFi and MQTT details.")
endif()

# A newly added file under main/ or under the selected half is invisible until
# cmake runs again, and the symptom is an "undefined reference" at link while
# the header resolves fine - INCLUDE_DIRS names a directory, so headers need no
# reconfigure. Run `idf.py reconfigure` after adding a file, or touch this file.
#
# REQUIRES covers both halves. Listing a component the running half never calls
# costs nothing in the image - nothing references it, so the linker leaves it
# out - and trimming the list per half would be one more thing to keep in sync.
idf_component_register(SRC_DIRS "." "${FIRMWARE_HALF}"
                       INCLUDE_DIRS "." "${FIRMWARE_HALF}"
                       REQUIRES rgb bq25186 battery soil eink
                                esp_driver_i2c esp_driver_gpio esp_timer esp_adc
                                esp_wifi esp_netif esp_event nvs_flash espressif__mqtt)
