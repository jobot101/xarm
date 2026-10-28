// ota.h — firmware update over WiFi (POST /api/ota, raw .bin body) and the running build for /api/state.
#pragma once
#include <stddef.h>
#include "esp_http_server.h"

esp_err_t ota_handle(httpd_req_t *r);
void ota_info(char *ver, size_t nv, char *part, size_t np);
