/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jose (jose-no) */
/* JUNGLE's engine: one breakbeat, cut into 16th-note slices, played back in time and rearranged as it goes.
 *
 * Pure C with no platform calls, so the host simulator and the FM-1 run the same code. The render (the audio
 * interrupt) owns all of the engine's state; the UI writes eng_par and the request flags, and reads what the
 * eng_* getters return. One break at a time: eng_load hands the engine a description of it (the samples are read in
 * place, from the flash on the FM-1). */
#pragma once
#include <stdint.h>

#define ENG_SR 44100u
#define ENG_MAXSL 64u                    /* slices: 16 a bar, up to 4 bars */

typedef struct {
    const int16_t *pcm;                  /* mono, 16-bit */
    uint32_t len;                        /* samples */
    uint32_t rate;                       /* its sample rate, 8000 .. 48000 */
    uint32_t bpm10;                      /* its tempo, in tenths of a BPM */
    uint32_t bars;                       /* 1, 2 or 4 */
    uint32_t nsl;                        /* slices = bars * 16 */
    uint32_t snare;                      /* the slice most like a snare (the loader's guess): the fills use it */
    uint32_t start[ENG_MAXSL];           /* each slice's first sample (ascending) */
} eng_break_t;

/* one step of the pattern */
enum { EV_REV = 1, EV_MOVED = 2, EV_FILL = 4, EV_LIVE = 8 };
typedef struct {
    int8_t sl;                           /* the slice to play, -1 = a rest */
    uint8_t rep;                         /* 1..4: retriggers in the step (a ratchet) */
    int8_t pitch;                        /* semitones */
    int8_t rise;                         /* semitones added at each retrigger (a roll that climbs) */
    uint8_t flags;                       /* EV_* */
} eng_ev_t;

/* the parameters, 0..100 unless noted (written by the UI, read by the render) */
enum {
    P_CHAOS, P_EVOLVE, P_PITCH, P_GATE,          /* HOME page   (PITCH: -24..+24 semitones) */
    P_SHUFFLE, P_STUTTER, P_REVERSE, P_DROP,     /* EDIT page: what the chaos does */
    P_TONE, P_DRIVE, P_CRUSH, P_SWING,           /* FX page    (SWING: 0..60) */
    P_BPM,                                       /* tenths of a BPM, 600 .. 2400 */
    P_FILL,                                      /* 0 off, 1 every 16 bars, 2: 8, 3: 4, 4: 2 */
    P_MODE,                                      /* 0 CHOP (the slices keep their pitch), 1 SPEED (repitched to the tempo) */
    NPAR
};
extern volatile int32_t eng_par[NPAR];

/* live keys (black keys, held): bits */
enum { LV_LOOP4, LV_LOOP2, LV_LOOP1, LV_ROLL, LV_REV, LV_OCTDN, LV_OCTUP, LV_TAPE, LV_MUTE, LV_FILL, LV_STRAIGHT,
       NLIVE };

void eng_init(uint32_t seed);
void eng_load(const eng_break_t *b);     /* main loop: the break to play from the next block (0: none; it is copied) */
int eng_loaded(void);                    /* the render has the break (or has let go of it) */
void eng_render(float *out, uint32_t n); /* render: n mono frames, about -1..1 */
void eng_midi(uint32_t status);          /* render: a real-time message in (F8 clock, FA start, FB continue, FC stop) */

void eng_play(int on);                   /* main loop: start / stop (with a MIDI clock coming in: wait for it) */
void eng_regen(void);                    /* a new pattern at the next step */
void eng_live(uint32_t mask);            /* the black keys held (LV_* bits) */
void eng_slice_key(int s);               /* a white key held: this slice at every step (-1 none) */

int eng_playing(void);                   /* 0 stopped, 1 playing, 2 waiting for the clock */
int eng_ext(void);                       /* a MIDI clock is driving it */
uint32_t eng_bpm10(void);                /* the tempo in tenths of a BPM (the clock's, with one) */
int eng_step(void);                      /* the step playing in the pattern (0 .. nsl-1), -1 stopped */
int eng_in_fill(void);
uint32_t eng_bar(void);                  /* bars since PLAY */
int32_t eng_voice_pos(void);             /* the sample the voice is at (-1 none) */
const eng_ev_t *eng_pattern(uint32_t *n);/* the pattern (n steps) */
const eng_ev_t *eng_fill(void);          /* the fill bar's 16 steps */
