// net.h — WiFi STA (menuconfig creds) with fallback AP "xarm-voice". mDNS: http://xarm.local
#pragma once
#include <stddef.h>
void net_start(void);
void net_info(char *ip, size_t nip, const char **mode);
