idf_component_register(SRC_DIRS "." "bringup"
                       INCLUDE_DIRS "." "bringup"
                       REQUIRES rgb bq25186 eink esp_driver_i2c esp_driver_gpio esp_timer esp_adc)
