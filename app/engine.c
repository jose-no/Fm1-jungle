/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jose (jose-no) */
/* JUNGLE's engine (engine.h): the clock, the pattern and its mutations, the slice voices and the output chain.
 * Everything here runs in the render; the main loop only writes eng_par and the request words.
 * No 64-bit multiplies (felucca-bsp docs/CPU.md): positions are floats, the RNG is 32-bit. */
#include "engine.h"
#include "fastmath.h"

volatile int32_t eng_par[NPAR];

/* ------------------------------------------------------------- requests --- */
static eng_break_t brk, brk_next;
static volatile int32_t load_req;            /* 1 = brk_next waits, 2 = unload; the render clears it */
static volatile int32_t play_req = -1;       /* 0 / 1 */
static volatile int32_t regen_req;
static volatile uint32_t live_mask;
static volatile int32_t slice_key = -1;
static int have;                             /* the render has a break */

/* ------------------------------------------------------------------ rng --- */
static uint32_t rng = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static float rndf(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }
static uint32_t rndn(uint32_t n) { return n ? (rnd() >> 8) % n : 0u; }

/* -------------------------------------------------------------- pattern --- */
static eng_ev_t pat[ENG_MAXSL], fillp[16];
static uint32_t beat_sig[ENG_MAXSL / 4];     /* the parameters each beat was made with */
static uint32_t nsteps = 16;

static uint32_t gen_sig(void)                /* what a beat depends on */
{
    uint32_t h = (uint32_t)eng_par[P_CHAOS];
    h = h * 101u + (uint32_t)eng_par[P_SHUFFLE];
    h = h * 101u + (uint32_t)eng_par[P_STUTTER];
    h = h * 101u + (uint32_t)eng_par[P_REVERSE];
    h = h * 101u + (uint32_t)eng_par[P_DROP];
    return h == ~0u ? 0u : h;
}

static void ev_orig(eng_ev_t *e, uint32_t s)
{
    e->sl = (int8_t)s;
    e->rep = 1;
    e->pitch = 0;
    e->rise = 0;
    e->flags = 0;
}

/* beat b (steps 4b .. 4b+3): the original, mutated as much as CHAOS says, in the ways the EDIT page weighs */
static void gen_beat(uint32_t b)
{
    static const int8_t COLOUR[] = {-12, -5, 5, 7, 12};
    uint32_t nb = nsteps / 4u, s0 = b * 4u, j;
    float p = (float)eng_par[P_CHAOS] * 0.01f;
    float wsh = (float)eng_par[P_SHUFFLE] * 0.01f, wst = (float)eng_par[P_STUTTER] * 0.01f;
    float wrv = (float)eng_par[P_REVERSE] * 0.01f, wdr = (float)eng_par[P_DROP] * 0.01f;
    eng_ev_t *e = pat + s0;
    for (j = 0; j < 4u; j++)
        ev_orig(e + j, s0 + j);
    if (p > 0.0f) {
        if (rndf() < p * wsh * 0.9f) {
            if (rndf() < 0.55f && nb > 1u) {     /* the whole beat from another */
                uint32_t src = rndn(nb - 1u);
                if (src >= b)
                    src++;
                for (j = 0; j < 4u; j++) {
                    e[j].sl = (int8_t)(src * 4u + j);
                    e[j].flags |= EV_MOVED;
                }
            } else {                              /* half of it from another half (the same place in a beat) */
                uint32_t h = rnd() & 1u, src = rndn(nb) * 2u + (rndf() < 0.7f ? h : 1u - h);
                for (j = 0; j < 2u; j++) {
                    e[h * 2u + j].sl = (int8_t)(src * 2u + j);
                    e[h * 2u + j].flags |= EV_MOVED;
                }
            }
        }
        if (rndf() < p * wsh * 0.15f) {           /* a ghost snare off the beat */
            j = 1u + rndn(3u);
            e[j].sl = (int8_t)brk.snare;
            e[j].flags |= EV_MOVED;
        }
        for (j = 0; j < 4u; j++) {
            if (rndf() < p * wst * 0.45f) {
                float r = rndf();
                if (r < 0.45f)
                    e[j].rep = 2;
                else if (r < 0.6f)
                    e[j].rep = 3;
                else if (r < 0.75f)
                    e[j].rep = 4;
                else if (j > 0u) {                /* the step before, again */
                    e[j].sl = e[j - 1u].sl;
                    e[j].flags |= EV_MOVED;
                } else
                    e[j].rep = 2;
            }
            if (rndf() < p * wst * 0.06f)
                e[j].pitch = COLOUR[rndn(5u)];
            if (rndf() < p * wrv * 0.3f)
                e[j].flags |= EV_REV;
            if (!(j == 0u && (b & 3u) == 0u) && rndf() < p * wdr * 0.3f)
                e[j].sl = -1;
        }
    }
    beat_sig[b] = gen_sig();
}

