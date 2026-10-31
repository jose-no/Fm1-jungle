/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jose (jose-no) */
/* JUNGLE: one breakbeat, loaded from the web page, sliced into 16ths and rearranged in time as it plays.
 * Part of fm1-base's unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are
 * in scope. The engine is engine.c (its own unit, -O2); the break's store and upload are brk.c, included here.
 *
 * The panel:
 *   PLAY start / stop        REC a new pattern         SEQ  CHOP / SPEED      ARP fill now (held)
 *   HOME / EDIT / FX pages   SAVE the settings        GLO  tap tempo         SEL the break's own tempo
 *   OCT- / OCT+ tempo -1 / +1   SELECT tempo   ALGO fills   PRESET swing   K1..K4 the page's four
 *   white keys: play on from that sixteenth of the break, while held
 *   black keys, while held: repeat 4 / 2 / 1 steps, roll, reverse, octave down, octave up, tape stop, mute, fill,
 *   the break straight
 *   MIDI in: clock, start, stop and continue; notes 36..51 as the white keys, 60..70 as the black keys; CC 20..31 the
 *   twelve page knobs, CC 14 the tempo (60 + value) */
#include "fastmath.h"
#include "engine.h"
#include "brk.c"

#define SET_MAGIC 0x4A4E4731u            /* "JNG1" */
typedef struct {
    uint32_t magic;
    int16_t par[NPAR];
    uint8_t rsv[2];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

enum { PG_HOME, PG_EDIT, PG_FX, NPG };
static const char *const PG_NAME[NPG] = {"HOME", "EDIT", "FX"};
static const char *const PAR_NAME[NPAR] = {"CHAOS", "EVOLVE", "PITCH", "GATE", "SHUFFLE", "STUTTER", "REVERSE",
                                           "DROP", "TONE", "DRIVE", "CRUSH", "SWING", "BPM", "FILL", "MODE"};
static const int16_t PAR_LO[NPAR] = {0, 0, -24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 600, 0, 0};
static const int16_t PAR_HI[NPAR] = {100, 100, 24, 100, 100, 100, 100, 100, 100, 100, 100, 60, 2400, 4, 1};
static const char *const FILL_NAME[5] = {"OFF", "16 BARS", "8 BARS", "4 BARS", "2 BARS"};
static const char *const MODE_NAME[2] = {"CHOP", "SPEED"};
static const char *const LIVE_NAME[NLIVE] = {"REPEAT 4", "REPEAT 2", "REPEAT 1", "ROLL", "REVERSE", "OCT DOWN",
                                             "OCT UP", "TAPE STOP", "MUTE", "FILL", "STRAIGHT"};

/* ------------------------------------------------------------- the break --- */
static const brk_hdr_t *hdr;             /* in the flash; 0 = none */
static eng_break_t eb;
static volatile uint8_t flash_busy;      /* an upload: the render leaves the break alone */
static int8_t wave_hi[236], wave_lo[236];   /* the overview, per column of the screen */
static uint32_t load_pct = 101;          /* an upload's progress (101: none) */
static uint8_t load_erasing;

static void wave_make(void)
{
    uint32_t x, i;
    const int16_t *p = eb.pcm;
    for (x = 0; x < 236u; x++) {
        uint32_t a = eb.len * x / 236u, e = eb.len * (x + 1u) / 236u;
        int32_t hi = 0, lo = 0;
        for (i = a; i < e; i++) {
            if (p[i] > hi) hi = p[i];
            if (p[i] < lo) lo = p[i];
        }
        wave_hi[x] = (int8_t)(hi * 24 / 32768);
        wave_lo[x] = (int8_t)(lo * 24 / 32768);
    }
}

static void break_take(const brk_hdr_t *h)
{
    uint32_t i;
    hdr = h;
    if (!h) {
        eng_load(0);
        return;
    }
    eb.pcm = (const int16_t *)plat_data(BRK_PCM_OFF);
    eb.len = h->len;
    eb.rate = h->rate;
    eb.bpm10 = h->bpm10;
    eb.bars = h->bars;
    eb.nsl = h->nsl;
    eb.snare = h->snare;
    for (i = 0; i < ENG_MAXSL; i++)
        eb.start[i] = i < h->nsl ? h->start[i] : 0u;
    wave_make();
    eng_load(&eb);
}

/* brk.c's hooks */
static void redraw_all(void);
static void draw_header(void);
void brk_app_unload(void)
{
    flash_busy = 1;                          /* (the render, an interrupt, sees it from its next block) */
    hdr = 0;
    eng_load(0);
}
void brk_app_loaded(const brk_hdr_t *h)
{
    load_pct = 101;
    if (h) {
        set.par[P_BPM] = (int16_t)h->bpm10;  /* a new break plays at its own tempo */
        eng_par[P_BPM] = h->bpm10;
    }
    break_take(h);
    flash_busy = 0;
    app_say(h ? "LOADED" : "LOAD FAILED", h ? h->name : "TRY AGAIN");
    redraw_all();
}
void brk_app_progress(uint32_t done, uint32_t total, int erasing)
{
    uint32_t pct = total ? done * 100u / total : 0u;
    if (pct == load_pct && (uint8_t)erasing == load_erasing)
        return;
    load_pct = pct;
    load_erasing = (uint8_t)erasing;
    draw_header();                           /* now: the main loop is busy with the upload */
}

/* ----------------------------------------------------------------- sound --- */
static float mono[APP_BLOCK];
static float gain_now;

static volatile uint32_t midi_keys;      /* MIDI notes held as keys: bits as plat_keys' (white then black order) */
static volatile int16_t midi_cc[NPAR];
static volatile uint32_t midi_cc_seen;

static void midi_in(void)                /* render: the packets that came in */
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24, key = 99;
        if (st >= 0xF8u) {
            eng_midi(st);
            continue;
        }
        if ((st & 0xE0u) == 0x80u) {         /* notes: 36..51 white keys, 60..70 black keys */
            if (d1 >= 36u && d1 < 52u)
                key = WK[d1 - 36u];
            else if (d1 >= 60u && d1 < 71u)
                key = BK[d1 - 60u];
            if (key < NKEYS) {
                if ((st & 0xF0u) == 0x90u && d2)
                    midi_keys |= 1u << key;
                else
                    midi_keys &= ~(1u << key);
            }
        } else if ((st & 0xF0u) == 0xB0u) {
            if (d1 >= 20u && d1 < 32u) {     /* the twelve page knobs */
                uint32_t p = d1 - 20u;
                midi_cc[p] = (int16_t)(PAR_LO[p] + (int32_t)d2 * (PAR_HI[p] - PAR_LO[p]) / 127);
                midi_cc_seen |= 1u << p;
            } else if (d1 == 14u) {
                midi_cc[P_BPM] = (int16_t)((60 + (int32_t)d2) * 10);
                midi_cc_seen |= 1u << P_BPM;
            }
        }
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    float gain = (float)master_q12 / 4096.0f * 8000000.0f;
    midi_in();
    if (flash_busy || brk_loading()) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    eng_render(mono, n);
    for (i = 0; i < n; i++) {
        float s;
        gain_now += (gain - gain_now) * 0.004f;
        s = mono[i] * gain_now;
        s = s > 8388000.0f ? 8388000.0f : s < -8388000.0f ? -8388000.0f : s;
        lr[2 * i] = (int32_t)s;
        lr[2 * i + 1] = (int32_t)s;
    }
}

