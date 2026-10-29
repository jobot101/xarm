# sounds.bin + sounds_gen.h are rendered by tools/make_sounds.py (CI does it before every build)
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/sounds.bin OR NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/sounds_gen.h)
    message(FATAL_ERROR "sound bank missing: run  python3 tools/make_sounds.py  (or --no-voice to test the build)")
endif()
idf_component_register(
    SRCS main.c core.c store.c arm.c cam.c pick.c router.c voice.c net.c web.c ota.c update.c audio.c fx.c moves.c kids.c
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_uart esp_driver_i2c esp_driver_i2s esp_http_server
             esp_wifi esp_netif esp_event esp_timer json app_update esp_app_format
             esp_http_client esp_https_ota mbedtls
    EMBED_FILES www/index.html.gz sounds.bin)
