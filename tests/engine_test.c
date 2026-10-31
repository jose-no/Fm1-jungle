/* SPDX-License-Identifier: GPL-3.0-only
 * The engine on its own, against a synthetic break (a kick, a snare and hats at 170 BPM, two bars at 44.1 kHz):
 *   cc -O2 -Ibase/include -Iapp tests/engine_test.c app/engine.c -lm -o build/engine_test && build/engine_test [OUT.wav]
 * Checks: at CHAOS 0 the break plays back sample for sample; the tempo and swing land where they should; a MIDI
 * clock drives it; the mutations, fills, live keys and the output chain stay in range. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  ok "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BPM 170.0
#define SR 44100
static int16_t pcm[SR * 4];
static uint32_t plen;
static eng_break_t b;

static void make_break(uint32_t rate)
{
    double spb = rate * 60.0 / BPM / 4.0;         /* samples per 16th */
    uint32_t i, st;
    /* kick on 0, 10; snare on 4, 12; hats on 2, 7, 8, 11, 14 (per bar) */
    static const char *BAR = "K.H.S..HH.KHS.H.";
    plen = (uint32_t)(spb * 32.0);
    memset(pcm, 0, sizeof pcm);
    for (st = 0; st < 32; st++) {
        char c = BAR[st & 15];
        uint32_t s0 = (uint32_t)(st * spb + 0.5);
        for (i = 0; i < (uint32_t)spb && s0 + i < plen; i++) {
            double t = (double)i / rate, v = 0;
            if (c == 'K') v = sin(2 * M_PI * (50 + 100 * exp(-t * 40)) * t) * exp(-t * 12);
            if (c == 'S') v = ((rand() & 0xFFFF) / 32768.0 - 1.0) * exp(-t * 25) * 0.7 + sin(2 * M_PI * 190 * t) * exp(-t * 30) * 0.4;
            if (c == 'H' || c == 'K' || c == 'S') v += ((rand() & 0xFFFF) / 32768.0 - 1.0) * exp(-t * 300) * 0.15;
            pcm[s0 + i] = (int16_t)(v * 20000);
        }
    }
    memset(&b, 0, sizeof b);
    b.pcm = pcm;
    b.len = plen;
    b.rate = rate;
    b.bpm10 = (uint32_t)(BPM * 10);
    b.bars = 2;
    b.nsl = 32;
    b.snare = 4;
    for (st = 0; st < 32; st++)
        b.start[st] = (uint32_t)(st * spb + 0.5);
}

static float buf[SR * 64];
static FILE *wav;
static void wav_open(const char *p)
{
    wav = fopen(p, "wb");
    if (wav) fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x44\xAC\0\0\x88\x58\x01\0\x02\0\x10\0data\0\0\0\0", 1, 44, wav);
}
static void wav_put(const float *x, uint32_t n)
{
    uint32_t i;
    if (!wav) return;
    for (i = 0; i < n; i++) {
        float v = x[i] < -1 ? -1 : x[i] > 1 ? 1 : x[i];
        int16_t s = (int16_t)(v * 32767);
        fwrite(&s, 2, 1, wav);
    }
}
static void wav_close(void)
{
    long n;
    uint32_t v;
    if (!wav) return;
    n = ftell(wav);
    v = (uint32_t)(n - 8); fseek(wav, 4, SEEK_SET); fwrite(&v, 4, 1, wav);
    v = (uint32_t)(n - 44); fseek(wav, 40, SEEK_SET); fwrite(&v, 4, 1, wav);
    fclose(wav);
}

/* n samples in blocks of 128, the MIDI clock given at block starts when clk_period > 0 */
static double clk_period, clk_next;
static uint32_t t_abs;
static void run(float *o, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i += 128) {
        uint32_t m = n - i < 128 ? n - i : 128;
        while (clk_period > 0 && clk_next <= t_abs) {
            eng_midi(0xF8);
            clk_next += clk_period;
        }
        eng_render(o + i, m);
        t_abs += m;
    }
}