void app_panic(void)
{
    eng_play(0);
    flash_busy = 1;                          /* an update is about to rewrite the flash */
}

int app_sysex(const uint8_t *p, uint32_t n) { return brk_sysex(p, n); }

/* -------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    uint32_t i;
    eng_init(0x1234567u);
    set.magic = SET_MAGIC;
    for (i = 0; i < NPAR; i++)
        set.par[i] = (int16_t)eng_par[i];
}

static void settings_apply(void)
{
    uint32_t i;
    for (i = 0; i < NPAR; i++)
        eng_par[i] = clamp(set.par[i], PAR_LO[i], PAR_HI[i]);
}

void app_init(void)
{
    settings_defaults();
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC)
        settings_defaults();
    settings_apply();
    break_take(brk_boot());
    if (hdr && plat_store_size(OBJ_SETTINGS) < 0)
        eng_par[P_BPM] = set.par[P_BPM] = (int16_t)hdr->bpm10;
}

/* -------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    int page;
    int32_t last_step, last_pos;
    uint32_t live;
    int white;                           /* the white key playing (-1 none) */
    uint32_t tap[4], ntap;
    uint32_t oct_t;                      /* OCT- / OCT+ held: repeats */
    uint32_t touched;                    /* params turned (bits) since the last draw */
    uint32_t touched_at;
    int32_t touched_par;
    uint8_t redraw, redraw_params;
} ui = {.page = 0, .white = -1, .last_step = -2, .touched_par = -1};

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void bpm_text(uint32_t b10, char *t)  /* 170.0 */
{
    char f[4];
    int_text((int32_t)(b10 / 10u), t);
    f[0] = '.';
    f[1] = (char)('0' + b10 % 10u);
    f[2] = 0;
    text_cat(t, f);
}

