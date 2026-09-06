# SRC_DIRS globs the sources at configure time, not at build time. A newly added
# file under main/ or main/bringup/ is invisible until cmake runs again, and the
# symptom is an "undefined reference" at link while the header resolves fine -
# INCLUDE_DIRS names a directory, so headers need no reconfigure. Run
# `idf.py reconfigure` after adding a file, or touch this file.
idf_component_register(SRC_DIRS "." "bringup"
                       INCLUDE_DIRS "." "bringup"
                       REQUIRES rgb bq25186 eink esp_driver_i2c esp_driver_gpio esp_timer esp_adc
                                esp_wifi esp_netif esp_event nvs_flash)
