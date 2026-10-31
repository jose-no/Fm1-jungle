/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jose (jose-no) */
/* JUNGLE: a breakbeat slicer for the M-VAVE FM-1. The app's half of the contract with fm1-base (device/unity.c,
 * host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "JUNGLE"
#define APP_TAGLINE "BREAKBEAT SLICER"
#define APP_SPLASH_RGB RGB(120, 230, 90)
#define APP_PALETTE 0u                   /* green */
#define APP_BLOCK 128u                   /* frames per audio block: 2.9 ms (a MIDI clock is read once a block) */
#define APP_SYSEX                        /* the break's upload (brk.c) */

/* the store: the settings in Felucca's settings pair, the panel calibration below the break's samples */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
int app_sysex(const uint8_t *p, uint32_t n);

/* the host simulator's checks and commands (tests/basic.txt) */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
#define APP_HOST_CMD
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d);
