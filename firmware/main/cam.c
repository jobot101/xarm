#include "cam.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CAM_ADDR 0x52
static const char *TAG = "cam";
static i2c_master_dev_handle_t s_dev;
static box_t s_box[2];
static bool s_seen[2];
static uint8_t s_raw[2][8];
static volatile bool s_ok;

static void task(void *arg)
{
    for (;;) {
        bool ok = true;
        for (int slot = 0; slot < 2; slot++) {
            uint8_t reg = (uint8_t)slot, raw[8];
            if (i2c_master_transmit_receive(s_dev, &reg, 1, raw, 8, 50) != ESP_OK) { ok = false; continue; }
            memcpy(s_raw[slot], raw, 8);
            box_t b;
            s_seen[slot] = cam_decode(raw, &b);
            s_box[slot] = b;
        }
        s_ok = ok;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t cam_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t d = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAM_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t e = i2c_master_bus_add_device(bus, &d, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "add device: %s", esp_err_to_name(e)); return e; }
    if (i2c_master_probe(bus, CAM_ADDR, 100) != ESP_OK) ESP_LOGW(TAG, "no ack at 0x52 — check wiring / color firmware");
    xTaskCreate(task, "cam", 3072, NULL, 4, NULL);
    return ESP_OK;
}

bool cam_get(int slot, box_t *out)
{
    if (slot < 0 || slot > 1) return false;
    *out = s_box[slot];
    return s_ok && s_seen[slot];
}

bool cam_ok(void) { return s_ok; }

void cam_raw_hex(char *out, size_t n)
{
    size_t k = 0;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 8 && k + 4 < n; i++)
            k += snprintf(out + k, n - k, "%02X%s", s_raw[s][i], i == 7 && s == 0 ? " | " : " ");
    if (k) out[k - 1] = 0;
}
