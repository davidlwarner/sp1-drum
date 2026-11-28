#ifndef SP1_DSP_DRUM_VOICES_H
#define SP1_DSP_DRUM_VOICES_H

/**
 * @file drum_voices.h
 * @brief Pure C11 4-Voice Drum Synthesis Engine + Per-Track Channel Strip + Dub Siren
 *        for 48 kHz ARM Cortex-M4F (nRF52840).
 *
 * Real-time & Portability Contract:
 *  - Zero Zephyr headers, zero dynamic memory allocation, strict C11 / C++17 compatible.
 *  - Flash-resident 257-entry sine and West-Coast wavefolder LUTs (2056 B in .rodata, 0 B SRAM).
 *  - Single-precision FPU compliance: explicit 'f' float suffixes, zero double promotion,
 *    and zero runtime division ('/') inside per-sample render loops.
 *  - Per-track TPT 1-pole tone filter is bypassed (0 cycles) inside [0.45f..0.55f] detent.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sequencer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_DSP_FS_HZ                   48000.0f
#define SP1_DSP_INV_FS_HZ               0.0000208333333f    /* 1.0f / 48000.0f */
#define SP1_DSP_PHASE_SCALE             89478.4853333f      /* 2^32 / 48000.0f */
#define SP1_DSP_INV_65536               0.0000152587890625f /* 1.0f / 65536.0f */
#define SP1_DSP_INV_2147483648          4.6566128730773926e-10f /* 1.0f / 2^31 */
#define SP1_DSP_INV_127                 0.007874015748f     /* 1.0f / 127.0f */

#define SP1_DSP_LUT_SIZE                256U
#define SP1_DSP_LUT_TABLE_LEN           257U
#define SP1_DRUM_NUM_KITS               4U
#define SP1_DRUM_NUM_VOICE_PARAMS       4U

/* Strict upper bound on phase-modulation index scaling (< INT32_MAX) to prevent
 * C11 float-to-int32 overflow undefined behavior. */
#define SP1_DSP_MAX_MOD_PHASE_SCALE     2000000000.0f
#define SP1_DSP_MAX_FB_PHASE_SCALE      500000000.0f

#define SP1_DSP_HAT_NUM_OSC             6U
#define SP1_DSP_ENV_FLOOR               1.0e-5f
#define SP1_DSP_DENORMAL_FLOOR          1.0e-20f

/* Flash budget: 2 * 257 * 4 = 2056 bytes in .rodata Flash; 0 bytes SRAM. */
extern const float k_sine_lut_256[SP1_DSP_LUT_TABLE_LEN];
extern const float k_wavefold_lut_256[SP1_DSP_LUT_TABLE_LEN];

/* ── Division-Free Per-Sample Inline DSP Primitives ──────────────────────── */

static inline uint32_t sp1_hz_to_phase_inc(float freq_hz) {
    if (freq_hz <= 0.0f) {
        return 0U;
    }
    const float max_hz = SP1_DSP_FS_HZ * 0.45f;
    if (freq_hz > max_hz) {
        freq_hz = max_hz;
    }
    return (uint32_t)(freq_hz * SP1_DSP_PHASE_SCALE);
}

static inline float sp1_sine_lut(uint32_t phase) {
    const uint32_t idx = phase >> 24U;
    const uint32_t frac = (phase >> 8U) & 0xFFFFU;
    const float t = (float)frac * SP1_DSP_INV_65536;
    const float a = k_sine_lut_256[idx];
    const float b = k_sine_lut_256[idx + 1U];
    return a + (b - a) * t;
}

static inline float sp1_triangle_osc(uint32_t phase) {
    const uint32_t folded = (phase & 0x80000000U) ? (~phase) : phase;
    const float norm = (float)(folded >> 15U) * SP1_DSP_INV_65536; /* [0.0f, 1.0f) */
    return (norm * 2.0f) - 1.0f;
}

