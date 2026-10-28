#include "moves.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "moves";
#define PART "store"
#define NS   "moves"

static char s_names[MOVE_MAX][16];          // "" = empty slot
static uint16_t (*s_rec)[NSERVO];           // recording buffer (heap)
static volatile int s_nrec;
static volatile bool s_recording;

static bool open_ns(nvs_handle_t *h, bool rw)
{
    return nvs_open_from_partition(PART, NS, rw ? NVS_READWRITE : NVS_READONLY, h) == ESP_OK;
}

void moves_init(void)
{
    esp_err_t e = nvs_flash_init_partition(PART);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(PART);
        e = nvs_flash_init_partition(PART);
    }
    if (e != ESP_OK) { ESP_LOGE(TAG, "no '%s' partition: %s", PART, esp_err_to_name(e)); return; }
    nvs_handle_t h;
    if (!open_ns(&h, false)) return;
    for (int i = 0; i < MOVE_MAX; i++) {
        char k[8]; size_t n = sizeof s_names[i];
        snprintf(k, sizeof k, "n%d", i);
        if (nvs_get_str(h, k, s_names[i], &n) != ESP_OK) s_names[i][0] = 0;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d saved moves", moves_count());
}

static void rec_task(void *arg)
{
    while (s_recording) {
        uint16_t p[NSERVO];
        if (s_nrec < MOVE_FRAMES && arm_read(p)) memcpy(s_rec[s_nrec++], p, sizeof p);
        if (s_nrec >= MOVE_FRAMES) break;           // full: keep what we have, wait for save
        vTaskDelay(pdMS_TO_TICKS(1000 / MOVE_HZ));
    }
    while (s_recording) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelete(NULL);
}

bool moves_record_start(void)
{
    if (s_recording) return false;
    if (!s_rec) s_rec = malloc(sizeof(uint16_t[MOVE_FRAMES][NSERVO]));
    if (!s_rec) return false;
    s_nrec = 0;
    arm_torque(false);
    s_recording = true;
    xTaskCreate(rec_task, "rec", 3072, NULL, 4, NULL);
    return true;
}

void moves_record_cancel(void) { s_recording = false; arm_torque(true); }

bool moves_record_save(const char *name)
{
    int n = s_nrec;
    s_recording = false;
    arm_torque(true);
    if (n < 2 || !name || !name[0]) return false;
    int slot = -1;
    for (int i = 0; i < MOVE_MAX; i++) if (!strcasecmp(s_names[i], name)) { slot = i; break; }   // same name: overwrite
    for (int i = 0; slot < 0 && i < MOVE_MAX; i++) if (!s_names[i][0]) slot = i;
    if (slot < 0) return false;
    nvs_handle_t h;
    if (!open_ns(&h, true)) return false;
    char kn[8], kd[8];
    snprintf(kn, sizeof kn, "n%d", slot); snprintf(kd, sizeof kd, "d%d", slot);
    bool ok = nvs_set_blob(h, kd, s_rec, n * sizeof s_rec[0]) == ESP_OK && nvs_set_str(h, kn, name) == ESP_OK
              && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) strlcpy(s_names[slot], name, sizeof s_names[slot]);
    ESP_LOGI(TAG, "saved '%s' (%d frames) in slot %d: %s", name, n, slot, ok ? "ok" : "FAILED");
    return ok;
}

bool moves_recording(int *frames) { if (frames) *frames = s_nrec; return s_recording; }

int moves_count(void) { int n = 0; for (int i = 0; i < MOVE_MAX; i++) n += s_names[i][0] != 0; return n; }

const char *moves_name(int i) { return (i >= 0 && i < MOVE_MAX) ? s_names[i] : ""; }

int moves_find(const char *text)
{
    for (int i = 0; i < MOVE_MAX; i++) if (s_names[i][0] && strstr(text, s_names[i])) return i;
    return -1;
}

int moves_load(int i, uint16_t (*frames)[NSERVO], int max)
{
    if (i < 0 || i >= MOVE_MAX || !s_names[i][0]) return 0;
    nvs_handle_t h;
    if (!open_ns(&h, false)) return 0;
    char kd[8]; snprintf(kd, sizeof kd, "d%d", i);
    size_t len = max * sizeof frames[0];
    int n = nvs_get_blob(h, kd, frames, &len) == ESP_OK ? (int)(len / sizeof frames[0]) : 0;
    nvs_close(h);
    return n;
}

bool moves_delete(int i)
{
    if (i < 0 || i >= MOVE_MAX || !s_names[i][0]) return false;
    nvs_handle_t h;
    if (!open_ns(&h, true)) return false;
    char kn[8], kd[8];
    snprintf(kn, sizeof kn, "n%d", i); snprintf(kd, sizeof kd, "d%d", i);
    nvs_erase_key(h, kn); nvs_erase_key(h, kd);
    nvs_commit(h); nvs_close(h);
    s_names[i][0] = 0;
    return true;
}