/* onsets: a 64-sample block much louder than the one before, then its first sample over a third of its peak */
static uint32_t onsets(const float *x, uint32_t n, uint32_t *at, uint32_t max)
{
    uint32_t b, i, c = 0;
    float prev = 0;
    for (b = 0; b + 64 <= n && c < max; b += 64) {
        float pk = 0;
        for (i = b; i < b + 64; i++) if (fabsf(x[i]) > pk) pk = fabsf(x[i]);
        if (pk > 0.02f && pk > prev * 3.0f + 0.01f) {
            uint32_t s0 = b >= 64 ? b - 64 : 0;
            for (i = s0; i < b + 64 && fabsf(x[i]) < pk / 3; i++) ;
            if (c == 0 || i - at[c - 1] > 400) at[c++] = i;
        }
        prev = pk;
    }
    return c;
}
static uint32_t nearest(const uint32_t *at, uint32_t k, uint32_t want)
{
    uint32_t i, best = 0;
    for (i = 0; i < k; i++) if (abs((int)at[i] - (int)want) < abs((int)best - (int)want)) best = at[i];
    return best;
}

int main(int argc, char **argv)
{
    uint32_t i, n, at[256], k;
    double maxerr = 0;
    const eng_ev_t *p;

    make_break(SR);
    eng_init(12345);
    eng_par[P_CHAOS] = 0;
    eng_par[P_FILL] = 0;
    eng_par[P_BPM] = (int32_t)(BPM * 10);
    eng_load(&b);
    run(buf, 128);
    CHECK(eng_loaded(), "the break loaded");
    p = eng_pattern(&n);
    CHECK(n == 32, "32 steps (%u)", n);

    /* CHAOS 0 at the break's tempo: the break, sample for sample (but for the first fade-in) */
    eng_play(1);
    t_abs = 0;
    run(buf, plen * 2);
    for (i = 200; i < plen * 2; i++) {
        double e = 9;
        int sh;
        for (sh = -2; sh <= 2; sh++) {
            double f = fabs(buf[i] - 0.9 * pcm[(i + plen + sh) % plen] / 32768.0);
            if (f < e) e = f;
        }
        uint32_t ph = i % plen, nearb = 0;
        for (k = 0; k < 32; k++)               /* a retrigger's crossfade is not an error */
            if (ph + 4 >= b.start[k] && ph < b.start[k] + 128) nearb = 1;
        if (!nearb && e > maxerr) maxerr = e;
    }
    CHECK(maxerr < 0.002, "CHAOS 0 plays the break as it is (max error %.5f)", maxerr);
    eng_play(0);
    run(buf, 2000);

    /* tempo: at 140 BPM the steps land 140 BPM apart */
    eng_par[P_BPM] = 1400;
    eng_play(1);
    run(buf, SR * 4);
    k = onsets(buf, SR * 4, at, 256);
    {
        double spb = SR * 60.0 / 140.0 / 4.0;   /* the snare: step 4, then 12 */
        uint32_t w4 = (uint32_t)(4 * spb), w12 = (uint32_t)(12 * spb), w20 = (uint32_t)(20 * spb);
        CHECK(abs((int)nearest(at, k, w4) - (int)w4) < 30 && abs((int)nearest(at, k, w12) - (int)w12) < 30 &&
              abs((int)nearest(at, k, w20) - (int)w20) < 30, "140 BPM: the snares on steps 4, 12, 20 (%u %u %u)",
              nearest(at, k, w4), nearest(at, k, w12), nearest(at, k, w20));
    }
    eng_play(0);
    run(buf, 2000);

    /* swing: odd steps late */
    eng_par[P_SWING] = 40;
    eng_par[P_BPM] = 1700;
    eng_play(1);
    run(buf, SR * 2);
    k = onsets(buf, SR * 2, at, 256);
    {
        double spb = SR * 60.0 / 170.0 / 4.0;
        /* step 7 (odd, a hat after silence) late by 0.2 of a step; step 10 (even, the kick) where it was */
        uint32_t w7 = (uint32_t)(7.2 * spb), w10 = (uint32_t)(10 * spb);
        CHECK(abs((int)nearest(at, k, w7) - (int)w7) < 30 && abs((int)nearest(at, k, w10) - (int)w10) < 30,
              "swing 40: step 7 late, at %u (want %u); step 10 at %u (want %u)", nearest(at, k, w7), w7,
              nearest(at, k, w10), w10);
    }
    eng_par[P_SWING] = 0;
    eng_play(0);
    run(buf, 2000);

    /* MIDI clock at 128 BPM: start, then the tempo follows */
    clk_period = SR * 60.0 / 128.0 / 24.0;
    clk_next = t_abs;
    run(buf, SR / 2);
    CHECK(eng_ext(), "the clock is followed");
    eng_midi(0xFA);
    run(buf, SR * 4);
    CHECK(eng_playing() == 1, "started on the clock");
    CHECK(abs((int)eng_bpm10() - 1280) <= 3, "clock tempo %u", eng_bpm10());
    {
        uint32_t c0 = eng_bar();
        run(buf, (uint32_t)(SR * 60.0 / 128.0 * 4 * 4));   /* four bars */
        CHECK(eng_bar() - c0 == 4, "four bars of clock, four bars played (%u)", eng_bar() - c0);
    }
    eng_midi(0xFC);
    run(buf, 2000);
    CHECK(eng_playing() == 0, "stopped by the clock's stop");
    clk_period = 0;
    run(buf, SR);
    CHECK(!eng_ext(), "the clock gone: our own again");

    /* chaos: changes the pattern, keeps everything in range, over a long run with every effect */
    wav_open(argc > 1 ? argv[1] : "/dev/null");
    eng_par[P_BPM] = 1700;
    eng_par[P_CHAOS] = 70;
    eng_par[P_EVOLVE] = 40;
    eng_par[P_FILL] = 3;
    eng_par[P_SWING] = 10;
    eng_play(1);
    {
        float peak = 0;
        uint32_t moved = 0, fills = 0, live, j;
        static const uint32_t LIVES[] = {0, 1u << LV_LOOP2, 0, 1u << LV_ROLL, 0, 1u << LV_REV, 0, 1u << LV_TAPE, 0,
                                         1u << LV_OCTDN, 0, 1u << LV_LOOP1, 0, 1u << LV_FILL, 0, 1u << LV_STRAIGHT, 0};
        uint32_t chunk = (uint32_t)(SR * 60.0 / 170.0);   /* a beat */
        for (j = 0; j < 64; j++) {
            live = j >= 16 && j < 16 + 17 * 2 ? LIVES[(j - 16) / 2] : 0;
            eng_live(live);
            if (j == 50) eng_slice_key(5);
            if (j == 52) eng_slice_key(-1);
            if (j == 40) { eng_par[P_DRIVE] = 60; eng_par[P_CRUSH] = 40; eng_par[P_TONE] = 60; eng_par[P_MODE] = 1; eng_par[P_GATE] = 40; }
            run(buf, chunk);
            wav_put(buf, chunk);
            for (i = 0; i < chunk; i++) {
                if (!isfinite(buf[i])) peak = 99;
                if (fabsf(buf[i]) > peak) peak = fabsf(buf[i]);
            }
            fills += eng_in_fill();
            p = eng_pattern(&n);
            for (i = 0; i < n; i++) moved += p[i].sl != (int8_t)i || p[i].rep != 1 || p[i].flags;
        }
        CHECK(peak < 1.0f && peak > 0.2f, "in range, every effect (peak %.3f)", peak);
        CHECK(moved > 50, "CHAOS 70 rearranges (%u steps changed over the run)", moved);
        CHECK(fills > 0, "fills come (%u beats in one)", fills);
    }
    wav_close();
    eng_play(0);

    /* a rate other than 44.1k, and unloading */
    make_break(22050);
    eng_par[P_CHAOS] = 0; eng_par[P_DRIVE] = 0; eng_par[P_CRUSH] = 0; eng_par[P_TONE] = 100; eng_par[P_MODE] = 0;
    eng_par[P_GATE] = 100; eng_par[P_SWING] = 0; eng_par[P_FILL] = 0;
    eng_load(&b);
    run(buf, 128);
    eng_play(1);
    run(buf, plen * 4);
    k = onsets(buf, plen * 4, at, 256);
    {
        uint32_t w = (uint32_t)(20 * SR * 60.0 / 170.0 / 4.0);
        CHECK(abs((int)nearest(at, k, w) - (int)w) < 30, "22.05 kHz break at 44.1 kHz: in time (%u, want %u)", nearest(at, k, w), w);
    }
    eng_load(0);
    run(buf, 256);
    run(buf, 4410);
    {
        float pk = 0;
        for (i = 0; i < 4410; i++) if (fabsf(buf[i]) > pk) pk = fabsf(buf[i]);
        CHECK(pk == 0.0f && eng_playing() == 0, "unloaded: silent");
    }
    printf("%s (%d failed)\n", fails ? "FAILED" : "all ok", fails);
    return fails != 0;
}