static void gen_all(void)
{
    uint32_t b;
    for (b = 0; b < nsteps / 4u; b++)
        gen_beat(b);
}

/* the fill bar: -2 = the pattern's step */
static uint32_t fill_n;
static void gen_fill(void)
{
    uint32_t i, t = rndn(5u), len = (t == 0u || t == 4u) && rndf() < 0.5f ? 8u : 4u, a = 16u - len;
    int8_t sn = (int8_t)brk.snare;
    for (i = 0; i < 16u; i++) {
        fillp[i].sl = -2;
        fillp[i].rep = 1;
        fillp[i].pitch = 0;
        fillp[i].rise = 0;
        fillp[i].flags = EV_FILL;
    }
    for (i = a; i < 16u; i++) {
        uint32_t k = i - a;                       /* 0 .. len-1 */
        eng_ev_t *e = fillp + i;
        switch (t) {
        case 0:                                   /* a snare roll that climbs */
            e->sl = sn;
            e->rep = (uint8_t)(k * 4u < len ? 1u : k * 2u < len ? 2u : 4u);
            e->pitch = (int8_t)(k * 6u / len);
            e->rise = e->rep > 2u ? 1 : 0;
            break;
        case 1: {                                 /* one hit, halving */
            static const uint8_t R[4] = {1, 2, 4, 4};
            e->sl = (int8_t)(k == 0u ? (int8_t)(rndn(nsteps / 4u) * 4u) : fillp[a].sl);
            e->rep = R[k];
            break;
        }
        case 2:                                   /* the last beat backwards */
            e->sl = (int8_t)(15u - k);
            e->flags |= EV_REV;
            break;
        case 3: {                                 /* the tape slows */
            static const int8_t P[4] = {-2, -4, -7, -12};
            e->sl = (int8_t)i;
            e->pitch = P[k];
            break;
        }
        default:                                  /* chopped up: snares and gaps */
            e->sl = rndf() < 0.2f ? -1 : rndf() < 0.6f ? sn : (int8_t)(rndn(nsteps / 4u) * 4u + rndn(4u));
            e->rep = (uint8_t)(rndf() < 0.3f ? 2u : 1u);
            break;
        }
    }
    fill_n = len;
}

/* ---------------------------------------------------------------- voices --- */
typedef struct {
    float pos, inc, amp, damp, trav, limit;
    int on;
} voice_t;
static voice_t vo[2];
static uint32_t vcur;
#define ATTACK 24.0f                              /* samples (0.5 ms) */
#define RELEASE 24.0f

static void voice_release(voice_t *v)
{
    if (v->on && v->damp >= 0.0f)
        v->damp = -1.0f / RELEASE;
}

static float bpm_now(void);
static float tape = 1.0f;
static float until_next(uint32_t sub);

static void voice_trig(const eng_ev_t *e, uint32_t sub)
{
    uint32_t sl, st, en, slen, lv = live_mask;
    float semis, inc;
    voice_t *v;
    int rev;
    if (!have || e->sl < 0 || (lv >> LV_MUTE & 1u)) {
        voice_release(&vo[vcur]);
        return;
    }
    sl = (uint32_t)e->sl % brk.nsl;
    st = brk.start[sl];
    en = sl + 1u < brk.nsl ? brk.start[sl + 1u] : brk.len;
    if (en <= st || st >= brk.len)
        return;
    slen = en - st;
    semis = (float)(eng_par[P_PITCH] + e->pitch + e->rise * (int32_t)sub);
    if (lv >> LV_OCTDN & 1u)
        semis -= 12.0f;
    if (lv >> LV_OCTUP & 1u)
        semis += 12.0f;
    inc = (float)brk.rate * (1.0f / (float)ENG_SR) * fm_exp2f(semis * (1.0f / 12.0f));
    if (eng_par[P_MODE] && brk.bpm10)
        inc *= bpm_now() * 10.0f / (float)brk.bpm10;
    rev = (e->flags & EV_REV) != 0;
    if (lv >> LV_REV & 1u)
        rev = !rev;
    voice_release(&vo[vcur]);                     /* the last one fades as this one starts */
    vcur ^= 1u;
    v = &vo[vcur];
    v->on = 1;
    v->amp = 0.0f;
    v->damp = 1.0f / ATTACK;
    v->trav = 0.0f;
    v->pos = rev ? (float)(en - 1u) : (float)st;
    v->inc = rev ? -inc : inc;
    v->limit = (float)slen * (0.08f + 0.92f * (float)eng_par[P_GATE] * 0.01f);   /* GATE 100: the whole slice */
    if (v->limit / inc < until_next(sub) - 2.0f)  /* it ends before the next trigger: faded by its end, so none of
                                                   * the next slice's hit is heard (a retrigger crossfades instead) */
        v->limit -= RELEASE * inc;
    if (v->limit < inc)
        v->limit = inc;
}