static void par_text(uint32_t p, char *t)
{
    int32_t v = eng_par[p];
    t[0] = 0;
    if (p == P_FILL)
        text_cat(t, FILL_NAME[clamp(v, 0, 4)]);
    else if (p == P_MODE)
        text_cat(t, MODE_NAME[v != 0]);
    else if (p == P_BPM)
        bpm_text((uint32_t)v, t);
    else if (p == P_PITCH) {
        if (v > 0)
            text_cat(t, "+");
        int_text(v, t + (v > 0));
    } else if (p == P_TONE && v >= 100)
        text_cat(t, "OPEN");
    else
        int_text(v, t);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void par_set(uint32_t p, int32_t v, int say)
{
    char t[16];
    v = clamp(v, PAR_LO[p], PAR_HI[p]);
    if (v == eng_par[p])
        return;
    eng_par[p] = v;
    set.par[p] = (int16_t)v;
    set_dirty = 1;
    ui.touched_par = (int32_t)p;
    ui.touched_at = plat_ms();
    ui.redraw_params = 1;
    if (say) {
        par_text(p, t);
        app_say(PAR_NAME[p], t);
    }
}

static void tap_tempo(void)
{
    uint32_t now = plat_ms(), sum;
    if (ui.ntap && now - ui.tap[(ui.ntap - 1u) & 3u] > 2000u)
        ui.ntap = 0;                         /* a pause: a new count */
    ui.tap[ui.ntap & 3u] = now;
    ui.ntap++;
    if (ui.ntap < 2u) {
        app_say("TAP", "AGAIN");
        return;
    }
    {
        uint32_t k = ui.ntap < 4u ? ui.ntap : 4u;
        uint32_t first = ui.tap[(ui.ntap - k) & 3u];
        sum = now - first;
        if (sum)
            par_set(P_BPM, (int32_t)(600000u * (k - 1u) / sum), 1);
    }
}

static void input(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t keys = plat_keys() | midi_keys, btn = plat_buttons(), k, live = 0, seen;
    int white = -1;
    int32_t e;
    /* white keys: the last one pressed and still held plays */
    for (k = 0; k < 16u; k++) {
        uint32_t bit = 1u << WK[k];
        if ((keys & bit) && !(ui.keys & bit))
            ui.white = (int)k;
    }
    if (ui.white >= 0 && (keys >> WK[ui.white] & 1u))
        white = ui.white;
    else
        for (k = 0; k < 16u; k++)
            if (keys >> WK[k] & 1u)
                white = (int)k;
    ui.white = white;
    eng_slice_key(white);
    for (k = 0; k < 11u; k++)
        if (keys >> BK[k] & 1u)
            live |= 1u << k;
    if (btn >> B_ARP & 1u)
        live |= 1u << LV_FILL;
    for (k = 0; k < NLIVE; k++)
        if ((live >> k & 1u) && !(ui.live >> k & 1u))
            app_say(LIVE_NAME[k], "");
    ui.live = live;
    eng_live(live);

    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_PLAY:
            if (!hdr)
                app_say("NO BREAK", "LOAD ONE");
            else {
                int on = eng_playing() == 0;
                eng_play(on);
                app_say(on ? (eng_ext() ? "WAITING" : "PLAY") : "STOP", on && eng_ext() ? "FOR THE CLOCK" : "");
            }
            break;
        case B_REC:
            eng_regen();
            app_say("NEW PATTERN", "");
            break;
        case B_SEQ:
            par_set(P_MODE, !eng_par[P_MODE], 1);
            break;
        case B_HOME:
        case B_EDIT:
        case B_FX:
            ui.page = k == B_HOME ? PG_HOME : k == B_EDIT ? PG_EDIT : PG_FX;
            ui.redraw_params = 1;
            app_say(PG_NAME[ui.page], "");
            break;
        case B_SAVE:
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else
                app_say("SAVE", "FAILED");
            break;
        case B_GLO:
            tap_tempo();
            break;
        case B_SEL:
            if (hdr)
                par_set(P_BPM, (int32_t)hdr->bpm10, 1);
            break;
        case B_OCTDN:
        case B_OCTUP:
            par_set(P_BPM, eng_par[P_BPM] + (k == B_OCTUP ? 10 : -10), 1);
            ui.oct_t = plat_ms() + 400u;
            break;
        default:
            break;
        }
    }
    if ((btn & (1u << B_OCTDN | 1u << B_OCTUP)) && ui.oct_t && (int32_t)(plat_ms() - ui.oct_t) >= 0) {
        par_set(P_BPM, eng_par[P_BPM] + (btn >> B_OCTUP & 1u ? 10 : -10), 1);
        ui.oct_t = plat_ms() + 80u;
    }

    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (k == EN_SELECT)
            par_set(P_BPM, eng_par[P_BPM] + e * 10, 1);
        else if (k == EN_ALGO)
            par_set(P_FILL, eng_par[P_FILL] + e, 1);
        else if (k == EN_PRESET)
            par_set(P_SWING, eng_par[P_SWING] + e, 1);
        else {
            uint32_t p = (uint32_t)ui.page * 4u + (k - EN_K1);
            int32_t step = p == P_PITCH ? 1 : 2;
            par_set(p, eng_par[p] + e * step, 1);
        }
    }
    seen = midi_cc_seen;                     /* the knobs from MIDI */
    if (seen) {
        midi_cc_seen = 0;
        for (k = 0; k < NPAR; k++)
            if (seen >> k & 1u)
                par_set(k, midi_cc[k], 0);
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
}

