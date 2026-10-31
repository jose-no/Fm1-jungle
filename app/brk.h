/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jose (jose-no) */
/* The break in the flash, and its upload from the web page over USB-MIDI SysEx (docs/PROTOCOL.md).
 *
 * The data area (plat.h) holds one break: a header sector at BRK_HDR_OFF, then the samples (mono, 16-bit, little
 * endian) from BRK_PCM_OFF up to the panel calibration's sectors at 0xDC000. The header is written last, so a
 * break that was not uploaded whole is never played. Uses plat_data*, plat_sysex_send and three hooks of the app's. */
#pragma once
#include <stdint.h>

#define BRK_HDR_OFF 0x97000u
#define BRK_PCM_OFF 0x98000u
#define BRK_PCM_END 0xDC000u
#define BRK_CAP (BRK_PCM_END - BRK_PCM_OFF)  /* 278528 bytes: 139264 samples, 4.3 s at 32 kHz, 6.3 s at 22.05 kHz */
#define BRK_MAGIC 0x4B52424Au                /* "JBRK" */
#define BRK_VERSION 1u
#define BRK_MAXSL 64u
#define BRK_CHUNK 448u                        /* sample bytes in one DATA frame (64 groups of 7: 512 bytes packed) */

typedef struct {
    uint32_t magic, version;
    uint32_t len;                             /* samples */
    uint32_t rate;                            /* Hz, 8000 .. 48000 */
    uint32_t bpm10;                           /* tempo, tenths of a BPM, 600 .. 2400 */
    uint32_t bars;                            /* 1, 2 or 4 */
    uint32_t nsl;                             /* bars * 16 */
    uint32_t snare;                           /* the slice most like a snare */
    uint32_t crc;                             /* CRC-32 of the samples' bytes */
    char name[24];                            /* upper case, 0-terminated */
    uint32_t start[BRK_MAXSL];                /* each slice's first sample */
    uint32_t hcrc;                            /* CRC-32 of everything above */
} brk_hdr_t;                                  /* 320 bytes */

/* status codes in the replies */
enum { BRK_OK, BRK_E_FRAME, BRK_E_SIZE, BRK_E_HEADER, BRK_E_FLASH, BRK_E_CRC, BRK_E_ORDER };

uint32_t brk_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
int brk_check(const brk_hdr_t *h);            /* BRK_OK: a header that makes sense (not its CRCs) */
const brk_hdr_t *brk_boot(void);              /* the break in the flash, checked whole (samples too); 0 = none */
int brk_sysex(const uint8_t *p, uint32_t n);  /* a SysEx frame (between F0 and F7): 1 = ours, answered */
int brk_loading(void);                        /* an upload has begun and not ended */

/* the app's hooks */
void brk_app_unload(void);                    /* stop reading the break now (the flash is about to change) */
void brk_app_loaded(const brk_hdr_t *h);      /* a new break, whole (h: in the flash) */
void brk_app_progress(uint32_t done, uint32_t total, int erasing);   /* bytes so far (erasing: before the first) */
