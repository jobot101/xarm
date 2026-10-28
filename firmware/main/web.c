#include "web.h"
#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "arm.h"
#include "cam.h"
#include "net.h"
#include "ota.h"
#include "update.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pick.h"
#include "router.h"
#include "store.h"
#include "voice.h"

static const char *TAG = "web";
extern const uint8_t app_start[] asm("_binary_index_html_gz_start");
extern const uint8_t app_end[]   asm("_binary_index_html_gz_end");
static float s_mark_x, s_mark_y;   // calibration: pixel marked in step 1
static bool s_mark_ok;

// ---------- helpers ----------
static esp_err_t send_json(httpd_req_t *r, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, s);
    cJSON_free(s);
    cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t reply(httpd_req_t *r, bool ok, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    cJSON_AddStringToObject(j, "msg", msg);
    return send_json(r, j);
}

static cJSON *body(httpd_req_t *r)
{
    char buf[1024];
    int len = r->content_len < (int)sizeof buf - 1 ? r->content_len : (int)sizeof buf - 1, got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, buf + got, len - got);
        if (n <= 0) return NULL;
        got += n;
    }
    buf[got] = 0;
    return cJSON_Parse(buf);
}

static int num(cJSON *o, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static cJSON *u16arr(const uint16_t *a, int n)
{
    cJSON *x = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(x, cJSON_CreateNumber(a[i]));
    return x;
}

static void read_arr(cJSON *o, const char *k, void *dst, bool sign)
{
    cJSON *a = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != NSERVO) return;
    for (int i = 0; i < NSERVO; i++) {
        int v = cJSON_GetArrayItem(a, i)->valueint;
        if (sign) ((int16_t *)dst)[i] = (int16_t)v; else ((uint16_t *)dst)[i] = clamp_pos(v);
    }
}

// ---------- GET /api/state  (polled ~5 Hz by the app) ----------
static esp_err_t h_state(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    char st[16], det[64], ip[16], buf[64];
    const char *mode;
    pick_status(st, sizeof st, det, sizeof det);
    cJSON_AddStringToObject(j, "state", st);
    cJSON_AddStringToObject(j, "detail", det);
    cJSON_AddBoolToObject(j, "busy", pick_busy());
    cJSON_AddBoolToObject(j, "holding", pick_holding());

    uint16_t p[NSERVO];
    arm_last(p);
    cJSON *arm = cJSON_AddObjectToObject(j, "arm");
    cJSON_AddItemToObject(arm, "pos", u16arr(p, NSERVO));
    cJSON_AddBoolToObject(arm, "torque", arm_torque_on());
    cJSON_AddBoolToObject(arm, "link", arm_link_ok());
    cJSON_AddNumberToObject(arm, "battery_mv", arm_battery_mv());

    cJSON *cam = cJSON_AddObjectToObject(j, "cam");
    cJSON_AddBoolToObject(cam, "ok", cam_ok());
    cam_raw_hex(buf, sizeof buf);
    cJSON_AddStringToObject(cam, "raw", buf);
    cJSON *slots = cJSON_AddArrayToObject(cam, "slots");
    for (int s = 0; s < NSLOT; s++) {
        box_t b; bool seen = cam_get(s, &b);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", g_cfg.slot_name[s]);
        cJSON_AddBoolToObject(o, "seen", seen);
        cJSON_AddNumberToObject(o, "x", b.x); cJSON_AddNumberToObject(o, "y", b.y);
        cJSON_AddNumberToObject(o, "w", b.w); cJSON_AddNumberToObject(o, "h", b.h);
        cJSON_AddItemToArray(slots, o);
    }

    cJSON_AddBoolToObject(j, "marked", s_mark_ok);
    cJSON *cal = cJSON_AddArrayToObject(j, "calib");
    for (int i = 0; i < g_cfg.n_calib; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "px", g_cfg.calib[i].px);
        cJSON_AddNumberToObject(o, "py", g_cfg.calib[i].py);
        cJSON_AddItemToArray(cal, o);
    }

    cJSON *v = cJSON_AddObjectToObject(j, "voice");
    cJSON_AddBoolToObject(v, "ok", voice_ok());
    voice_last(buf, sizeof buf);
    cJSON_AddStringToObject(v, "last", buf);

    net_info(ip, sizeof ip, &mode);
    cJSON *n = cJSON_AddObjectToObject(j, "net");
    cJSON_AddStringToObject(n, "ip", ip);
    cJSON_AddStringToObject(n, "mode", mode);
    char ssid[33];
    net_ssid(ssid, sizeof ssid);
    cJSON_AddStringToObject(n, "ssid", ssid);
    cJSON_AddBoolToObject(n, "online", net_online());

    char ver[32], part[16];
    ota_info(ver, sizeof ver, part, sizeof part);
    cJSON *fw = cJSON_AddObjectToObject(j, "fw");
    cJSON_AddStringToObject(fw, "version", ver);
    cJSON_AddStringToObject(fw, "partition", part);
    char latest[16], ust[64];
    update_status(latest, sizeof latest, ust, sizeof ust);
    cJSON_AddStringToObject(fw, "latest", latest);
    cJSON_AddStringToObject(fw, "update", ust);
    cJSON_AddBoolToObject(fw, "auto", update_auto());
    return send_json(r, j);
}

