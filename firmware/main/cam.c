#include "cam.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CAM_ADDR 0x52
static const char *TAG = "cam";
static i2c_master_dev_handle_t s_dev;
static i2c_master_bus_handle_t s_bus;
static box_t s_box[2];
static bool s_seen[2];
static uint8_t s_raw[2][CAM_BYTES];
static volatile bool s_ok;

static void task(void *arg)
{
    int misses = 0;
    for (;;) {
        // no camera answering for 2 s: stop reading (each failed read floods the log with I2C errors) and
        // just knock quietly every 2 s until one shows up
        if (misses >= 20) {
            if (i2c_master_probe(s_bus, CAM_ADDR, 50) != ESP_OK) { s_ok = false; vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            ESP_LOGI(TAG, "camera answered at 0x52");
            misses = 0;
        }
        bool ok = true;
        for (int slot = 0; slot < 2; slot++) {
            uint8_t reg = (uint8_t)slot, raw[CAM_BYTES];
            if (i2c_master_transmit_receive(s_dev, &reg, 1, raw, CAM_BYTES, 50) != ESP_OK) { ok = false; continue; }
            memcpy(s_raw[slot], raw, CAM_BYTES);
            box_t b;
            s_seen[slot] = cam_decode(raw, &b);
            s_box[slot] = b;
        }
        s_ok = ok;
        misses = ok ? 0 : misses + 1;
        if (misses == 20) ESP_LOGW(TAG, "no camera at 0x52 — checking every 2 s");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Camera switch (NVS "cam_on", default off): with it off the camera is never polled, cam_ok() stays false, the
// app hides everything that needs it, and the router drops picking, sorting and stacking. Applies at boot.
static bool s_enabled;
bool cam_enabled(void) { return s_enabled; }
void cam_enable_save(bool on)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "cam_on", on); nvs_commit(h); nvs_close(h); }
}

esp_err_t cam_init(i2c_master_bus_handle_t bus)
{
    s_bus = bus;                             // the I2C scan works either way
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("xarm", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "cam_on", &v); nvs_close(h); }
    s_enabled = v;
    if (!s_enabled) { ESP_LOGI(TAG, "camera switched off"); return ESP_OK; }
    i2c_device_config_t d = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAM_ADDR,
        .scl_speed_hz = 100000,
    };
    s_bus = bus;
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
        for (int i = 0; i < CAM_BYTES && k + 4 < n; i++)
            k += snprintf(out + k, n - k, "%02X%s", s_raw[s][i], i == CAM_BYTES - 1 && s == 0 ? " | " : " ");
    if (k) out[k - 1] = 0;
}

// who answers on the shared I2C bus — expect 0x18 ES8311, 0x20 TCA9555, 0x40 ES7210, 0x51 RTC, 0x52 camera
int cam_scan(char *out, size_t n)
{
    int found = 0; size_t k = 0;
    out[0] = 0;
    if (!s_bus) return 0;
    for (int a = 0x08; a < 0x78 && k + 6 < n; a++)
        if (i2c_master_probe(s_bus, a, 20) == ESP_OK) { k += snprintf(out + k, n - k, "%s0x%02x", found ? " " : "", a); found++; }
    return found;
}