void app_input_only(void) { input(); }

/* the lights: PLAY, the page, SAVE when unsaved; the keys show where in the break it plays */
static void lights(void)
{
    static const uint8_t WK[16] = PLAT_WHITE_KEYS, BK[11] = PLAT_BLACK_KEYS;
    uint32_t b = 0, kl = 0, k, n;
    int st = eng_step();
    eng_pattern(&n);
    if (eng_playing())
        b |= 1u << B_PLAY;
    if (eng_playing() == 2 && (plat_ms() & 256u))
        b &= ~(1u << B_PLAY);
    b |= 1u << (ui.page == PG_HOME ? B_HOME : ui.page == PG_EDIT ? B_EDIT : B_FX);
    if (set_dirty)
        b |= 1u << B_SAVE;
    if (eng_par[P_MODE])
        b |= 1u << B_SEQ;
    if (ui.btn >> B_ARP & 1u)
        b |= 1u << B_ARP;
    if (st >= 0 && n) {
        const eng_ev_t *p = eng_pattern(&n);
        int sl = p[st].sl;
        if (ui.white >= 0)
            kl |= 1u << WK[ui.white];
        else if (sl >= 0)
            kl |= 1u << WK[(uint32_t)sl * 16u / n];   /* the slice playing, on the white keys */
    }
    for (k = 0; k < 11u; k++)
        if (ui.live >> k & 1u)
            kl |= 1u << BK[k];
    plat_glow(1);
    plat_leds(b, kl);
}

/* ------------------------------------------------------------------ draw --- */
#define WAVE_Y 34
#define GRID_Y 92
#define PAR_Y 162

static void draw_header(void)
{
    char t[32];
    uint32_t now = plat_ms();
    cv_begin(240, 32, C_BLACK);
    if (load_pct <= 100u) {
        int32_t w = (int32_t)load_pct * 220 / 100;
        cv_text(10, 2, &FONT_B, load_erasing ? "ERASING" : "LOADING", C_WHITE);
        int_text((int32_t)load_pct, t);
        text_cat(t, "%");
        cv_text(230 - text_w(&FONT_B, t), 2, &FONT_B, t, C_HI);
        cv_rect(10, 24, 220, 4, C_LINE);
        cv_rect(10, 24, w, 4, C_HI);
    } else if (now < ui.msg_until) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, APP_NAME, C_WHITE), y = 4;
        if (eng_ext())
            cv_text(x + 8, 10, &FONT_XS, "EXT", C_HI);
        if (eng_playing() == 2)
            cv_text(x + 34, 10, &FONT_XS, "WAIT", (now & 256u) ? C_WHITE : C_DIM);
        else if (eng_in_fill())
            cv_text(x + 34, 10, &FONT_XS, "FILL", C_WHITE);
        bpm_text(eng_ext() ? eng_bpm10() : (uint32_t)eng_par[P_BPM], t);
        cv_text(230 - text_w(&FONT_M, t), y, &FONT_M, t, C_HI);
    }
    cv_blit(0, 0);
}