// ---------- POST /api/wifi {"ssid":"..","pass":".."} -> saved, reboot to join ----------
static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }

static esp_err_t h_wifi(httpd_req_t *r)
{
    cJSON *b = body(r);
    cJSON *ss = cJSON_GetObjectItem(b, "ssid"), *pw = cJSON_GetObjectItem(b, "pass");
    bool ok = cJSON_IsString(ss) && ss->valuestring[0] && strlen(ss->valuestring) < 33
              && net_set_wifi(ss->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
    cJSON_Delete(b);
    if (!ok) return reply(r, false, "need a network name");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return reply(r, true, "saved — rebooting onto your WiFi. find it at http://xarm.local");
}

// ---------- POST /api/update {"auto":true|false} and/or {"check":true} ----------
static esp_err_t h_update(httpd_req_t *r)
{
    cJSON *b = body(r);
    cJSON *a = cJSON_GetObjectItem(b, "auto");
    if (cJSON_IsBool(a)) update_set_auto(cJSON_IsTrue(a));
    else update_check_now();
    cJSON_Delete(b);
    return reply(r, true, cJSON_IsBool(a) ? (update_auto() ? "auto-update on" : "auto-update off") : "checking…");
}

// ---------- POST /api/cmd {"text":"pick up the red"} ----------
static esp_err_t h_cmd(httpd_req_t *r)
{
    cJSON *b = body(r);
    cJSON *t = cJSON_GetObjectItem(b, "text");
    char msg[96];
    bool ok = cJSON_IsString(t) && router_exec(t->valuestring, msg, sizeof msg);
    if (!cJSON_IsString(t)) strcpy(msg, "need text");
    cJSON_Delete(b);
    return reply(r, ok, msg);
}

// ---------- POST /api/jog {"id":6,"pos":420,"ms":150}  or {"pos":[...6],"ms":800} ----------
static esp_err_t h_jog(httpd_req_t *r)
{
    if (pick_busy()) return reply(r, false, "busy");
    cJSON *b = body(r);
    cJSON *pos = cJSON_GetObjectItem(b, "pos");
    int ms = num(b, "ms", 150);
    if (cJSON_IsArray(pos)) { uint16_t p[NSERVO]; arm_last(p); read_arr(b, "pos", p, false); arm_move(p, ms); }
    else arm_move_one(num(b, "id", 0), clamp_pos(num(b, "pos", 500)), ms);
    cJSON_Delete(b);
    return reply(r, true, "ok");
}

// ---------- POST /api/torque {"on":false} ----------
static esp_err_t h_torque(httpd_req_t *r)
{
    if (pick_busy()) return reply(r, false, "busy");
    cJSON *b = body(r);
    bool on = cJSON_IsTrue(cJSON_GetObjectItem(b, "on"));
    cJSON_Delete(b);
    arm_torque(on);
    return reply(r, true, on ? "holding" : "limp — move it by hand");
}

// ---------- POST /api/teach {"what": home|drop|calib_mark|calib_add|calib_undo|calib_clear|test_lift} ----------
static esp_err_t h_teach(httpd_req_t *r)
{
    if (pick_busy()) return reply(r, false, "busy");
    cJSON *b = body(r);
    cJSON *w = cJSON_GetObjectItem(b, "what");
    char what[16] = "";
    if (cJSON_IsString(w)) strlcpy(what, w->valuestring, sizeof what);
    cJSON_Delete(b);

    uint16_t p[NSERVO];
    char msg[96];
    bool ok = true;

    if (!strcmp(what, "calib_mark")) {
        // step 1: object on the mat, arm clear of the camera view. remember where the camera sees it
        s_mark_ok = pick_locate(0, &s_mark_x, &s_mark_y) || pick_locate(1, &s_mark_x, &s_mark_y);
        if (!s_mark_ok) return reply(r, false, "camera can't see a target");
        snprintf(msg, sizeof msg, "marked (%.0f,%.0f) — now jog to grasp pose, then Save point", s_mark_x, s_mark_y);
        return reply(r, true, msg);
    } else if (!strcmp(what, "calib_undo")) {
        if (g_cfg.n_calib) g_cfg.n_calib--;
        snprintf(msg, sizeof msg, "%d points", g_cfg.n_calib);
    } else if (!strcmp(what, "calib_clear")) {
        g_cfg.n_calib = 0;
        strcpy(msg, "cleared");
    } else if (!arm_read(p)) {
        return reply(r, false, "no reply from arm");
    } else if (!strcmp(what, "home")) {
        memcpy(g_cfg.home, p, sizeof p); strcpy(msg, "home saved");
    } else if (!strcmp(what, "drop")) {
        memcpy(g_cfg.drop, p, sizeof p); strcpy(msg, "drop saved");
    } else if (!strcmp(what, "test_lift")) {
        for (int i = 0; i < NSERVO; i++) p[i] = clamp_pos(p[i] + g_cfg.lift[i]);
        arm_move(p, 400);
        return reply(r, true, "applied lift from current pose");
    } else if (!strcmp(what, "calib_add")) {
        // step 2: arm is now at the grasp pose over the marked object
        if (!s_mark_ok) return reply(r, false, "mark the target first");
        if (g_cfg.n_calib >= MAX_CALIB) return reply(r, false, "full — undo or clear");
        calib_pt_t *c = &g_cfg.calib[g_cfg.n_calib++];
        c->px = (int16_t)s_mark_x; c->py = (int16_t)s_mark_y;
        memcpy(c->pos, p, sizeof p);
        s_mark_ok = false;
        snprintf(msg, sizeof msg, "point %d at (%d,%d)", g_cfg.n_calib, c->px, c->py);
    } else {
        ok = false; strcpy(msg, "unknown");
    }
    if (ok) store_save();
    return reply(r, ok, msg);
}

// ---------- GET/POST /api/config ----------
static esp_err_t h_cfg_get(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "home", u16arr(g_cfg.home, NSERVO));
    cJSON_AddItemToObject(j, "drop", u16arr(g_cfg.drop, NSERVO));
    cJSON *l = cJSON_AddArrayToObject(j, "lift");
    for (int i = 0; i < NSERVO; i++) cJSON_AddItemToArray(l, cJSON_CreateNumber(g_cfg.lift[i]));
    cJSON_AddNumberToObject(j, "grip_open", g_cfg.grip_open);
    cJSON_AddNumberToObject(j, "grip_close", g_cfg.grip_close);
    cJSON_AddNumberToObject(j, "move_ms", g_cfg.move_ms);
    cJSON_AddNumberToObject(j, "grip_ms", g_cfg.grip_ms);
    cJSON *n = cJSON_AddArrayToObject(j, "slot_name");
    for (int i = 0; i < NSLOT; i++) cJSON_AddItemToArray(n, cJSON_CreateString(g_cfg.slot_name[i]));
    return send_json(r, j);
}

