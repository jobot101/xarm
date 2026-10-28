#include "store.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAGIC 0x58415234u     // bump when cfg_t layout changes -> defaults reload
static const char *TAG = "store";
cfg_t g_cfg;
pick2_t g_pick2 = { .align_axis = 1, .align_turn = 375, .grab_check = 1, .grab_margin = 40, .grab_tries = 3, .size_split = 900 };
int16_t g_reach[NSERVO] = {0, 0, 0, -90, 90, 0};   // elbow out, shoulder forward. flip in Settings if backwards
static SemaphoreHandle_t s_mx;

void store_lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
void store_unlock(void) { xSemaphoreGive(s_mx); }

static void defaults(cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = MAGIC;
    for (int i = 0; i < NSERVO; i++) { c->home[i] = 500; c->drop[i] = 500; }
    c->drop[5] = 800;                 // base swung to one side
    memcpy(c->give, c->home, sizeof c->give);
    memcpy(c->bell, c->home, sizeof c->bell);
    memcpy(c->bin[0], c->home, sizeof c->bin[0]); c->bin[0][5] = 250;   // red bin left
    memcpy(c->bin[1], c->home, sizeof c->bin[1]); c->bin[1][5] = 750;   // blue bin right
    memcpy(c->look, c->home, sizeof c->look);
    c->scan[0] = 0; c->scan[1] = -200; c->scan[2] = 200;
    c->cam_on_head = 1;
    memcpy(c->helper, c->home, sizeof c->helper);
    memcpy(c->tower, c->home, sizeof c->tower); c->tower[5] = 350;
    c->stack_pct = 30;
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
        nvs_handle_t r; size_t rl = sizeof g_reach;
        if (nvs_open("xarm", NVS_READONLY, &r) == ESP_OK) {
            nvs_get_blob(r, "reach", g_reach, &rl);
            pick2_t p2; size_t pl = sizeof p2;
            if (nvs_get_blob(r, "pick2", &p2, &pl) == ESP_OK && pl == sizeof p2) g_pick2 = p2;
            nvs_close(r);
        }
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

void store_reach_save(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "reach", g_reach, sizeof g_reach);
        nvs_commit(h);
        nvs_close(h);
    }
}

void store_pick2_save(void)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "pick2", &g_pick2, sizeof g_pick2);
        nvs_commit(h);
        nvs_close(h);
    }
}
