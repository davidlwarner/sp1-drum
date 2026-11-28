#ifndef SP1_DSP_MASTER_FX_H
#define SP1_DSP_MASTER_FX_H

/**
 * @file master_fx.h
 * @brief Pure C11 Stereo Master Performance FX Bus for 48 kHz ARM Cortex-M4F (nRF52840).
 *
 * Includes:
 *  1. Tape-Style Pitch-Gliding Dub Ping-Pong Delay (16 KB static SRAM = 4,096 int16_t
 *     samples/tap @ 8 kHz 6x warm tape decimation = 512 ms/tap, 1,024 ms ping-pong,
 *     with fractional Doppler pitch-shift glide locking into musical tempo divisions).
 *  2. Tempo-Synced Beat-Repeat / Stutter Looper (32 KB static SRAM = 8,192 stereo int16_t
 *     frames, quantized 1/4 down to 1/64 ratchet with 64-frame boundary crossfading).
 *  3. DJ-Style Resonant Cytomic TPT SVF Isolator (center bypass deadzone [0.45..0.55]).
 *  4. Zero-Order Hold Sample-Rate Decimator / Bitcrusher + Division-Free Master Soft-Clipper.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp/drum_voices.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 8,192 stereo int16_t frames = 16,384 int16_t samples = 32,768 B (32.0 KB) static SRAM */
#define SP1_FX_STUTTER_FRAMES           8192U
#define SP1_FX_STUTTER_MASK             (SP1_FX_STUTTER_FRAMES - 1U)
#define SP1_FX_STUTTER_XFADE_FRAMES     64U
#define SP1_FX_STUTTER_INV_XFADE        0.015625f   /* 1.0f / 64.0f */
#define SP1_FX_STUTTER_MIN_LOOP_FRAMES  128U
#define SP1_FX_STUTTER_MAX_LOOP_FRAMES  (SP1_FX_STUTTER_FRAMES - SP1_FX_STUTTER_XFADE_FRAMES - 4U)

/* 4,096 mono int16_t samples per L/R tap = 8,192 int16_t samples = 16,384 B (16.0 KB) static SRAM.
 * Clocked at 8 kHz (6x warm analog tape decimation), providing 512 ms/tap (1,024 ms ping-pong)
 * while keeping total firmware static SRAM safely < 115 KB (< 90% of the 128 KB CI gate). */
#define SP1_FX_DELAY_TAP_SAMPLES        4096U
#define SP1_FX_DELAY_TAP_MASK           (SP1_FX_DELAY_TAP_SAMPLES - 1U)
#define SP1_FX_DELAY_DECIM_FACTOR       6U

/* DJ Isolator center bypass deadzone [0.45f, 0.55f] */
#define SP1_FX_DJ_DEADZONE_LOW          0.45f
#define SP1_FX_DJ_DEADZONE_HIGH         0.55f

typedef enum {
    SP1_STUTTER_OFF      = 0,
    SP1_STUTTER_DIV_1_4  = 1,   /* 1/4 note beat repeat */
    SP1_STUTTER_DIV_1_8  = 2,   /* 1/8 note beat repeat */
    SP1_STUTTER_DIV_1_16 = 3,   /* 1/16 note beat repeat */
    SP1_STUTTER_DIV_1_32 = 4,   /* 1/32 note ratchet */
    SP1_STUTTER_DIV_1_64 = 5    /* 1/64 note fast buzz ratchet */
} sp1_stutter_div_t;

typedef enum {
    SP1_DELAY_DIV_1_32     = 0, /* 0.50 * 16th */
    SP1_DELAY_DIV_1_16     = 1, /* 1.00 * 16th */
    SP1_DELAY_DIV_DOT_1_16 = 2, /* 1.50 * 16th (boot default) */
    SP1_DELAY_DIV_1_8      = 3, /* 2.00 * 16th */
    SP1_DELAY_DIV_DOT_1_8  = 4, /* 3.00 * 16th */
    SP1_DELAY_DIV_1_4      = 5, /* 4.00 * 16th */
    SP1_DELAY_DIV_COUNT    = 6
} sp1_delay_div_t;

/* Complete Stereo Master Performance FX Bus State.
 * Static RAM: 32,768 B (stutter) + 16,384 B (tape dub delay) + ~200 B (state) = ~49,352 B (~48.2 KB). */
