// eye.c — Johnny's eyes: the board's DVP camera, read by the PC. See eye.h.
#include "eye.h"
#include "sdkconfig.h"

#if CONFIG_XARM_JOHNNY
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "store.h"
#include "cam.h"

static const char *TAG = "eye";
static bool s_ok;
static char s_sensor[16] = "none";

// Pins from xiaozhi-esp32 boards/waveshare/esp32-s3-audio-board/config.h. SCCB rides the shared I2C bus (port 0,
// made in main.c), so the camera driver must use the new i2c_master driver too (CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW).
#define CAM_XCLK 43
#define CAM_PCLK 44
#define CAM_VSYNC 21
#define CAM_HREF 1
#define CAM_D0 2
#define CAM_D1 17
#define CAM_D2 18
#define CAM_D3 39
#define CAM_D4 45
#define CAM_D5 46
#define CAM_D6 47
#define CAM_D7 48

#define TCA9555_ADDR 0x20

// xiaozhi's board init wakes the camera with EXIO5 low and EXIO6 high (TCA9555 port 0 bits 5 and 6).
// Read-modify-write, like sd.c: the buttons, the SD card's D3 and the LCD resets share the port.
static void wake(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t d = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA9555_ADDR, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t h;
    if (i2c_master_bus_add_device(bus, &d, &h) != ESP_OK) return;
    uint8_t reg, v;
    reg = 0x02; if (i2c_master_transmit_receive(h, &reg, 1, &v, 1, 50) == ESP_OK) {      // output port 0
        uint8_t w[2] = {0x02, (uint8_t)((v & ~0x20) | 0x40)}; i2c_master_transmit(h, w, 2, 50); }
    reg = 0x06; if (i2c_master_transmit_receive(h, &reg, 1, &v, 1, 50) == ESP_OK) {      // config port 0 (1 = input)
        uint8_t w[2] = {0x06, (uint8_t)(v & ~0x60)}; i2c_master_transmit(h, w, 2, 50); }
    i2c_master_bus_rm_device(h);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t start(pixformat_t fmt)
{
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = CAM_XCLK,
        .pin_sccb_sda = -1, .pin_sccb_scl = -1, .sccb_i2c_port = 0,     // reuse the shared bus
        .pin_d7 = CAM_D7, .pin_d6 = CAM_D6, .pin_d5 = CAM_D5, .pin_d4 = CAM_D4,
        .pin_d3 = CAM_D3, .pin_d2 = CAM_D2, .pin_d1 = CAM_D1, .pin_d0 = CAM_D0,
        .pin_vsync = CAM_VSYNC, .pin_href = CAM_HREF, .pin_pclk = CAM_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_1, .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = fmt,
        .frame_size = FRAMESIZE_QVGA,                                   // 320x240 is plenty to find a block
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,                              // internal RAM is nearly all spoken for
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&c);
}

bool eye_init(i2c_master_bus_handle_t bus)
{
    wake(bus);
    esp_err_t e = start(PIXFORMAT_JPEG);
    if (e == ESP_ERR_NOT_SUPPORTED) e = start(PIXFORMAT_RGB565);        // sensor without JPEG: encode per request
    if (e != ESP_OK) { ESP_LOGW(TAG, "no camera on the ribbon (%s)", esp_err_to_name(e)); return false; }
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        camera_sensor_info_t *i = esp_camera_sensor_get_info(&s->id);
        if (i) strlcpy(s_sensor, i->name, sizeof s_sensor);
    }
    s_ok = true;
    ESP_LOGI(TAG, "camera %s up, waiting for the PC to look (GET /cam.jpg)", s_sensor);
    return true;
}

bool eye_ok(void) { return s_ok; }
const char *eye_sensor(void) { return s_sensor; }

static esp_err_t h_jpg(httpd_req_t *r)
{
    camera_fb_t *fb = s_ok ? esp_camera_fb_get() : NULL;
    if (!fb) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no camera frame"); return ESP_OK; }
    uint8_t *buf = fb->buf;
    size_t len = fb->len;
    bool own = false;
    if (fb->format != PIXFORMAT_JPEG) {
        own = frame2jpg(fb, 70, &buf, &len);
        if (!own) { esp_camera_fb_return(fb); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg encode"); return ESP_OK; }
    }
    httpd_resp_set_type(r, "image/jpeg");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, (const char *)buf, len);
    if (own) free(buf);
    esp_camera_fb_return(fb);
    return e;
}

// {"blobs": [[x,y,w,h] | null, ...]} from pc/eye.py
static esp_err_t h_blobs(httpd_req_t *r)
{
    char body[256];
    int n = r->content_len < (int)sizeof body - 1 ? r->content_len : (int)sizeof body - 1, got = 0;
    while (got < n) { int k = httpd_req_recv(r, body + got, n - got); if (k <= 0) return ESP_FAIL; got += k; }
    body[got] = 0;
    cJSON *j = cJSON_Parse(body), *a = j ? cJSON_GetObjectItem(j, "blobs") : NULL;
    if (cJSON_IsArray(a)) {
        for (int s = 0; s < NSLOT; s++) {
            cJSON *it = cJSON_GetArrayItem(a, s);
            box_t b = {0};
            bool seen = cJSON_IsArray(it) && cJSON_GetArraySize(it) == 4;
            if (seen) {
                b.x = cJSON_GetArrayItem(it, 0)->valueint; b.y = cJSON_GetArrayItem(it, 1)->valueint;
                b.w = cJSON_GetArrayItem(it, 2)->valueint; b.h = cJSON_GetArrayItem(it, 3)->valueint;
                seen = b.w > 0 && b.h > 0;
            }
            cam_feed(s, &b, seen);
        }
    }
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, cJSON_IsArray(a) ? "{\"ok\":true}" : "{\"ok\":false}");
}

void eye_register(httpd_handle_t h)
{
    const httpd_uri_t u[] = {
        {"/cam.jpg",   HTTP_GET,  h_jpg},
        {"/api/blobs", HTTP_POST, h_blobs},
    };
    for (size_t i = 0; i < sizeof u / sizeof *u; i++) httpd_register_uri_handler(h, &u[i]);
}

#else   // Muffin: no ribbon camera
bool eye_init(i2c_master_bus_handle_t bus) { (void)bus; return false; }
bool eye_ok(void) { return false; }
const char *eye_sensor(void) { return "none"; }
void eye_register(httpd_handle_t h) { (void)h; }
#endif
