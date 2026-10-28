#include "net.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static char s_ip[16] = "0.0.0.0";
static const char *s_mode = "none";
static volatile bool s_ap;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && !s_ap) esp_wifi_connect();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof s_ip);
        s_mode = "sta";
        ESP_LOGI(TAG, "connected: http://%s  http://xarm.local", s_ip);
        xEventGroupSetBits(s_ev, 1);
    }
}

static void start_ap(void)
{
    s_ap = true;
    esp_wifi_stop();
    wifi_config_t ap = { .ap = { .ssid = "xarm-voice", .channel = 6, .max_connection = 4 } };
    strlcpy((char *)ap.ap.password, CONFIG_XARM_AP_PASS, sizeof ap.ap.password);
    ap.ap.authmode = strlen(CONFIG_XARM_AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    strcpy(s_ip, "192.168.4.1");
    s_mode = "ap";
    ESP_LOGW(TAG, "AP mode: join wifi 'xarm-voice', open http://192.168.4.1");
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_ps(WIFI_PS_NONE);              // snappier web UI

    if (strlen(CONFIG_XARM_WIFI_SSID)) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, CONFIG_XARM_WIFI_SSID, sizeof sta.sta.ssid);
        strlcpy((char *)sta.sta.password, CONFIG_XARM_WIFI_PASS, sizeof sta.sta.password);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_start();
        if (!(xEventGroupWaitBits(s_ev, 1, false, true, pdMS_TO_TICKS(12000)) & 1)) start_ap();
    } else {
        start_ap();
    }

    mdns_init();
    mdns_hostname_set("xarm");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void net_info(char *ip, size_t n, const char **mode)
{
    strlcpy(ip, s_ip, n);
    *mode = s_mode;
}
