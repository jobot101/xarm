#include "web.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "arm.h"
#include "cam.h"
#include "net.h"
#include "ota.h"
#include "update.h"
#include "moves.h"
#include "kids.h"
#include "fx.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pick.h"
#include "router.h"
#include "store.h"
#include "voice.h"
#include "cloud.h"
#include "sdkconfig.h"
#include "audio.h"

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
    {
        uint16_t l[NSERVO][2];
        arm_limits(l);
        cJSON *la = cJSON_AddArrayToObject(arm, "lim");
        for (int i = 0; i < NSERVO; i++) cJSON_AddItemToArray(la, u16arr(l[i], 2));
    }

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
    cJSON *kd = cJSON_AddObjectToObject(j, "kids");
    cJSON_AddNumberToObject(kd, "cur", kids_current());
    cJSON *kl = cJSON_AddArrayToObject(kd, "list");
    for (int i = 0; i < KIDS_MAX; i++) {
        const kid_t *k = kids_get(i);
        if (!k) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "name", k->name);
        cJSON_AddStringToObject(o, "emoji", k->emoji);
        cJSON_AddNumberToObject(o, "jewel", k->jewel);
        cJSON_AddNumberToObject(o, "visits", k->visits);
        cJSON_AddNumberToObject(o, "best_bells", k->best_bells);
        cJSON_AddNumberToObject(o, "best_tower", k->best_tower);
        cJSON_AddItemToArray(kl, o);
    }
    cJSON *mv = cJSON_AddObjectToObject(j, "moves");
    int frames = 0;
    cJSON_AddBoolToObject(mv, "recording", moves_recording(&frames));
    cJSON_AddNumberToObject(mv, "frames", frames);
    cJSON *ml = cJSON_AddArrayToObject(mv, "list");
    for (int i = 0; i < MOVE_MAX; i++) if (moves_name(i)[0]) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "i", i);
        cJSON_AddStringToObject(o, "name", moves_name(i));
        cJSON_AddItemToArray(ml, o);
    }
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
    {
        char vs[64]; int lv, g, th; float pr;
        voice_diag(vs, sizeof vs, &lv, &pr, &g, &th);
        cJSON_AddStringToObject(v, "status", vs);
        cJSON_AddNumberToObject(v, "level", lv);
        cJSON_AddNumberToObject(v, "prob", (int)(pr * 100));
        cJSON_AddNumberToObject(v, "gain", g);
        cJSON_AddNumberToObject(v, "thresh", th);
        bool wk, open; const char *vad;
        voice_diag2(&wk, &open, &vad);
        cJSON_AddBoolToObject(v, "wake", wk);          // "Hey Muffin" first?
        cJSON_AddBoolToObject(v, "awake", open);       // listening for commands right now
        cJSON_AddStringToObject(v, "vad", vad);        // "VADNet" or "WebRTC" (old speech models in flash)
        cJSON_AddBoolToObject(v, "dual", audio_mic_dual());
        {   // cloud ears (Groq Whisper). The key itself never leaves the board, only "gsk_…wxyz"
            bool on; char kh[24], cs[80], cl[96]; int ms, today;
            cloud_diag(&on, kh, sizeof kh, cs, sizeof cs, cl, sizeof cl, &ms, &today);
            cJSON *c = cJSON_AddObjectToObject(v, "cloud");
            cJSON_AddBoolToObject(c, "on", on);
            cJSON_AddBoolToObject(c, "ready", cloud_ready());
            cJSON_AddStringToObject(c, "key", kh);
            cJSON_AddStringToObject(c, "status", cs);
            cJSON_AddStringToObject(c, "last", cl);
            cJSON_AddNumberToObject(c, "ms", ms);
            cJSON_AddNumberToObject(c, "today", today);
            cJSON_AddNumberToObject(c, "max", CONFIG_XARM_STT_DAILY_MAX);
        }
        char bs[72];
        update_sound_status(bs, sizeof bs);
        cJSON_AddBoolToObject(v, "bank", fx_bank_ok());
        cJSON_AddStringToObject(v, "bank_status", bs);
        voice_note_t nt[8];
        int nn = voice_notes(nt, 8);
        cJSON *ev = cJSON_AddArrayToObject(v, "heard");
        for (int i = 0; i < nn; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "p", nt[i].phrase);
            cJSON_AddNumberToObject(o, "prob", nt[i].prob);
            cJSON_AddStringToObject(o, "why", nt[i].why);
            cJSON_AddNumberToObject(o, "ago", nt[i].age_s);
            cJSON_AddItemToArray(ev, o);
        }
    }

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
    return reply(r, true, "saved — rebooting onto your WiFi. find it at http://muffin or http://muffin.local");
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

