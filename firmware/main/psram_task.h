// psram_task.h — on the MotherShip/MotherShip build, internal RAM is the scarce thing (~400 KB, most of it taken
// by WiFi, the AFE and the web server). Tasks that only move data and never write flash get their stack in PSRAM.
// Muffin keeps every stack internal (its tasks run router commands that write NVS).
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

static inline BaseType_t task_psram(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                    TaskHandle_t *out, BaseType_t core)
{
#if CONFIG_XARM_MOTHERSHIP
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r == pdPASS) return r;
#endif
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, core);
}