static esp_err_t h_cfg_post(httpd_req_t *r)
{
    cJSON *b = body(r);
    if (!b) return reply(r, false, "bad json");
    store_lock();
    read_arr(b, "home", g_cfg.home, false);
    read_arr(b, "drop", g_cfg.drop, false);
    read_arr(b, "lift", g_cfg.lift, true);
    g_cfg.grip_open  = clamp_pos(num(b, "grip_open", g_cfg.grip_open));
    g_cfg.grip_close = clamp_pos(num(b, "grip_close", g_cfg.grip_close));
    g_cfg.move_ms    = (uint16_t)num(b, "move_ms", g_cfg.move_ms);
    g_cfg.grip_ms    = (uint16_t)num(b, "grip_ms", g_cfg.grip_ms);
    cJSON *n = cJSON_GetObjectItem(b, "slot_name");
    bool names = cJSON_IsArray(n) && cJSON_GetArraySize(n) == NSLOT;
    for (int i = 0; names && i < NSLOT; i++) {
        cJSON *s = cJSON_GetArrayItem(n, i);
        if (cJSON_IsString(s)) strlcpy(g_cfg.slot_name[i], s->valuestring, sizeof g_cfg.slot_name[i]);
    }
    store_unlock();
    cJSON_Delete(b);
    store_save();
    if (names) voice_reload();
    return reply(r, true, "saved");
}

// ---------- app ----------
static esp_err_t h_app(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    return httpd_resp_send(r, (const char *)app_start, app_end - app_start);
}

bool web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 16;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 15;                 // OTA uploads: a 1 MB .bin over WiFi needs more than the 5 s default
    httpd_handle_t h;
    if (httpd_start(&h, &c) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return false; }
    const httpd_uri_t routes[] = {
        {"/api/state",  HTTP_GET,  h_state},
        {"/api/cmd",    HTTP_POST, h_cmd},
        {"/api/jog",    HTTP_POST, h_jog},
        {"/api/torque", HTTP_POST, h_torque},
        {"/api/teach",  HTTP_POST, h_teach},
        {"/api/config", HTTP_GET,  h_cfg_get},
        {"/api/config", HTTP_POST, h_cfg_post},
        {"/api/ota",    HTTP_POST, ota_handle},
        {"/api/wifi",   HTTP_POST, h_wifi},
        {"/api/update", HTTP_POST, h_update},
        {"/*",          HTTP_GET,  h_app},       // keep last
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; i++) httpd_register_uri_handler(h, &routes[i]);
    ESP_LOGI(TAG, "web up on :80");
    return true;
}
