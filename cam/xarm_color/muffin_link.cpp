#include "muffin_link.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include "colors.h"

static WiFiUDP udp;
static uint16_t seq;
static uint32_t last_ms;

void link_begin(void)
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                  // no power-save naps: they add 100+ ms of lag
  WiFi.setAutoReconnect(true);
  WiFi.begin(MUFFIN_SSID, MUFFIN_PASS);
}

// "MUF1" | u16 seq | slot0 color | slot1 color | 5 x (x, y, w, h)
void link_send(const send_color_data_t color[5])
{
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (now - last_ms < 40) return;
  last_ms = now;
  uint8_t p[8 + 5 * 4] = {'M', 'U', 'F', '1', (uint8_t)seq, (uint8_t)(seq >> 8), SLOT0_COLOR, SLOT1_COLOR};
  seq++;
  for (int c = 0; c < 5; c++) {
    p[8 + c * 4 + 0] = color[c].center_x;
    p[8 + c * 4 + 1] = color[c].center_y;
    p[8 + c * 4 + 2] = color[c].width;
    p[8 + c * 4 + 3] = color[c].length;
  }
  udp.beginPacket(WiFi.gatewayIP(), MUFFIN_PORT);   // the board is the network's gateway (192.168.4.1)
  udp.write(p, sizeof p);
  udp.endPacket();
}