/* ----------------------------------------------------------------- clock --- */
static int playing;                               /* 0, 1, 2 waiting for the clock */
static uint32_t step_abs;                         /* steps since PLAY */
static float phase;                               /* in the step, 0..1 */
static float ps, sps = 4000.0f;                   /* samples into the step (phase = ps / sps), and in a step */
static uint32_t t_now;                            /* samples since the start */
static uint32_t clk_last;                         /* when the last clock came */
static float clk_avg;                             /* samples between clocks */
static int ext;                                   /* a clock is coming in */
static uint32_t ext_clocks;                       /* clocks since the downbeat */
static eng_ev_t cur;                              /* the step playing */
static uint32_t sub_next;
static float trig_at, sub_span;
static int in_fill;
static uint32_t lp_len, lp_abs, lp_src;           /* a beat repeat held: its length, when, from where */
static int32_t sk_last = -1;
static uint32_t sk_abs, sk_src;                   /* a white key held: when, from where */
static uint32_t live_prev;

static float bpm_now(void)
{
    if (ext && clk_avg > 0.0f)
        return (float)ENG_SR * 60.0f / (clk_avg * 24.0f);
    return (float)eng_par[P_BPM] * 0.1f;
}

static uint32_t fill_every(void)
{
    static const uint8_t F[5] = {0, 16, 8, 4, 2};
    int32_t f = eng_par[P_FILL];
    return F[f < 0 ? 0 : f > 4 ? 4 : f];
}

/* the step that plays at step_abs: the pattern, then the fill, the keys and the live effects over it */
static void resolve(uint32_t sabs, eng_ev_t *e)
{
    uint32_t s = sabs % nsteps, lv = live_mask, len, fe = fill_every(), bar = sabs / 16u;
    int32_t sk = slice_key;
    if (sk >= 0) {                                /* a white key: play on from its place */
        if (sk != sk_last) {
            sk_last = sk;
            sk_abs = sabs;
            sk_src = (uint32_t)sk * nsteps / 16u;
        }
        s = (sk_src + (sabs - sk_abs)) % nsteps;
    } else
        sk_last = -1;
    len = lv >> LV_LOOP1 & 1u ? 1u : lv >> LV_LOOP2 & 1u ? 2u : lv >> LV_LOOP4 & 1u ? 4u : 0u;
    if (len) {                                    /* a beat repeat: the last 1, 2 or 4 steps around again */
        if (len != lp_len) {
            lp_len = len;
            lp_abs = sabs;
            lp_src = s;
        }
        s = (lp_src + (sabs - lp_abs) % len) % nsteps;
    } else
        lp_len = 0;
    if (lv >> LV_STRAIGHT & 1u)
        ev_orig(e, s);
    else
        *e = pat[s];
    if ((lv >> LV_FILL & 1u) && !(live_prev >> LV_FILL & 1u))
        gen_fill();
    in_fill = 0;
    if ((lv >> LV_FILL & 1u) || (fe && bar % fe == fe - 1u)) {
        const eng_ev_t *f = fillp + (sabs & 15u);
        if (f->sl != -2) {
            *e = *f;
            in_fill = 1;
        }
    }
    live_prev = lv;
    if (sk >= 0)
        e->flags |= EV_LIVE;
    if (lv >> LV_ROLL & 1u && e->rep < 4u)
        e->rep = 4;
}

/* output samples from sub-trigger sub of the step playing to the next trigger (this step's or the next one's) */
static float until_next(uint32_t sub)
{
    float at = trig_at + (float)sub * sub_span;
    if (sub + 1u < cur.rep)
        return sub_span * sps;
    return (1.0f - at + (((step_abs + 1u) & 1u) ? (float)eng_par[P_SWING] * 0.005f : 0.0f)) * sps;
}