// ---------- POST /api/moves {"action":"record"|"save"|"cancel"|"play"|"delete", "name":"..", "i":n} ----------
static esp_err_t h_moves(httpd_req_t *r)
{
    cJSON *b = body(r);
    cJSON *a = cJSON_GetObjectItem(b, "action"), *nm = cJSON_GetObjectItem(b, "name");
    int i = num(b, "i", -1);
    const char *act = cJSON_IsString(a) ? a->valuestring : "";
    char msg[64] = "ok";
    bool ok = true;
    if (!strcmp(act, "record")) {
        ok = !pick_busy() && moves_record_start();
        strcpy(msg, ok ? "recording — move the arm by hand" : "busy");
    } else if (!strcmp(act, "save")) {
        char name[16] = "";
        if (cJSON_IsString(nm)) for (int k = 0; nm->valuestring[k] && k < 15; k++) name[k] = (char)tolower((unsigned char)nm->valuestring[k]);
        ok = moves_record_save(name);
        snprintf(msg, sizeof msg, ok ? "saved \"%s\"" : "couldn't save (too short, no name, or full)", name);
        if (ok) voice_reload();
    } else if (!strcmp(act, "cancel")) {
        moves_record_cancel(); strcpy(msg, "cancelled");
    } else if (!strcmp(act, "play")) {
        ok = pick_run(OP_PLAY, i, msg, sizeof msg);
    } else if (!strcmp(act, "delete")) {
        ok = moves_delete(i); strcpy(msg, ok ? "deleted" : "no such move");
        if (ok) voice_reload();
    } else { ok = false; strcpy(msg, "unknown action"); }
    cJSON_Delete(b);
    return reply(r, ok, msg);
}

// ---------- POST /api/kids {"action":"add",name,emoji,jewel} | {"action":"select"|"remove", i} | {"action":"score", game, score} ----------
static esp_err_t h_kids(httpd_req_t *r)
{
    cJSON *b = body(r);
    cJSON *a = cJSON_GetObjectItem(b, "action");
    const char *act = cJSON_IsString(a) ? a->valuestring : "";
    char msg[48] = "ok";
    bool ok = true;
    if (!strcmp(act, "add")) {
        cJSON *nm = cJSON_GetObjectItem(b, "name"), *em = cJSON_GetObjectItem(b, "emoji");
        int i = kids_add(cJSON_IsString(nm) ? nm->valuestring : "", cJSON_IsString(em) ? em->valuestring : "", num(b, "jewel", 0));
        ok = i >= 0;
        if (ok) { kids_select(i); voice_reload(); snprintf(msg, sizeof msg, "hi %s!", kids_get(i)->name); }
        else strcpy(msg, "need a name (or the list is full)");
    } else if (!strcmp(act, "select")) {
        ok = kids_select(num(b, "i", -1));
        if (ok) snprintf(msg, sizeof msg, "hi %s!", kids_get(kids_current())->name);
    } else if (!strcmp(act, "remove")) {
        ok = kids_remove(num(b, "i", -1));
        if (ok) voice_reload();
    } else if (!strcmp(act, "score")) {
        cJSON *g = cJSON_GetObjectItem(b, "game");
        kids_score(cJSON_IsString(g) ? g->valuestring : "", num(b, "score", -1));
    } else { ok = false; strcpy(msg, "unknown action"); }
    cJSON_Delete(b);
    return reply(r, ok, msg);
}