static inline float sp1_wavefold_lut(float x) {
    if (x <= -4.0f) {
        return k_wavefold_lut_256[0];
    }
    if (x >= 4.0f) {
        return k_wavefold_lut_256[256];
    }
    const float pos = (x + 4.0f) * 32.0f; /* 256.0f / 8.0f = 32.0f */
    uint32_t idx = (uint32_t)pos;
    if (idx >= SP1_DSP_LUT_SIZE) {
        idx = SP1_DSP_LUT_SIZE - 1U;
    }
    const float frac = pos - (float)idx;
    const float a = k_wavefold_lut_256[idx];
    const float b = k_wavefold_lut_256[idx + 1U];
    return a + (b - a) * frac;
}

static inline float sp1_soft_clip(float x) {
    if (x >= 1.5f) {
        return 1.0f;
    }
    if (x <= -1.5f) {
        return -1.0f;
    }
    const float x2 = x * x;
    return x * (1.0f - (0.14814815f * x2));
}

static inline float sp1_xorshift32_noise(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0U) {
        x = 0x9E3779B9U;
    }
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return (float)(int32_t)x * SP1_DSP_INV_2147483648;
}

/* ── Cytomic TPT State-Variable Filter (SVF) ─────────────────────────────── */

/* Static RAM: 24 bytes per SVF instance */
typedef struct {
    float ic1eq;
    float ic2eq;
    float a1;
    float a2;
    float a3;
    float k;
} sp1_svf_t;

static inline void sp1_svf_reset(sp1_svf_t *f) {
    if (f == NULL) {
        return;
    }
    f->ic1eq = 0.0f;
    f->ic2eq = 0.0f;
}

void sp1_svf_set_params(sp1_svf_t *f, float cutoff_hz, float q);

static inline void sp1_svf_process(sp1_svf_t *f,
                                   float v0,
                                   float *lp,
                                   float *bp,
                                   float *hp) {
    const float v3 = v0 - f->ic2eq;
    const float v1 = (f->a1 * f->ic1eq) + (f->a2 * v3);
    const float v2 = f->ic2eq + (f->a2 * f->ic1eq) + (f->a3 * v3);
    f->ic1eq = (2.0f * v1) - f->ic1eq;
    f->ic2eq = (2.0f * v2) - f->ic2eq;

#if !defined(__ARM_FP) || (__ARM_FP == 0)
    if (fabsf(f->ic1eq) < SP1_DSP_DENORMAL_FLOOR) {
        f->ic1eq = 0.0f;
    }
    if (fabsf(f->ic2eq) < SP1_DSP_DENORMAL_FLOOR) {
        f->ic2eq = 0.0f;
    }
#endif

    if (lp != NULL) {
        *lp = v2;
    }
    if (bp != NULL) {
        *bp = v1;
    }
    if (hp != NULL) {
        *hp = v0 - (f->k * v1) - v2;
    }
}

/* ── Per-Track Mixer Channel Strip State ─────────────────────────────────── */

/* Static RAM: 32 bytes per track (128 B for 4 tracks). */
typedef struct {
    float level_gain;       /* Linear mix gain [0.0f..1.15f], default 1.0f */
    float pan_l;            /* Stereo pan Left multiplier, default 1.0f */
    float pan_r;            /* Stereo pan Right multiplier, default 1.0f */
    float delay_send;       /* Dub Ping-Pong Delay send level [0.0f..1.0f] */
    float tone_g;           /* 1-pole filter coefficient in (0.0f, 1.0f) */
    float tone_z;           /* 1-pole filter state */
    uint8_t tone_mode;      /* 0 = flat bypass ([0.45..0.55]), 1 = LP, 2 = HP */
    uint8_t pad[3];
} sp1_track_strip_t;

/* ── 4 Drum Synthesis Voice Structures + Dub Siren ───────────────────────── */

