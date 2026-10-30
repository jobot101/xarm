#include "kids.h"
#include <ctype.h>
#include <string.h>
#include "esp_log.h"
#include "fx.h"
#include "nvs.h"
#include "school.h"

static const char *TAG = "kids";
static kid_t s_kids[KIDS_MAX];
static int s_cur = -1;

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition("store", "kids", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "all", s_kids, sizeof s_kids);
    nvs_commit(h);
    nvs_close(h);
}

void kids_init(void)        // after moves_init() (which brings up the "store" partition)
{
    nvs_handle_t h;
    size_t len = sizeof s_kids;
    if (nvs_open_from_partition("store", "kids", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "all", s_kids, &len) != ESP_OK || len != sizeof s_kids) memset(s_kids, 0, sizeof s_kids);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%d kids", kids_count());
}

int kids_count(void) { int n = 0; for (int i = 0; i < KIDS_MAX; i++) n += s_kids[i].name[0] != 0; return n; }

const kid_t *kids_get(int i) { return (i >= 0 && i < KIDS_MAX && s_kids[i].name[0]) ? &s_kids[i] : NULL; }

int kids_add(const char *name, const char *emoji, int jewel)
{
    char n[16] = "";
    for (int k = 0, j = 0; name[k] && j < 15; k++)
        if (isalpha((unsigned char)name[k]) || name[k] == ' ') n[j++] = (char)tolower((unsigned char)name[k]);
    if (!n[0]) return -1;
    for (int i = 0; i < KIDS_MAX; i++) if (!strcmp(s_kids[i].name, n)) return i;   // already known
    for (int i = 0; i < KIDS_MAX; i++) if (!s_kids[i].name[0]) {
        memset(&s_kids[i], 0, sizeof s_kids[i]);
        strlcpy(s_kids[i].name, n, sizeof s_kids[i].name);
        strlcpy(s_kids[i].emoji, emoji && emoji[0] ? emoji : "🙂", sizeof s_kids[i].emoji);
        s_kids[i].jewel = (uint8_t)(jewel & 3);
        save();
        school_kid_reset(i);                  // a fresh slot starts school from lesson one
        return i;
    }
    return -1;
}

bool kids_remove(int i)
{
    if (!kids_get(i)) return false;
    memset(&s_kids[i], 0, sizeof s_kids[i]);
    if (s_cur == i) s_cur = -1;
    save();
    school_kid_reset(i);
    return true;
}

bool kids_select(int i)
{
    if (!kids_get(i)) { s_cur = -1; return false; }
    s_cur = i;
    s_kids[i].visits++;
    save();
    // each name gets its own 3-note jingle: same kid, same tune, every time
    unsigned h = 5381;
    for (const char *c = s_kids[i].name; *c; c++) h = h * 33 + (unsigned char)*c;
    fx_greet(s_kids[i].jewel, h);
    return true;
}

int kids_current(void) { return s_cur; }

int kids_find(const char *text)
{
    for (int i = 0; i < KIDS_MAX; i++) if (s_kids[i].name[0] && strstr(text, s_kids[i].name)) return i;
    return -1;
}

bool kids_score(const char *game, int score)
{
    if (s_cur < 0 || score < 0) return false;
    uint16_t *best = !strcmp(game, "bells") ? &s_kids[s_cur].best_bells
                   : !strcmp(game, "tower") ? &s_kids[s_cur].best_tower : NULL;
    if (best && score > *best) { *best = (uint16_t)score; save(); fx_event(FX_CHEER); return true; }
    return false;
}
