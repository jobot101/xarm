#include "store.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAGIC 0x58415231u     // bump when cfg_t layout changes -> defaults reload
static const char *TAG = "store";
cfg_t g_cfg;
static SemaphoreHandle_t s_mx;

void store_lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
void store_unlock(void) { xSemaphoreGive(s_mx); }

static void defaults(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = MAGIC;
    for (int i = 0; i < NSERVO; i++) { c->home[i] = 500; c->drop[i] = 500; }
    c->drop[5] = 800;                 // base swung to one side
    c->lift[4] = -120;                // shoulder (id5). flip sign in the app if hover dives instead of lifts
    c->grip_open = 200;               // id1 gripper. verify open/close direction with jog
    c->grip_close = 650;
    c->move_ms = 700;
    c->grip_ms = 350;
    strcpy(c->slot_name[0], "red");
    strcpy(c->slot_name[1], "blue");
}

void store_load(void)
{
    s_mx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    size_t len = sizeof g_cfg;
    if (nvs_open("xarm", NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_blob(h, "cfg", &g_cfg, &len);
        nvs_close(h);
        if (e == ESP_OK && len == sizeof g_cfg && g_cfg.magic == MAGIC) {
            ESP_LOGI(TAG, "loaded, %d calib pts", g_cfg.n_calib);
            return;
        }
    }
    ESP_LOGW(TAG, "no saved config, using defaults");
    defaults(&g_cfg);
}

void store_save(void)
{
    nvs_handle_t h;
    store_lock();
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "cfg", &g_cfg, sizeof g_cfg);
        nvs_commit(h);
        nvs_close(h);
    }
    store_unlock();
}
