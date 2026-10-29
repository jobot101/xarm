#include "net.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static char s_ip[16] = "0.0.0.0";
static char s_ssid[33], s_pass[65];
static const char *s_mode = "none";
static volatile bool s_ap, s_online;     // s_ap: home WiFi gave up, backup network only

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_online = false;
        if (!s_ap) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof s_ip);
        s_mode = "sta";
        s_online = true;
        ESP_LOGI(TAG, "connected to %s: http://%s  http://muffin  http://muffin.local  (backup: wifi 'Muffin' -> http://192.168.4.1)", s_ssid, s_ip);
        xEventGroupSetBits(s_ev, 1);
    }
}

// The board's own network "Muffin" (http://192.168.4.1). It stays up next to home WiFi, so any
// phone — iPhone or Android — always has one address that works, no router or mDNS needed.
static void ap_config(void)
{
    wifi_config_t ap = { .ap = { .ssid = "Muffin", .channel = 6, .max_connection = 4 } };
    strlcpy((char *)ap.ap.password, CONFIG_XARM_AP_PASS, sizeof ap.ap.password);
    ap.ap.authmode = strlen(CONFIG_XARM_AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
}

// home WiFi failed: drop the station side (its retries would keep hopping the AP's channel)
static void start_ap(void)
{
    s_ap = true;
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);
    ap_config();
    esp_wifi_start();
    strcpy(s_ip, "192.168.4.1");
    s_mode = "ap";
    ESP_LOGW(TAG, "AP mode: join wifi 'Muffin', open http://192.168.4.1");
}

// creds saved from the web app win; menuconfig values are the fallback
static void load_creds(void)
{
    strlcpy(s_ssid, CONFIG_XARM_WIFI_SSID, sizeof s_ssid);
    strlcpy(s_pass, CONFIG_XARM_WIFI_PASS, sizeof s_pass);
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_ssid;
    char ssid[33];
    if (nvs_get_str(h, "ssid", ssid, &n) == ESP_OK && ssid[0]) {
        strlcpy(s_ssid, ssid, sizeof s_ssid);
        n = sizeof s_pass;
        if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    }
    nvs_close(h);
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t *sta_if = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    // DHCP hostname: most home routers then answer http://muffin (or muffin.lan / muffin.home) for every
    // phone. That's the Android path, since Android often can't resolve muffin.local.
    esp_netif_set_hostname(sta_if, "muffin");
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_ps(WIFI_PS_NONE);              // snappier web UI

    load_creds();
    if (s_ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, s_ssid, sizeof sta.sta.ssid);
        strlcpy((char *)sta.sta.password, s_pass, sizeof sta.sta.password);
        esp_wifi_set_mode(WIFI_MODE_APSTA);       // home WiFi + the backup network at once
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        ap_config();
        esp_wifi_start();
        if (!(xEventGroupWaitBits(s_ev, 1, false, true, pdMS_TO_TICKS(15000)) & 1)) {
            ESP_LOGW(TAG, "couldn't join '%s'", s_ssid);
            start_ap();
        }
    } else {
        start_ap();
    }

    mdns_init();
    mdns_hostname_set("muffin");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

bool net_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK
              && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void net_info(char *ip, size_t n, const char **mode)
{
    strlcpy(ip, s_ip, n);
    *mode = s_mode;
}

void net_ssid(char *out, size_t n) { strlcpy(out, s_ssid, n); }
bool net_online(void) { return s_online; }