static void draw_wave(void)
{
    char t[40];
    uint32_t i, x;
    int32_t pos = eng_voice_pos();
    cv_begin(240, 56, C_BLACK);
    if (!hdr) {
        cv_text(10, 8, &FONT_B, "NO BREAK", C_HI);
        cv_text(10, 30, &FONT_XS, "LOAD ONE FROM THE WEB PAGE", C_GRAY);
    } else {
        cv_text(2, 0, &FONT_XS, hdr->name, C_AMB);
        {
            char b[8];
            bpm_text(hdr->bpm10, b);
            t[0] = 0;
            int_text((int32_t)hdr->bars, t);
            text_cat(t, hdr->bars == 1u ? " BAR  " : " BARS  ");
            text_cat(t, b);
        }
        cv_text(238 - text_w(&FONT_XS, t), 0, &FONT_XS, t, C_GRAY);
        for (x = 0; x < 236u; x++) {
            int32_t hi = wave_hi[x], lo = wave_lo[x];
            cv_rect(2 + (int32_t)x, 31 - hi, 1, hi - lo + 1, C_DIM);
        }
        for (i = 0; i < hdr->nsl; i++) {      /* the slices: a tick each, taller on the beats */
            int32_t sx = 2 + (int32_t)(hdr->start[i] * 236u / hdr->len);
            cv_rect(sx, i % 4u ? 52 : 48, 1, i % 4u ? 3 : 7, (i & 15u) == 0u ? C_HI : C_GRAY);
        }
        if (pos >= 0 && (uint32_t)pos < hdr->len)
            cv_rect(2 + (int32_t)((uint32_t)pos * 236u / hdr->len), 12, 2, 38, C_WHITE);
    }
    cv_blit(0, WAVE_Y);
}

/* the pattern: a cell a step, rows of 16 (a bar each); a moved slice lit, a rest empty, reversed in the amber,
 * retriggers as notches, the step playing in white */
static void draw_grid(void)
{
    uint32_t n, i, rows;
    const eng_ev_t *p = eng_pattern(&n);
    int st = eng_step();
    int32_t ch;
    cv_begin(240, 66, C_BLACK);
    if (!n) {
        cv_blit(0, GRID_Y);
        return;
    }
    rows = n / 16u;
    ch = rows == 1u ? 40 : rows == 2u ? 30 : 15;
    for (i = 0; i < n; i++) {
        const eng_ev_t *e = p + i;
        int32_t x = 4 + (int32_t)(i % 16u) * 14 + ((i % 16u) / 4u), y = (int32_t)(i / 16u) * (ch + 2) + 2;
        uint16_t c = e->sl < 0 ? C_BLACK : e->flags & EV_REV ? C_AMB : e->flags & EV_MOVED || e->pitch ? C_HI : C_DIM;
        if ((int)i == st)
            c = C_WHITE;
        cv_rect(x, y, 12, ch, C_LINE);
        if (e->sl >= 0)
            cv_rect(x + 1, y + 1, 10, ch - 2, c);
        else if ((int)i == st)
            cv_rect(x + 1, y + ch - 3, 10, 2, C_WHITE);
        if (e->rep > 1u) {                   /* the retriggers: notches */
            uint32_t r;
            for (r = 1; r < e->rep; r++)
                cv_rect(x + 1, y + (int32_t)r * ch / (int32_t)e->rep, 10, 1, C_BLACK);
        }
    }
    cv_blit(0, GRID_Y);
}

