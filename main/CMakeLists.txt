idf_component_register(SRCS "hw_timer.c" "time_sync_demo.c" "time_sync.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi esp_timer nvs_flash esp_event driver)