// ---------- POST /api/fx {"show":0..3} (game: light + note) | {"event":"sad"|"happy"} ----------
static esp_err_t h_fx(httpd_req_t *r)
{
    cJSON *b = body(r);
    int show = num(b, "show", -1);
    cJSON *e = cJSON_GetObjectItem(b, "event");
    if (show >= 0) fx_show(show);
    else if (cJSON_IsString(e)) fx_event(!strcmp(e->valuestring, "sad") ? FX_ERROR : FX_CHEER);
    cJSON_Delete(b);
    return reply(r, true, "ok");
}

// ---------- POST /api/limits {"i":0..5,"min":n,"max":n} ----------
static esp_err_t h_limits(httpd_req_t *r)
{
    cJSON *b = body(r);
    int i = num(b, "i", -1), lo = num(b, "min", 0), hi = num(b, "max", 1000);
    cJSON_Delete(b);
    if (i < 0 || i >= NSERVO || hi - lo < 20) return reply(r, false, "bad limits");
    arm_limits_set(i, lo, hi);
    return reply(r, true, "limits saved");
}

// ---------- GET /api/i2c -> {"found":"0x18 0x20 0x40 0x51 0x52","cam":true} ----------
static esp_err_t h_i2c(httpd_req_t *r)
{
    char list[128];
    cam_scan(list, sizeof list);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "found", list);
    cJSON_AddBoolToObject(j, "cam", strstr(list, "0x52") != NULL);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, txt);
    free(txt);
    return e;
}

// ---------- GET /api/phrases -> ["stop","go home",...] — everything voice listens for ----------
static esp_err_t h_phrases(httpd_req_t *r)
{
    enum { MAXP = 200 };
    char (*ph)[48] = malloc(MAXP * 48);
    if (!ph) return reply(r, false, "no memory");
    int n = router_phrases(ph, MAXP);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(ph[i]));
    free(ph);
    char *txt = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, txt);
    free(txt);
    return e;
}

// ---------- POST /api/voice {"gain":0..42,"thresh":5..90,"wake":0|1,"dual":0|1,"cloud":0|1,"stt_key":"gsk_…"} ----------
// "dual" (second mic) changes the AFE channel layout, so it's saved and the board reboots.
static esp_err_t h_voice(httpd_req_t *r)
{
    cJSON *b = body(r);
    voice_tune(num(b, "gain", -1), num(b, "thresh", -1), num(b, "wake", -1));
    int dual = num(b, "dual", -1), cl = num(b, "cloud", -1);
    cJSON *key = cJSON_GetObjectItem(b, "stt_key");
    bool keyed = cJSON_IsString(key);
    if (keyed) cloud_set_key(key->valuestring);
    if (cl == 0 || cl == 1) cloud_enable(cl);
    cJSON_Delete(b);
    if (keyed) return reply(r, true, cloud_ready() ? "key saved — say “Hey Muffin”" : "key saved");
    if ((dual == 0 || dual == 1) && dual != audio_mic_dual()) {
        audio_mic_dual_save(dual);
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
        return reply(r, true, dual ? "both mics on — restarting" : "one mic — restarting");
    }
    return reply(r, true, "voice tuned");
}

// ---------- POST /api/cmd {"text":"pick up the red"} ----------
static esp_err_t h_cmd(httpd_req_t *r)
{
    cJSON *b = body(r);
    cJSON *t = cJSON_GetObjectItem(b, "text");
    char msg[96];
    bool ok = cJSON_IsString(t) && router_exec(t->valuestring, msg, sizeof msg);
    if (!cJSON_IsString(t)) strcpy(msg, "need text");
    else voice_engage();     // someone's playing from the app: Muffin listens without "Hey Muffin" for a bit
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
    // one joint = a slider or nudge button: chase the newest value smoothly instead of a timed hop,
    // so drag updates arriving unevenly over WiFi don't turn into stop-start motion
    else { (void)ms; arm_follow_one(num(b, "id", 0), clamp_pos(num(b, "pos", 500))); }
    cJSON_Delete(b);
    return reply(r, true, "ok");
}

