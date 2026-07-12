idf_component_register(SRCS "main.c"
                        PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_wifi esp_event esp_netif nvs_flash esp_timer)