static void begin_step(uint32_t sabs)
{
    uint32_t s = sabs % nsteps, b = s / 4u, fe = fill_every();
    if (s == 0u && sabs) {                        /* the loop around again: EVOLVE redraws some beats */
        float ev = (float)eng_par[P_EVOLVE] * 0.01f;
        uint32_t i;
        for (i = 0; i < nsteps / 4u; i++)
            if (rndf() < ev)
                beat_sig[i] = ~0u;
    }
    if ((s & 3u) == 0u) {
        if (regen_req) {
            regen_req = 0;
            gen_all();
        } else if (beat_sig[b] != gen_sig())     /* changed since it was made (a knob, or EVOLVE) */
            gen_beat(b);
    }
    if ((sabs & 15u) == 0u && fe && (sabs / 16u) % fe == fe - 1u)
        gen_fill();
    resolve(sabs, &cur);
    trig_at = (sabs & 1u) ? (float)eng_par[P_SWING] * 0.005f : 0.0f;
    if (cur.rep < 1u)
        cur.rep = 1;
    sub_span = (1.0f - trig_at) / (float)cur.rep;
    sub_next = 0;
}

static void start_at_zero(void)
{
    rng ^= t_now * 2654435761u;                   /* a different pattern each time it starts */
    if (!rng)
        rng = 0x9E3779B9u;
    playing = 1;
    step_abs = 0;
    phase = ps = 0.0f;
    tape = 1.0f;
    begin_step(0);
}

static void stop_all(void)
{
    playing = 0;
    voice_release(&vo[0]);
    voice_release(&vo[1]);
}

void eng_midi(uint32_t st)
{
    if (st == 0xF8u) {
        uint32_t dt = t_now - clk_last;
        if (clk_last && dt > 20u && dt < 9000u)   /* 12 .. 5500 BPM worth of clocks: a sane one */
            clk_avg = clk_avg > 0.0f && ext ? clk_avg + ((float)dt - clk_avg) * 0.08f : (float)dt;
        clk_last = t_now ? t_now : 1u;
        if (!ext && playing == 1) {               /* a clock appears under us: follow it from where we are */
            ext_clocks = step_abs * 6u + (uint32_t)(phase * 6.0f);
            ext = 1;
            return;
        }
        ext = 1;
        if (playing == 2) {
            ext_clocks = 0;
            start_at_zero();
            return;
        }
        if (playing == 1) {
            uint32_t fs, i;
            float fp;
            ext_clocks++;
            fs = ext_clocks / 6u;
            fp = (float)(ext_clocks % 6u) * (1.0f / 6.0f);
            for (i = 0; step_abs < fs && i < 4u; i++) {   /* behind the clock: catch up to it */
                step_abs++;
                phase = ps = 0.0f;
                begin_step(step_abs);
            }
            if (step_abs < fs)                    /* far behind: jump */
                step_abs = fs;
            if (step_abs == fs && phase < fp) {
                phase = fp;
                ps = fp * sps;
            }
        }
    } else if (st == 0xFAu) {                     /* start: the next clock is the downbeat */
        stop_all();
        if (have)
            playing = 2;
    } else if (st == 0xFBu) {
        if (!playing && have)
            playing = 2;
    } else if (st == 0xFCu)
        stop_all();
}

/* ----------------------------------------------------------------- render --- */
static float lp1, lp2, hold_v;
static uint32_t hold_n;