static void draw_params(void)
{
    char t[20];
    uint32_t k, now = plat_ms();
    cv_begin(240, 78, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    cv_text(4, 4, &FONT_XS, PG_NAME[ui.page], C_HI);
    t[0] = 0;
    text_cat(t, MODE_NAME[eng_par[P_MODE] != 0]);
    cv_text(60, 4, &FONT_XS, t, C_GRAY);
    t[0] = 0;
    text_cat(t, "FILL ");
    text_cat(t, FILL_NAME[clamp(eng_par[P_FILL], 0, 4)]);
    cv_text(116, 4, &FONT_XS, t, C_GRAY);
    if (ui.page != PG_FX) {
        t[0] = 0;
        text_cat(t, "SW ");
        int_text(eng_par[P_SWING], t + 3);
        cv_text(236 - text_w(&FONT_XS, t), 4, &FONT_XS, t, C_GRAY);
    }
    for (k = 0; k < 4u; k++) {
        uint32_t p = (uint32_t)ui.page * 4u + k;
        int32_t x = 4 + (int32_t)k * 59, v = eng_par[p], w;
        int lit = ui.touched_par == (int32_t)p && now - ui.touched_at < 500u;
        cv_text(x, 24, &FONT_XS, PAR_NAME[p], lit ? C_WHITE : C_GRAY);
        par_text(p, t);
        cv_text(x, 40, &FONT_B, t, lit ? C_WHITE : C_HI);
        w = (v - PAR_LO[p]) * 52 / (PAR_HI[p] - PAR_LO[p]);
        cv_rect(x, 64, 52, 3, C_LINE);
        cv_rect(x, 64, w, 3, lit ? C_WHITE : C_HI);
    }
    cv_blit(0, PAR_Y);
}

static void redraw_all(void)
{
    ui.redraw = 1;
    ui.redraw_params = 1;
    ui.last_step = -2;
}

void app_ui_init(void)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    redraw_all();
}

void app_frame(void)
{
    static uint32_t last_hdr_key, last_wave_t;
    uint32_t now = plat_ms(), hk;
    int st;
    int32_t pos;
    input();
    ann_tick(now);
    lights();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.touched_par >= 0 && now - ui.touched_at >= 500u && now - ui.touched_at < 600u)
        ui.redraw_params = 1;
    hk = (eng_ext() ? eng_bpm10() : (uint32_t)eng_par[P_BPM]) | (uint32_t)eng_playing() << 16 |
         (uint32_t)eng_ext() << 18 | (uint32_t)eng_in_fill() << 19 | ((now >> 8) & 1u) << 20;
    if (hk != last_hdr_key || ui.redraw) {
        last_hdr_key = hk;
        draw_header();
    }
    st = eng_step();
    pos = eng_voice_pos();
    if (st != ui.last_step || ui.redraw) {
        ui.last_step = st;
        draw_grid();
    }
    if ((pos != ui.last_pos && now - last_wave_t >= 30u) || ui.redraw) {
        ui.last_pos = pos;
        last_wave_t = now;
        draw_wave();
    }
    if (ui.redraw_params || ui.redraw) {
        ui.redraw_params = 0;
        draw_params();
    }
    ui.redraw = 0;
}

/* ------------------------------------------------------------- host tests --- */
#ifdef PLAT_HOST
#include <math.h>
static uint8_t host_reply[96];
static uint32_t host_reply_n, host_replies;
void brk_host_reply(const uint8_t *f, uint32_t n)
{
    host_replies++;
    host_reply_n = n < sizeof host_reply ? n : sizeof host_reply;
    memcpy(host_reply, f, host_reply_n);
}
static int host_status(void)                 /* the last reply's status (its first unpacked byte) */
{
    if (host_reply_n < 8u)
        return -1;
    return host_reply[6] | ((host_reply[5] & 1) << 7);
}

/* a frame as the web page sends it: 7D 4A 42 cmd, then the payload packed 7 bytes to 8 */
static int host_send(uint32_t cmd, const uint8_t *d, uint32_t n)
{
    static uint8_t f[700];
    uint32_t o = 0, i, k;
    f[o++] = 0x7D;
    f[o++] = 0x4A;
    f[o++] = 0x42;
    f[o++] = (uint8_t)cmd;
    for (i = 0; i < n; i += 7u) {
        uint32_t g = n - i > 7u ? 7u : n - i, hi = 0;
        for (k = 0; k < g; k++)
            hi |= (uint32_t)(d[i + k] >> 7) << k;
        f[o++] = (uint8_t)hi;
        for (k = 0; k < g; k++)
            f[o++] = d[i + k] & 0x7Fu;
    }
    if (!app_sysex(f, o))
        return -1;
    return host_status();
}