/* Track 1 — Kick / Sub Voice. Static RAM: 64 bytes. */
typedef struct {
    uint32_t phase;
    float amp_env;
    float pitch_env;
    float click_env;
    float amp_decay_coeff;
    float pitch_decay_coeff;
    float click_decay_coeff;
    float base_freq_hz;
    float pitch_span_base_hz;
    float pitch_span_hz;
    float pd_depth_scale;
    float velocity_gain;
    float tune_param;       /* p0: [0.0..1.0], 0.50 = 45 Hz */
    float decay_param;      /* p1: [0.0..1.0], 0.45 default */
    float punch_param;      /* p2: [0.0..1.0], 0.50 default */
    float drive_param;      /* p3: [0.0..1.0], 0.35 default */
    bool active;
    uint8_t pad[3];
} sp1_kick_voice_t;

/* Track 2 — Snare / Rimshot Morphing Voice. Static RAM: 92 bytes. */
typedef struct {
    uint32_t phase_body;
    uint32_t phase_rim;
    uint32_t rng;
    sp1_svf_t wire_svf;
    float body_env;
    float noise_env;
    float knock_env;
    float body_decay_coeff;
    float noise_decay_coeff;
    float knock_decay_coeff;
    float body_freq_hz;
    float rim_freq_hz;
    float velocity_gain;
    float pitch_param;      /* p0: [0.0..1.0], 0.50 = 1.0x */
    float tone_morph;       /* p1: 0.0 = Rimshot .. 1.0 = Snare */
    float wire_decay_param; /* p2: [0.0..1.0], wire noise decay */
    float wire_filter_param;/* p3: [0.0..1.0], wire SVF cutoff */
    bool active;
    uint8_t pad[3];
} sp1_snare_voice_t;

/* Track 3 — 6-Oscillator Inharmonic Metallic Hi-Hat Voice. Static RAM: 112 bytes. */
typedef struct {
    uint32_t phase[SP1_DSP_HAT_NUM_OSC];
    uint32_t phase_inc[SP1_DSP_HAT_NUM_OSC];
    uint32_t rng;
    sp1_svf_t hp_svf;
    float amp_env;
    float amp_decay_coeff;
    float metal_gain;
    float noise_gain;
    float velocity_gain;
    float tune_param;       /* p0: [0.0..1.0], 0.50 = 1.0x */
    float decay_morph;      /* p1: 0.0 = Closed .. 1.0 = Open */
    float cutoff_param;     /* p2: [0.0..1.0], HP SVF cutoff */
    float noise_mix_param;  /* p3: [0.0..1.0], metal cluster <-> noise */
    bool active;
    uint8_t pad[3];
} sp1_hat_voice_t;

/* Track 4 — 2-Op FM / West-Coast Wavefold Perc Voice. Static RAM: 80 bytes. */
typedef struct {
    uint32_t carrier_phase;
    uint32_t mod_phase;
    float prev_mod;
    float amp_env;
    float pitch_env;
    float index_env;
    float amp_decay_coeff;
    float pitch_decay_coeff;
    float index_decay_coeff;
    float carrier_base_hz;
    float pitch_span_hz;
    float mod_ratio;
    float mod_phase_scale;  /* Clamped <= SP1_DSP_MAX_MOD_PHASE_SCALE (2.0e9f) */
    float fb_phase_scale;   /* Clamped <= SP1_DSP_MAX_FB_PHASE_SCALE  (5.0e8f) */
    float velocity_gain;
    float pitch_param;      /* p0: [0.0..1.0], 0.50 = nominal */
    float ratio_param;      /* p1: [0.0..1.0], harmonic <-> clangorous */
    float decay_param;      /* p2: [0.0..1.0], amp & pitch sweep */
    float timbre_morph;     /* p3: [0.0..1.0], FM depth & wavefold */
    bool active;
    uint8_t pad[3];
} sp1_perc_voice_t;

/* Live Jamaican Sound-System Dub Siren Voice (Function hold while playing).
 * Static RAM: 40 bytes. */
