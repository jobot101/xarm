// pc.h — Johnny: the arm's brain runs on the shop PC (CONFIG_XARM_JOHNNY).
//
// The PC connects to ws://<board>/ws and the board becomes hands, ears and mouth:
//   board -> PC  binary: mic audio, 16 kHz mono int16 LE, after the AFE (echo-cancelled, gain-leveled)
//                text:   {"ev":"hello",...} on connect, {"ev":"state",...} when the arm's job changes,
//                        {"ev":"cmd",...} reply to each command, {"ev":"spoke"} when a reply finished playing,
//                        {"ev":"stop"} when "stop" was heard on the board (it already stopped the arm)
//   PC -> board  binary: speech to play, same format as the mic
//                text:   {"cmd":"hold this for me","id":7}  any router text (router.c), run as the app
//                        {"hush":true}  cut the speech     {"led":"idle|listen|think"}
//                        {"attn":true}  perk up (idle life)     {"listening":bool}  lean in, head cocked
//                        {"volume":0..100}     {"ping":true}  keep-alive (also refreshes the socket's LRU slot)
// One PC at a time: a new connection replaces the old one.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "sdkconfig.h"

// 1 on the Johnny build, for use in plain C expressions (a bool Kconfig that is off is not defined at all)
#ifdef CONFIG_XARM_JOHNNY
#define JOHNNY 1
#else
#define JOHNNY 0
#endif

void pc_register(httpd_handle_t h);     // web.c: adds /ws
void pc_sock_closed(int fd);            // web.c close hook: that socket is gone
bool pc_connected(void);
void pc_mic(const int16_t *pcm, int n); // voice.c: AFE output, every fetch
void pc_stopped(void);                  // router.c "stop": cut the speech, tell the PC
bool pc_speaking(void);                 // speech playing or queued
float pc_talk_level(void);              // loudness of the speech playing, 0..1 (pick.c nods along)
void pc_json(cJSON *o);                 // /api/state "brain"