/* a synthetic break: kicks, snares and hats at BPM, BARS bars at RATE */
static int16_t host_pcm[BRK_CAP / 2u];
static int host_upload(double bpm, uint32_t bars, uint32_t rate, int corrupt)
{
    brk_hdr_t h;
    double spb = rate * 60.0 / bpm / 4.0;
    uint32_t i, st, off;
    int rc;
    static const char *BAR = "K.H.S..HH.KHS.H.";
    memset(&h, 0, sizeof h);
    h.magic = BRK_MAGIC;
    h.version = BRK_VERSION;
    h.len = (uint32_t)(spb * 16.0 * bars);
    h.rate = rate;
    h.bpm10 = (uint32_t)(bpm * 10.0 + 0.5);
    h.bars = bars;
    h.nsl = bars * 16u;
    h.snare = 4;
    snprintf(h.name, sizeof h.name, "TEST %u", (unsigned)(bpm + 0.5));
    if (h.len * 2u > BRK_CAP)
        return -9;
    memset(host_pcm, 0, sizeof host_pcm);
    for (st = 0; st < h.nsl; st++) {
        char c = BAR[st & 15u];
        uint32_t s0 = (uint32_t)(st * spb + 0.5);
        h.start[st] = s0;
        for (i = 0; i < (uint32_t)spb && s0 + i < h.len; i++) {
            double t = (double)i / rate, v = 0;
            if (c == 'K') v = sin(2 * M_PI * (50 + 100 * exp(-t * 40)) * t) * exp(-t * 12);
            if (c == 'S') v = ((rand() & 0xFFFF) / 32768.0 - 1.0) * exp(-t * 25) * 0.7 + sin(2 * M_PI * 190 * t) * exp(-t * 30) * 0.4;
            if (c != '.') v += ((rand() & 0xFFFF) / 32768.0 - 1.0) * exp(-t * 300) * 0.15;
            host_pcm[s0 + i] = (int16_t)(v * 20000);
        }
    }
    h.crc = brk_crc32(0, (const uint8_t *)host_pcm, h.len * 2u);
    if ((rc = host_send(2, (const uint8_t *)&h, sizeof h)) != BRK_OK)
        return rc;
    for (off = 0; off < h.len * 2u; off += BRK_CHUNK) {
        uint8_t d[4 + BRK_CHUNK];
        uint32_t n = h.len * 2u - off < BRK_CHUNK ? h.len * 2u - off : BRK_CHUNK;
        d[0] = (uint8_t)off;
        d[1] = (uint8_t)(off >> 8);
        d[2] = (uint8_t)(off >> 16);
        d[3] = (uint8_t)(off >> 24);
        memcpy(d + 4, (const uint8_t *)host_pcm + off, n);
        if (corrupt && off == 4480u)
            d[10] ^= 0x55u;
        if ((rc = host_send(3, d, 4 + n)) != BRK_OK)
            return rc;
    }
    return host_send(4, 0, 0);
}

static int host_last_rc = -100;
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)d;
    if (!strcmp(cmd, "upload")) {            /* upload BPM BARS RATE [corrupt] */
        host_last_rc = host_upload(atof(a), (uint32_t)atoi(b), (uint32_t)atoi(c), !strcmp(d, "corrupt"));
        printf("  upload %s %s %s -> %d\n", a, b, c, host_last_rc);
        return 1;
    }
    if (!strcmp(cmd, "hello")) {
        host_send(1, 0, 0);
        return 1;
    }
    if (!strcmp(cmd, "sysex_bad")) {         /* a frame that is not ours: refused */
        static const uint8_t f[] = {0x7D, 0x11, 0x22, 0x01};
        printf("  foreign frame taken: %d\n", app_sysex(f, sizeof f));
        return 1;
    }
    return 0;
}

int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "upload_rc"))
        *got = host_last_rc;
    else if (!strcmp(what, "has_break"))
        *got = hdr != 0;
    else if (!strcmp(what, "bpm"))
        *got = eng_par[P_BPM];
    else if (!strcmp(what, "playing"))
        *got = eng_playing();
    else if (!strcmp(what, "step"))
        *got = eng_step();
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "chaos"))
        *got = eng_par[P_CHAOS];
    else if (!strcmp(what, "par")) {
        *got = 0;
        return 0;
    } else if (!strncmp(what, "par", 3))
        *got = eng_par[atoi(what + 3) % NPAR];
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "ext"))
        *got = eng_ext();
    else if (!strcmp(what, "live"))
        *got = (int)ui.live;
    else if (!strcmp(what, "white"))
        *got = ui.white;
    else if (!strcmp(what, "replies"))
        *got = (int)host_replies;
    else if (!strcmp(what, "hello_has"))     /* the last HELLO's "a break is there" */
        *got = host_reply_n > 12u ? host_reply[11] : -1;
    else
        return 0;
    return 1;
}
#endif
