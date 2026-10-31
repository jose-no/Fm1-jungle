/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jose (jose-no) */
/* The break in the flash and its upload (brk.h, docs/PROTOCOL.md). Main loop only (plat_data_write's rule). */
#include "brk.h"
#include "plat.h"

#define SX0 0x7Du                              /* non-commercial ID, then "JB" */
#define SX1 0x4Au
#define SX2 0x42u
enum { C_HELLO = 1, C_BEGIN, C_DATA, C_END, C_HEADER };

static brk_hdr_t up;                           /* the header of the upload in progress */
static uint8_t up_on;
static uint32_t up_done;                       /* bytes written (the highest end) */
static uint8_t raw[600];

uint32_t brk_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static uint32_t tab[256];
    uint32_t i, j;
    if (!tab[1])
        for (i = 0; i < 256u; i++) {
            uint32_t c = i;
            for (j = 0; j < 8u; j++)
                c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
    crc = ~crc;
    while (n--)
        crc = tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

int brk_check(const brk_hdr_t *h)
{
    uint32_t i;
    if (h->magic != BRK_MAGIC || h->version != BRK_VERSION)
        return BRK_E_HEADER;
    if (h->len < 1000u || h->len * 2u > BRK_CAP)
        return BRK_E_SIZE;
    if (h->rate < 8000u || h->rate > 48000u || h->bpm10 < 600u || h->bpm10 > 2400u)
        return BRK_E_HEADER;
    if ((h->bars != 1u && h->bars != 2u && h->bars != 4u) || h->nsl != h->bars * 16u || h->snare >= h->nsl)
        return BRK_E_HEADER;
    for (i = 0; i < h->nsl; i++)
        if (h->start[i] >= h->len || (i && h->start[i] <= h->start[i - 1u]))
            return BRK_E_HEADER;
    if (h->name[sizeof h->name - 1u])
        return BRK_E_HEADER;
    return BRK_OK;
}

static uint32_t hdr_crc(const brk_hdr_t *h) { return brk_crc32(0, (const uint8_t *)h, (uint32_t)((const uint8_t *)&h->hcrc - (const uint8_t *)h)); }

static uint32_t pcm_crc(uint32_t bytes)
{
    const uint8_t *p = plat_data(BRK_PCM_OFF);
    return p ? brk_crc32(0, p, bytes) : 0u;
}

const brk_hdr_t *brk_boot(void)
{
    const brk_hdr_t *h = (const brk_hdr_t *)plat_data(BRK_HDR_OFF);
    if (!h || brk_check(h) != BRK_OK || h->hcrc != hdr_crc(h) || pcm_crc(h->len * 2u) != h->crc)
        return 0;
    return h;
}

int brk_loading(void) { return up_on; }

/* --------------------------------------------------------------- SysEx --- */
static uint32_t unpack7(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t max)
{
    uint32_t o = 0;
    while (n) {
        uint32_t g = n > 8u ? 8u : n, k;
        uint8_t hi = s[0];
        for (k = 1; k < g && o < max; k++)
            d[o++] = (uint8_t)(s[k] | ((hi >> (k - 1u)) & 1u) << 7);
        s += g;
        n -= g;
    }
    return o;
}

static void reply(uint32_t cmd, const uint8_t *d, uint32_t n)
{
    uint8_t f[96];
    uint32_t o = 0, i;
    f[o++] = 0xF0;
    f[o++] = SX0;
    f[o++] = SX1;
    f[o++] = SX2;
    f[o++] = (uint8_t)(0x40u | cmd);
    for (i = 0; i < n && o + 9u < sizeof f; i += 7u) {
        uint32_t g = n - i > 7u ? 7u : n - i, k, hi = 0;
        for (k = 0; k < g; k++)
            hi |= (uint32_t)(d[i + k] >> 7) << k;
        f[o++] = (uint8_t)hi;
        for (k = 0; k < g; k++)
            f[o++] = d[i + k] & 0x7Fu;
    }
    f[o++] = 0xF7;
    plat_sysex_send(f, o);
#ifdef PLAT_HOST
    {
        extern void brk_host_reply(const uint8_t *f, uint32_t n);
        brk_host_reply(f, o);
    }
#endif
}

static void put32(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16);
    d[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *s) { return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24; }

static void status(uint32_t cmd, int st, uint32_t extra)
{
    uint8_t d[5];
    d[0] = (uint8_t)st;
    put32(d + 1, extra);
    reply(cmd, d, 5);
}

static void hello(void)
{
    uint8_t d[64];
    const brk_hdr_t *h = up_on ? 0 : brk_boot();
    uint32_t i;
    for (i = 0; i < sizeof d; i++)
        d[i] = 0;
    d[0] = BRK_VERSION;
    put32(d + 1, BRK_CAP);
    d[5] = h != 0;
    if (h) {
        put32(d + 6, h->len);
        put32(d + 10, h->rate);
        put32(d + 14, h->bpm10);
        put32(d + 18, h->bars);
        put32(d + 22, h->crc);
        for (i = 0; i < 23u && h->name[i]; i++)
            d[26 + i] = (uint8_t)h->name[i];
    }
    reply(C_HELLO, d, 50);
}

static int write_header(const brk_hdr_t *h)
{
    brk_hdr_t c = *h;
    c.hcrc = hdr_crc(&c);
    if (plat_data_erase(BRK_HDR_OFF) || plat_data_write(BRK_HDR_OFF, &c, sizeof c))
        return BRK_E_FLASH;
    return BRK_OK;
}

static int got_header(const uint8_t *d, uint32_t n, brk_hdr_t *h)
{
    uint32_t i;
    uint8_t *o = (uint8_t *)h;
    if (n < sizeof *h)
        return BRK_E_FRAME;
    for (i = 0; i < sizeof *h; i++)
        o[i] = d[i];
    return brk_check(h);
}

int brk_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t cmd, dn;
    int st;
    if (n < 4u || p[0] != SX0 || p[1] != SX1 || p[2] != SX2)
        return 0;
    cmd = p[3];
    dn = unpack7(p + 4, n - 4u, raw, sizeof raw);
    switch (cmd) {
    case C_HELLO:
        hello();
        break;
    case C_BEGIN: {
        uint32_t off, end;
        if ((st = got_header(raw, dn, &up)) != BRK_OK) {
            status(cmd, st, 0);
            break;
        }
        brk_app_unload();
        up_on = 1;
        up_done = 0;
        brk_app_progress(0, up.len * 2u, 1);
        end = BRK_PCM_OFF + ((up.len * 2u + 0xFFFu) & ~0xFFFu);
        st = plat_data_erase(BRK_HDR_OFF) ? BRK_E_FLASH : BRK_OK;   /* the old break gone first */
        for (off = BRK_PCM_OFF; st == BRK_OK && off < end; off += 0x1000u)
            if (plat_data_erase(off))
                st = BRK_E_FLASH;
        if (st != BRK_OK)
            up_on = 0;
        brk_app_progress(0, up.len * 2u, 0);
        status(cmd, st, 0);
        break;
    }
    case C_DATA: {
        uint32_t off, len;
        if (!up_on) {
            status(cmd, BRK_E_ORDER, 0);
            break;
        }
        if (dn < 5u) {
            status(cmd, BRK_E_FRAME, 0);
            break;
        }
        off = get32(raw);
        len = dn - 4u;
        if (len > BRK_CHUNK || off + len > up.len * 2u) {
            status(cmd, BRK_E_SIZE, off);
            break;
        }
        st = plat_data_write(BRK_PCM_OFF + off, raw + 4, len) ? BRK_E_FLASH : BRK_OK;
        if (off + len > up_done)
            up_done = off + len;
        brk_app_progress(up_done, up.len * 2u, 0);
        status(cmd, st, off);
        break;
    }
    case C_END:
        if (!up_on) {
            status(cmd, BRK_E_ORDER, 0);
            break;
        }
        {
            uint32_t c = pcm_crc(up.len * 2u);
            st = c != up.crc ? BRK_E_CRC : write_header(&up);
            up_on = 0;
            if (st == BRK_OK && brk_boot())
                brk_app_loaded(brk_boot());
            else
                brk_app_loaded(0);
            status(cmd, st, c);
        }
        break;
    case C_HEADER: {                            /* new slices, the same samples: only the header is rewritten */
        const brk_hdr_t *h = brk_boot();
        brk_hdr_t nh;
        if (up_on || !h) {
            status(cmd, BRK_E_ORDER, 0);
            break;
        }
        if ((st = got_header(raw, dn, &nh)) == BRK_OK && (nh.len != h->len || nh.crc != h->crc))
            st = BRK_E_CRC;
        if (st == BRK_OK) {
            brk_app_unload();
            st = write_header(&nh);
            brk_app_loaded(brk_boot());
        }
        status(cmd, st, 0);
        break;
    }
    default:
        status(cmd & 0x3Fu, BRK_E_FRAME, 0);
        break;
    }
    return 1;
}
