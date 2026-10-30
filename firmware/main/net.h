// net.h — WiFi STA (creds saved from the web app, else menuconfig) with fallback AP "xarm-voice".
// mDNS: http://xarm.local
#pragma once
#include <stdbool.h>
#include <stddef.h>
void net_start(void);
void net_info(char *ip, size_t nip, const char **mode);
void net_ssid(char *out, size_t n);                       // configured home network ("" if none)
bool net_online(void);
void net_why(char *out, size_t n);                        // why home WiFi isn't joined ("" when it is)                                    // STA connected with an IP (internet possible)
bool net_set_wifi(const char *ssid, const char *pass);    // save to NVS; takes effect after reboot
