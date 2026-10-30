#include "cam.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
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

// ---- wireless link: the camera joins Muffin's own WiFi ("Muffin" AP) and sends a UDP packet per frame to
// the board's AP address (cam/xarm_color/muffin_link.cpp). Packet = "MUF1", u16 seq, slot0 color, slot1 color,
// 5 x (x, y, w, h). Fresh wireless data wins; with none for 500 ms the I2C reading is used.
#define CAM_UDP_PORT 4747
static box_t s_wbox[2];
static bool s_wseen[2];
static uint8_t s_wraw[2][CAM_BYTES];
static volatile int64_t s_wlast_us;
static char s_wfrom[16];

static bool wifi_fresh(void) { return s_wlast_us && esp_timer_get_time() - s_wlast_us < 500000; }

static void udp_task(void *arg)
{
    int sock = -1;
    for (;;) {
        if (sock < 0) {                       // the network stack starts after us: keep trying
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(CAM_UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
            if (sock < 0 || bind(sock, (struct sockaddr *)&a, sizeof a) < 0) {
                if (sock >= 0) close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            ESP_LOGI(TAG, "listening for the wireless camera on UDP %d", CAM_UDP_PORT);
        }
        uint8_t p[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(sock, p, sizeof p, 0, (struct sockaddr *)&from, &fl);
        if (n < 8 + 5 * CAM_BYTES || memcmp(p, "MUF1", 4)) continue;
        for (int slot = 0; slot < 2; slot++) {
            int c = p[6 + slot];
            if (c > 4) continue;
            const uint8_t *raw = p + 8 + c * CAM_BYTES;
            memcpy(s_wraw[slot], raw, CAM_BYTES);
            box_t b;
            s_wseen[slot] = cam_decode(raw, &b);
            s_wbox[slot] = b;
        }
        if (!s_wlast_us) {
            inet_ntoa_r(from.sin_addr, s_wfrom, sizeof s_wfrom);
            ESP_LOGI(TAG, "wireless camera connected from %s", s_wfrom);
        }
        s_wlast_us = esp_timer_get_time();
    }
}

static void task(void *arg)
{
    int misses = 0;
    for (;;) {
        // no camera answering for 2 s: stop reading (each failed read floods the log with I2C errors) and
        // just knock quietly every 2 s until one shows up
        if (misses >= 20) {
            if (wifi_fresh() || i2c_master_probe(s_bus, CAM_ADDR, 50) != ESP_OK) { s_ok = false; vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
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

esp_err_t cam_init(i2c_master_bus_handle_t bus)
{
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
    xTaskCreate(udp_task, "cam_udp", 3072, NULL, 4, NULL);
    return ESP_OK;
}

bool cam_get(int slot, box_t *out)
{
    if (slot < 0 || slot > 1) return false;
    if (wifi_fresh()) { *out = s_wbox[slot]; return s_wseen[slot]; }
    *out = s_box[slot];
    return s_ok && s_seen[slot];
}

bool cam_ok(void) { return wifi_fresh() || s_ok; }
const char *cam_link(void) { return wifi_fresh() ? "wifi" : s_ok ? "wired" : "none"; }

void cam_raw_hex(char *out, size_t n)
{
    size_t k = 0;
    uint8_t (*raw)[CAM_BYTES] = wifi_fresh() ? s_wraw : s_raw;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < CAM_BYTES && k + 4 < n; i++)
            k += snprintf(out + k, n - k, "%02X%s", raw[s][i], i == CAM_BYTES - 1 && s == 0 ? " | " : " ");
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