// ---------- POST /api/drive {"base":-1|0|1,"lift":..,"reach":..,"tilt":..,"twist":..,"grip":..,"step":n,"ms":n}
// continuous hold-to-move: the app sends this ~every 100 ms while a direction is held, and all-zero on
// release. It sets a velocity (step units per tick); the board's 25 Hz streamer ramps it smoothly, so
// WiFi jitter between packets doesn't reach the servos. No packet for 300 ms = ramp down by itself.
// base/twist/tilt/grip drive joints directly; lift/reach move along the taught lift and reach vectors.
static esp_err_t h_drive(httpd_req_t *r)
{
    if (pick_busy()) return reply(r, false, "busy");
    cJSON *b = body(r);
    int step = num(b, "step", 10), ms = num(b, "ms", 110);
    int base = num(b, "base", 0), lift = num(b, "lift", 0), reach = num(b, "reach", 0);
    int tilt = num(b, "tilt", 0), twist = num(b, "twist", 0), grip = num(b, "grip", 0);
    cJSON_Delete(b);
    float per_s = 1000.0f / (ms > 40 ? ms - 20 : 100);       // app sends ms = tick + 20
    float v[NSERVO];
    for (int i = 0; i < NSERVO; i++)
        v[i] = (lift * g_cfg.lift[i] + reach * g_reach[i]) * step / 100.0f;
    v[5] += base * step;
    v[1] += twist * step * 3 / 2.0f;
    v[2] += tilt * step;
    v[0] += grip * step * 2 * (g_cfg.grip_close > g_cfg.grip_open ? 1 : -1);   // +1 = close
    for (int i = 0; i < NSERVO; i++) v[i] *= per_s;
    arm_drive(v, 300);
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
        // step 1: object on the mat. fixed cam: arm out of view. head cam: arm goes to the look pose first
        if (g_cfg.cam_on_head) pick_look();
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
    } else if (!strcmp(what, "helper")) {
        memcpy(g_cfg.helper, p, sizeof p); strcpy(msg, "helper spot saved");
    } else if (!strcmp(what, "tower")) {
        memcpy(g_cfg.tower, p, sizeof p); strcpy(msg, "tower spot saved (first block's set-down pose)");
    } else if (!strcmp(what, "look")) {
        memcpy(g_cfg.look, p, sizeof p); strcpy(msg, "look pose saved — redo calibration from here");
    } else if (!strcmp(what, "give")) {
        memcpy(g_cfg.give, p, sizeof p); strcpy(msg, "hand-off spot saved");
    } else if (!strcmp(what, "bell")) {
        memcpy(g_cfg.bell, p, sizeof p); strcpy(msg, "bell spot saved");
    } else if (!strcmp(what, "bin0") || !strcmp(what, "bin1")) {
        int b = what[3] - '0';
        memcpy(g_cfg.bin[b], p, sizeof p); snprintf(msg, sizeof msg, "%s bin saved", g_cfg.slot_name[b]);
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
    cJSON *p2 = cJSON_AddObjectToObject(j, "pick2");
    cJSON_AddNumberToObject(p2, "align_axis", g_pick2.align_axis);
    cJSON_AddNumberToObject(p2, "align_turn", g_pick2.align_turn);
    cJSON_AddBoolToObject(p2, "grab_check", g_pick2.grab_check);
    cJSON_AddNumberToObject(p2, "grab_margin", g_pick2.grab_margin);
    cJSON_AddNumberToObject(p2, "grab_tries", g_pick2.grab_tries);
    cJSON_AddNumberToObject(p2, "size_split", g_pick2.size_split);
    cJSON *rc = cJSON_AddArrayToObject(j, "reach");
    for (int i = 0; i < NSERVO; i++) cJSON_AddItemToArray(rc, cJSON_CreateNumber(g_reach[i]));
    cJSON *l = cJSON_AddArrayToObject(j, "lift");
    for (int i = 0; i < NSERVO; i++) cJSON_AddItemToArray(l, cJSON_CreateNumber(g_cfg.lift[i]));
    cJSON_AddNumberToObject(j, "grip_open", g_cfg.grip_open);
    cJSON_AddNumberToObject(j, "grip_close", g_cfg.grip_close);
    cJSON_AddNumberToObject(j, "move_ms", g_cfg.move_ms);
    cJSON_AddNumberToObject(j, "grip_ms", g_cfg.grip_ms);
    cJSON_AddBoolToObject(j, "cam_on_head", g_cfg.cam_on_head);
    cJSON_AddNumberToObject(j, "stack_pct", g_cfg.stack_pct);
    cJSON *sc = cJSON_AddArrayToObject(j, "scan");
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(sc, cJSON_CreateNumber(g_cfg.scan[i]));
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
    if (cJSON_IsArray(cJSON_GetObjectItem(b, "reach"))) { read_arr(b, "reach", g_reach, true); store_reach_save(); }
    cJSON *q = cJSON_GetObjectItem(b, "pick2");
    if (cJSON_IsObject(q)) {
        g_pick2.align_axis  = (uint8_t)num(q, "align_axis", g_pick2.align_axis);
        g_pick2.align_turn  = (int16_t)num(q, "align_turn", g_pick2.align_turn);
        cJSON *gc = cJSON_GetObjectItem(q, "grab_check");
        if (cJSON_IsBool(gc)) g_pick2.grab_check = cJSON_IsTrue(gc);
        g_pick2.grab_margin = (int16_t)num(q, "grab_margin", g_pick2.grab_margin);
        g_pick2.grab_tries  = (uint8_t)num(q, "grab_tries", g_pick2.grab_tries);
        g_pick2.size_split  = (int16_t)num(q, "size_split", g_pick2.size_split);
        store_pick2_save();
    }
    g_cfg.grip_open  = clamp_pos(num(b, "grip_open", g_cfg.grip_open));
    g_cfg.grip_close = clamp_pos(num(b, "grip_close", g_cfg.grip_close));
    g_cfg.move_ms    = (uint16_t)num(b, "move_ms", g_cfg.move_ms);
    g_cfg.grip_ms    = (uint16_t)num(b, "grip_ms", g_cfg.grip_ms);
    g_cfg.stack_pct = (int16_t)num(b, "stack_pct", g_cfg.stack_pct);
    cJSON *hd = cJSON_GetObjectItem(b, "cam_on_head");
    if (cJSON_IsBool(hd)) g_cfg.cam_on_head = cJSON_IsTrue(hd);
    cJSON *sc = cJSON_GetObjectItem(b, "scan");
    if (cJSON_IsArray(sc) && cJSON_GetArraySize(sc) == 3)
        for (int i = 0; i < 3; i++) g_cfg.scan[i] = (int16_t)cJSON_GetArrayItem(sc, i)->valueint;
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
    c.max_uri_handlers = 24;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.lru_purge_enable = true;
    c.stack_size = 8192;                      // /api/state builds a big JSON tree; 4 KB default is tight
    c.recv_wait_timeout = 15;                 // OTA uploads: a 1 MB .bin over WiFi needs more than the 5 s default
    httpd_handle_t h;
    if (httpd_start(&h, &c) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return false; }
    const httpd_uri_t routes[] = {
        {"/api/state",  HTTP_GET,  h_state},
        {"/api/cmd",    HTTP_POST, h_cmd},
        {"/api/jog",    HTTP_POST, h_jog},
        {"/api/drive",  HTTP_POST, h_drive},
        {"/api/torque", HTTP_POST, h_torque},
        {"/api/teach",  HTTP_POST, h_teach},
        {"/api/config", HTTP_GET,  h_cfg_get},
        {"/api/config", HTTP_POST, h_cfg_post},
        {"/api/ota",    HTTP_POST, ota_handle},
        {"/api/wifi",   HTTP_POST, h_wifi},
        {"/api/update", HTTP_POST, h_update},
        {"/api/moves",  HTTP_POST, h_moves},
        {"/api/fx",     HTTP_POST, h_fx},
        {"/api/kids",   HTTP_POST, h_kids},
        {"/api/voice",  HTTP_POST, h_voice},
        {"/api/limits", HTTP_POST, h_limits},
        {"/api/i2c",    HTTP_GET,  h_i2c},
        {"/api/phrases",HTTP_GET,  h_phrases},
        {"/*",          HTTP_GET,  h_app},       // keep last
    };
    for (size_t i = 0; i < sizeof routes / sizeof *routes; i++) httpd_register_uri_handler(h, &routes[i]);
    ESP_LOGI(TAG, "web up on :80");
    return true;
}
