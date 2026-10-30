// The TF slot (Waveshare wiki): CLK GPIO40, CMD GPIO42, D0 GPIO41, D1/D2 not connected, D3 on the TCA9555
// (EXIO3). That's SDMMC 1-bit mode, with D3 driven high so the card starts in SD mode, not SPI mode.
// D1-D3 must be GPIO_NUM_NC here: the S3 defaults put them on 8/9/10, and GPIO10 is the I2C clock.
#include "sd.h"
#include <stdio.h>
#include <string.h>
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "sd";
static bool s_ok;
static char s_status[64] = "no card";
static sdmmc_card_t *s_card;

#define TCA9555_ADDR 0x20

// EXIO3 = TCA9555 port 0 bit 3 -> output, high. Read-modify-write: the buttons and resets share the port.
static void d3_high(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t d = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA9555_ADDR, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t h;
    if (i2c_master_bus_add_device(bus, &d, &h) != ESP_OK) return;
    uint8_t reg, v;
    reg = 0x02; if (i2c_master_transmit_receive(h, &reg, 1, &v, 1, 50) == ESP_OK) {      // output port 0
        uint8_t w[2] = {0x02, (uint8_t)(v | 0x08)}; i2c_master_transmit(h, w, 2, 50); }
    reg = 0x06; if (i2c_master_transmit_receive(h, &reg, 1, &v, 1, 50) == ESP_OK) {      // config port 0 (1 = input)
        uint8_t w[2] = {0x06, (uint8_t)(v & ~0x08)}; i2c_master_transmit(h, w, 2, 50); }
    i2c_master_bus_rm_device(h);
}

static void fmt_size(char *out, size_t n, uint64_t b)
{
    if (b >= 1000000000ULL) snprintf(out, n, "%.1f GB", b / 1e9);
    else snprintf(out, n, "%u MB", (unsigned)(b / 1000000));
}

bool sd_init(i2c_master_bus_handle_t bus)
{
    d3_high(bus);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;                 // 20 MHz: plenty for 32 KB/s of voice
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_40;
    slot.cmd = GPIO_NUM_42;
    slot.d0 = GPIO_NUM_41;
    slot.d1 = slot.d2 = slot.d3 = slot.d4 = slot.d5 = slot.d6 = slot.d7 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,                     // never: the card may hold someone's photos
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t e = esp_vfs_fat_sdmmc_mount(SD_ROOT, &host, &slot, &mc, &s_card);
    if (e == ESP_FAIL) {
        strlcpy(s_status, "card isn't FAT32 (exFAT cards are left alone, never formatted)", sizeof s_status);
    } else if (e != ESP_OK) {
        snprintf(s_status, sizeof s_status, "no card (%s)", esp_err_to_name(e));
    } else {
        s_ok = true;
        uint64_t total = 0, free_b = 0;
        esp_vfs_fat_info(SD_ROOT, &total, &free_b);
        char t[16], f[16];
        fmt_size(t, sizeof t, total); fmt_size(f, sizeof f, free_b);
        snprintf(s_status, sizeof s_status, "%s card, %s free", t, f);
    }
    ESP_LOGI(TAG, "%s", s_status);
    return s_ok;
}

bool sd_ok(void) { return s_ok; }
void sd_status(char *out, size_t n) { strlcpy(out, s_status, n); }

uint64_t sd_free_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_ok) esp_vfs_fat_info(SD_ROOT, &total, &free_b);
    return free_b;
}

bool sd_voice_wanted(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("xarm", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "sd_voice", &v); nvs_close(h); }
    return v != 0;
}

void sd_voice_set(bool on)
{
    nvs_handle_t h;
    if (nvs_open("xarm", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "sd_voice", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}