typedef struct {
    uint32_t osc_phase;
    uint32_t lfo_phase;
    uint32_t lfo_inc;
    float amp_env;
    float base_freq_hz;
    float lfo_depth_oct;
    float delay_send;
    float last_lfo_vis;     /* Normalized [0.0f..1.0f] LFO position for LED wave */
    uint8_t lfo_mode;       /* 0 = steady horn, 1 = smooth wail, 2 = 2-tone square */
    bool gate;
    bool active;
    uint8_t pad;
} sp1_dub_siren_t;

/* Complete 4-Voice Drum Synthesizer + 4 Channel Strips + Dub Siren State.
 * Static RAM: ~520 bytes total (zero dynamic allocation). */
typedef struct {
    sp1_kick_voice_t kick;
    sp1_snare_voice_t snare;
    sp1_hat_voice_t hat;
    sp1_perc_voice_t perc;
    sp1_track_strip_t strips[SP1_SEQ_NUM_TRACKS];
    sp1_dub_siren_t siren;
} sp1_drum_synth_t;

/* ── Public 4-Voice Drum Synth + Channel Strip + Dub Siren API ───────────── */

void sp1_drum_voices_init(sp1_drum_synth_t *synth);

void sp1_drum_voices_load_kit(sp1_drum_synth_t *synth, uint8_t kit_idx);

void sp1_drum_voices_trigger(sp1_drum_synth_t *synth,
                             uint8_t track,
                             uint8_t velocity,
                             bool accent);

/* Macro voice setters (Mode 3 unshifted F1–F4: preserve custom Pitch/Filter edits) */
void sp1_drum_voices_set_kick_params(sp1_drum_synth_t *synth,
                                     float decay_0_to_1,
                                     float drive_0_to_1);
void sp1_drum_voices_set_snare_morph(sp1_drum_synth_t *synth,
                                     float morph_0_rim_to_1_snare);
void sp1_drum_voices_set_hat_decay(sp1_drum_synth_t *synth,
                                   float decay_0_closed_to_1_open);
void sp1_drum_voices_set_perc_timbre(sp1_drum_synth_t *synth,
                                     float timbre_0_to_1);

/* Deep 4-parameter per-voice sound design (Mode 3 Hold T_x + F1–F4) */
void sp1_drum_voices_set_voice_param(sp1_drum_synth_t *synth,
                                     uint8_t track,
                                     uint8_t param_idx,
                                     float norm_0_to_1);
float sp1_drum_voices_get_voice_param(const sp1_drum_synth_t *synth,
                                      uint8_t track,
                                      uint8_t param_idx);

/* Per-track mixer channel strip (Mode 2: Level, Pan, Dub Delay Send, Tone Filter) */
void sp1_drum_voices_set_track_mix(sp1_drum_synth_t *synth,
                                   uint8_t track,
                                   float level_0_to_1,
                                   float pan_0_to_1,
                                   float delay_send_0_to_1,
                                   float tone_0_to_1);

/* Live Dub Siren control (Function hold during playback) */
void sp1_drum_voices_set_siren(sp1_drum_synth_t *synth,
                               bool gate,
                               float pitch_0_to_1,
                               float lfo_rate_0_to_1,
                               float lfo_shape_0_to_1,
                               float delay_send_0_to_1);
float sp1_drum_voices_get_siren_lfo_level(const sp1_drum_synth_t *synth);

/**
 * Render one block (up to 96 frames = 2.0 ms @ 48 kHz) of the 4 drum voices + Dub Siren
 * with sample-accurate intra-block trigger dispatch matching triggers[k].block_offset.
 */
void sp1_drum_voices_render_block(sp1_drum_synth_t *synth,
                                  const sp1_seq_trigger_event_t *triggers,
                                  uint32_t num_triggers,
                                  float *out_main_l,
                                  float *out_main_r,
                                  float *out_send_l,
                                  float *out_send_r,
                                  uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* SP1_DSP_DRUM_VOICES_H */
