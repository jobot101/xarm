idf_component_register(
    SRCS main.c core.c store.c arm.c cam.c pick.c router.c voice.c net.c web.c ota.c
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_uart esp_driver_i2c esp_driver_i2s esp_http_server
             esp_wifi esp_netif esp_event esp_timer json app_update esp_app_format
    EMBED_FILES www/index.html.gz)