typedef struct {
    /* 1. Static 32 KB circular stereo stutter buffer (interleaved L, R int16_t) */
    int16_t stutter_buf[SP1_FX_STUTTER_FRAMES * 2U];

    /* 2. Static 16 KB stereo ping-pong tape dub delay buffers (L tap + R tap int16_t) */
    int16_t delay_buf_l[SP1_FX_DELAY_TAP_SAMPLES];
    int16_t delay_buf_r[SP1_FX_DELAY_TAP_SAMPLES];

    /* 3. Stutter / Beat-Repeat runtime state */
    uint32_t stutter_write_pos;
    uint32_t stutter_anchor_pos;
    uint32_t stutter_read_offset;
    uint32_t stutter_loop_len;
    sp1_stutter_div_t stutter_target_div;
    sp1_stutter_div_t stutter_active_div;
    float stutter_wet_env;

    /* 4. Tape-Style Pitch-Gliding Dub Ping-Pong Delay runtime state */
    uint32_t delay_write_pos;
    uint32_t delay_active_samples;  /* Non-zero while echo energy remains in flight on tape */
    uint8_t delay_decim_cnt;
    uint8_t delay_locked_div;       /* sp1_delay_div_t [0..5] */
    bool delay_free_dragging;       /* True while slider actively moves; false glides to division */
    bool delay_tap_initialized;
    float delay_time_norm;          /* Normalized fader position [0.0f..1.0f], default 0.40f */
    float delay_tap_target;         /* Target delay tap in fractional 8 kHz samples */
    float delay_tap_smooth;         /* Slewed read-head offset for Doppler tape pitch shift */
    float delay_prev_l;
    float delay_curr_l;
    float delay_prev_r;
    float delay_curr_r;
    float delay_lp_l;
    float delay_lp_r;
    float delay_wet;
    float delay_feedback;
    float siren_wet_target;
    float siren_wet_env;
    float siren_fb_target;
    float siren_fb_env;

    /* 5. DJ Resonant Cytomic TPT SVF Isolator state */
    sp1_svf_t dj_svf_l;
    sp1_svf_t dj_svf_r;
    float dj_filter_target;     /* [0.0f..1.0f], 0.5f = center bypass */
    float dj_filter_smooth;
    float dj_resonance_q;       /* Clamped in [0.707f..12.0f] */

    /* 6. Bitcrusher / Sample-Rate Decimator + Master Limiter state */
    float crush_amount;         /* [0.0f..1.0f], 0.0f = clean 48 kHz 16-bit */
    uint32_t crush_hold_period; /* 1..16 samples ZOH decimation */
    uint32_t crush_hold_cnt;
    float crush_hold_l;
    float crush_hold_r;
    float crush_quant_scale;
    float crush_inv_quant_scale;
    float master_drive;
} sp1_master_fx_t;

/* ── Public Master FX Bus API ────────────────────────────────────────────── */

void sp1_master_fx_init(sp1_master_fx_t *fx);
void sp1_master_fx_reset(sp1_master_fx_t *fx);

void sp1_master_fx_set_dj_filter(sp1_master_fx_t *fx,
                                 float pos_0_to_1,
                                 float resonance_q);

void sp1_master_fx_set_stutter(sp1_master_fx_t *fx, sp1_stutter_div_t div);

void sp1_master_fx_set_bitcrusher(sp1_master_fx_t *fx, float crush_0_to_1);

void sp1_master_fx_set_dub_delay(sp1_master_fx_t *fx,
                                 float wet_0_to_1,
                                 float feedback_0_to_1);

/**
 * Configure Tape-Style Pitch-Gliding Dub Delay Duration:
 *  - While free_dragging is true, delay target tracks time_0_to_1 continuously so active
 *    repeats bend smoothly in pitch like changing physical tape speed.
 *  - When free_dragging becomes false, delay target locks onto the nearest musical tempo
 *    division (1/32, 1/16, dotted 1/16, 1/8, dotted 1/8, 1/4) and glides smoothly into sync.
 */
void sp1_master_fx_set_dub_delay_time(sp1_master_fx_t *fx,
                                      float time_0_to_1,
                                      bool free_dragging);

sp1_delay_div_t sp1_master_fx_get_dub_delay_div(const sp1_master_fx_t *fx);

/**
 * Set momentary Dub Siren echo throw & temporary feedback boost (smoothly decaying back
 * to the drum mix's delay wet/feedback over ~1.5 s after Function is released).
 */
void sp1_master_fx_set_siren_throw(sp1_master_fx_t *fx,
                                   bool siren_gate,
                                   float throw_0_to_1);

void sp1_master_fx_process_block(sp1_master_fx_t *fx,
                                 uint32_t step_16th_frames,
                                 const float *in_main_l,
                                 const float *in_main_r,
                                 const float *in_send_l,
                                 const float *in_send_r,
                                 int16_t *out_interleaved_pcm,
                                 uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* SP1_DSP_MASTER_FX_H */