void eng_render(float *out, uint32_t n)
{
    uint32_t i, k, lv = live_mask;
    float d, cap_ph = 2.0f, a = 1.0f, g = 1.0f, gn = 1.0f, q = 0.0f;
    uint32_t cap_step = 0, hold_len = 1;
    int32_t tone = eng_par[P_TONE], drive = eng_par[P_DRIVE], crush = eng_par[P_CRUSH];

    if (load_req) {
        uint32_t lr = (uint32_t)load_req;
        stop_all();
        vo[0].on = vo[1].on = 0;
        if (lr == 1u) {
            brk = brk_next;
            nsteps = brk.nsl < 4u ? 4u : brk.nsl & ~3u;
            if (brk.snare >= brk.nsl)
                brk.snare = 4u % brk.nsl;
            have = 1;
            gen_all();
            gen_fill();
        } else
            have = 0;
        load_req = 0;
    }
    if (play_req >= 0) {
        int on = play_req;
        play_req = -1;
        if (on && have)
            if (ext && t_now - clk_last < 22050u)
                playing = 2;
            else
                start_at_zero();
        else
            stop_all();
    }
    if (ext && t_now - clk_last > 22050u)        /* the clock stopped coming: our own again */
        ext = 0;

    d = ext ? 1.0f / (clk_avg * 6.0f) : (float)eng_par[P_BPM] * 0.1f * 4.0f / (60.0f * (float)ENG_SR);
    sps = 1.0f / d;
    if (ext) {
        cap_step = (ext_clocks + 1u) / 6u;
        cap_ph = (float)((ext_clocks + 1u) % 6u) * (1.0f / 6.0f);
    }
    if (tone < 100) {
        float fc = 60.0f * fm_exp2f((float)tone * 0.085f);
        a = 1.0f - fm_expf(-6.2831853f * fc / (float)ENG_SR);
    }
    if (drive > 0) {
        g = 1.0f + (float)drive * 0.07f;
        gn = 1.0f / fm_tanhf(g);
    }
    if (crush > 0) {
        hold_len = 1u + (uint32_t)crush * 15u / 100u;
        q = fm_exp2f(15.0f - (float)crush * 0.1f);
    }

    for (i = 0; i < n; i++) {
        float s = 0.0f;
        t_now++;
        if (playing == 1) {
            if (lv >> LV_TAPE & 1u)
                tape = tape > 0.0f ? tape - d * 0.25f : 0.0f;   /* to a stop over a beat */
            else
                tape = 1.0f;
            if (!ext || step_abs < cap_step || (ps + 1.0f) * d < cap_ph)
                ps += 1.0f;
            else if (step_abs == cap_step && phase < cap_ph)
                ps = cap_ph * sps;
            phase = ps * d;
            if (phase >= 1.0f) {
                ps -= sps;
                if (ps < 0.0f)
                    ps = 0.0f;
                phase = ps * d;
                step_abs++;
                begin_step(step_abs);
            }
            while (sub_next < cur.rep && phase >= trig_at + (float)sub_next * sub_span) {
                voice_trig(&cur, sub_next);
                sub_next++;
            }
        }
        for (k = 0; k < 2u; k++) {
            voice_t *v = &vo[k];
            int32_t idx;
            float fr, x;
            const int16_t *p;
            if (!v->on)
                continue;
            idx = (int32_t)v->pos;
            if (v->pos < 0.0f || idx + 1 >= (int32_t)brk.len) {
                v->on = 0;
                continue;
            }
            p = brk.pcm + idx;
            fr = v->pos - (float)idx;
            x = (float)p[0] + fr * (float)(p[1] - p[0]);
            s += x * v->amp;
            v->pos += v->inc * tape;
            v->trav += fm_fabsf(v->inc * tape);
            if (v->trav > v->limit)
                voice_release(v);
            v->amp += v->damp;
            if (v->amp >= 1.0f) {
                v->amp = 1.0f;
                v->damp = 0.0f;
            } else if (v->amp <= 0.0f && v->damp < 0.0f)
                v->on = 0;
        }
        s *= 1.0f / 32768.0f;
        if (crush > 0) {
            if (++hold_n >= hold_len) {
                hold_n = 0;
                hold_v = fm_floorf(s * q + 0.5f) / q;
            }
            s = hold_v;
        }
        if (drive > 0)
            s = fm_tanhf(s * g) * gn;
        if (tone < 100) {
            lp1 += a * (s - lp1);
            lp2 += a * (lp1 - lp2);
            s = lp2;
        }
        out[i] = s * 0.9f;
    }
}

/* ------------------------------------------------------------ the UI's side --- */
void eng_init(uint32_t seed)
{
    static const int32_t DEF[NPAR] = {35, 25, 0, 100, 70, 45, 20, 15, 100, 0, 0, 0, 1650, 2, 0};
    uint32_t i;
    for (i = 0; i < NPAR; i++)
        eng_par[i] = DEF[i];
    rng = seed ? seed : 0x9E3779B9u;
}

void eng_load(const eng_break_t *b)
{
    if (b) {
        brk_next = *b;
        load_req = 1;
    } else
        load_req = 2;
}
int eng_loaded(void) { return load_req == 0; }
void eng_play(int on) { play_req = on ? 1 : 0; }
void eng_regen(void) { regen_req = 1; }
void eng_live(uint32_t mask) { live_mask = mask; }
void eng_slice_key(int s) { slice_key = s; }
int eng_playing(void) { return playing; }
int eng_ext(void) { return ext; }
uint32_t eng_bpm10(void) { return (uint32_t)(bpm_now() * 10.0f + 0.5f); }
int eng_step(void) { return playing == 1 ? (int)(step_abs % nsteps) : -1; }
int eng_in_fill(void) { return in_fill; }
uint32_t eng_bar(void) { return step_abs / 16u; }
int32_t eng_voice_pos(void) { return vo[vcur].on ? (int32_t)vo[vcur].pos : -1; }
const eng_ev_t *eng_pattern(uint32_t *n)
{
    *n = have ? nsteps : 0u;
    return pat;
}
const eng_ev_t *eng_fill(void) { return fillp; }
