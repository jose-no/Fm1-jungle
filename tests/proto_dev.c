/* SPDX-License-Identifier: GPL-3.0-only
 * brk.c alone, as the FM-1 runs it, behind a stand-in for the platform: for the loader page's test (tests/loader_test.mjs).
 * Reads SysEx frames as hex lines on stdin, writes the replies the same way on stdout; "dump FILE" writes the data
 * area. cc -Iapp -Ibase/include tests/proto_dev.c -o build/proto_dev */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define PLAT_HOST_STUB
#include "plat.h"
static uint8_t flash[PLAT_DATA_HI - PLAT_DATA_LO];
const uint8_t *plat_data(uint32_t off) { return off >= PLAT_DATA_LO && off < PLAT_DATA_HI ? flash + off - PLAT_DATA_LO : 0; }
int plat_data_erase(uint32_t off) { if (off & 0xFFFu || !plat_data(off)) return -1; memset(flash + off - PLAT_DATA_LO, 0xFF, 0x1000); return 0; }
int plat_data_write(uint32_t off, const void *s, uint32_t n)
{
    uint32_t i;
    if (!plat_data(off) || off + n > PLAT_DATA_HI) return -1;
    for (i = 0; i < n; i++) flash[off - PLAT_DATA_LO + i] &= ((const uint8_t *)s)[i];
    return 0;
}
void plat_sysex_send(const uint8_t *f, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) printf("%02X", f[i]);
    printf("\n");
    fflush(stdout);
}
#include "brk.c"
static int loaded;
void brk_app_unload(void) {}
void brk_app_loaded(const brk_hdr_t *h) { loaded = h != 0; fprintf(stderr, "loaded: %s\n", h ? h->name : "(none)"); }
void brk_app_progress(uint32_t d, uint32_t t, int e) { (void)d, (void)t, (void)e; }
int main(void)
{
    static char line[4096];
    static uint8_t f[2048];
    memset(flash, 0xFF, sizeof flash);
    while (fgets(line, sizeof line, stdin)) {
        uint32_t n = 0;
        char *h = line;
        if (!strncmp(line, "dump ", 5)) {
            FILE *o = fopen(strtok(line + 5, "\n"), "wb");
            if (o) { fwrite(flash, 1, sizeof flash, o); fclose(o); }
            printf("dumped\n");
            fflush(stdout);
            continue;
        }
        while (h[0] && h[1] && h[0] != '\n' && n < sizeof f) {
            unsigned v;
            sscanf(h, "%2x", &v);
            f[n++] = (uint8_t)v;
            h += 2;
        }
        if (n >= 3 && f[0] == 0xF0 && f[n - 1] == 0xF7)
            brk_sysex(f + 1, n - 2);
    }
    return 0;
}
