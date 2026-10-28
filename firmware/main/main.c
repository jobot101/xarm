// xarm-voice — Waveshare ESP32-S3-AUDIO-Board drives a Hiwonder xArm 1S,
// sees with a Hiwonder ESP32-S3 vision module, listens with ESP-SR, serves a React control app.
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "arm.h"
#include "cam.h"
#include "net.h"
#include "pick.h"
#include "store.h"
#include "update.h"
#include "audio.h"
#include "fx.h"
#include "moves.h"
#include "kids.h"
#include "voice.h"
#include "web.h"

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    store_load();
    moves_init();          // recorded tricks live in the "store" partition
    kids_init();           // who's playing + their best scores

    // one shared bus: ES7210 0x40, ES8311 0x18, TCA9555 0x20, RTC 0x51, vision module 0x52
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_XARM_I2C_SDA,
        .scl_io_num = CONFIG_XARM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));

    audio_init(bus);       // speaker + mics
    fx_start();            // LED ring + robot sounds
    fx_event(update_new_version() ? FX_UPDATED : FX_BOOT);   // fresh firmware: fanfare + "Ready to rumble!"
    arm_init();
    cam_init(bus);
    pick_init();
    net_start();
    // the web UI is up: this build is good. A build that can't serve it rolls back on the next reboot
    if (web_start()) esp_ota_mark_app_valid_cancel_rollback();
    update_start();        // watches GitHub Pages for new builds
    voice_start(bus);      // last: loads models from flash, slowest
}
