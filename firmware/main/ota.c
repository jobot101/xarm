#include "ota.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pick.h"

static const char *TAG = "ota";
#define CHUNK 4096

static esp_err_t reply(httpd_req_t *r, bool ok, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    cJSON_AddStringToObject(j, "msg", msg);
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, s);
    cJSON_free(s);
    cJSON_Delete(j);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));   // let the reply leave first
    esp_restart();
}

// POST /api/ota: stream the image into the inactive slot, make it the boot slot, reboot
esp_err_t ota_handle(httpd_req_t *r)
{
    pick_stop();
    for (int i = 0; i < 50 && pick_busy(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (pick_busy()) return reply(r, false, "busy");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return reply(r, false, "no ota slot");
    if (r->content_len <= 0 || r->content_len > (int)part->size) return reply(r, false, "bad image size");

    esp_ota_handle_t h;
    esp_err_t e = esp_ota_begin(part, r->content_len, &h);
    if (e != ESP_OK) return reply(r, false, "ota begin failed");
    char *buf = malloc(CHUNK);
    if (!buf) { esp_ota_abort(h); return reply(r, false, "out of memory"); }

    for (int left = r->content_len; left > 0 && e == ESP_OK; ) {
        int n = httpd_req_recv(r, buf, left < CHUNK ? left : CHUNK);
        if (n <= 0) { e = ESP_FAIL; break; }
        e = esp_ota_write(h, buf, n);
        left -= n;
    }
    free(buf);

    if (e == ESP_OK) e = esp_ota_end(h);      // validates the image header and checksum
    else esp_ota_abort(h);
    if (e == ESP_OK) e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        char msg[48];
        snprintf(msg, sizeof msg, "update failed: %s", esp_err_to_name(e));
        ESP_LOGE(TAG, "%s", msg);
        return reply(r, false, msg);
    }

    ESP_LOGW(TAG, "updated to slot %s, rebooting", part->label);
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return reply(r, true, "updated — rebooting");
}

void ota_info(char *ver, size_t nv, char *part, size_t np)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    strlcpy(ver, esp_app_get_description()->version, nv);
    strlcpy(part, p ? p->label : "?", np);
}
