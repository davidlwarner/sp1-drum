#ifndef SP1_DSP_MASTER_FX_H
#define SP1_DSP_MASTER_FX_H

/**
 * @file master_fx.h
 * @brief Pure C11 Stereo Master Performance FX Bus for 48 kHz ARM Cortex-M4F (nRF52840).
 *
 * Includes:
 *  1. Dub Ping-Pong Delay Send (8 KB static SRAM, tempo-synced L<->R bounce fed by Snare + Perc).
 *  2. Tempo-Synced Beat-Repeat / Stutter Looper (64 KB static SRAM = 16,384 stereo int16_t frames,
 *     quantized 1/4 down to 1/64 ratchet with 64-frame boundary crossfading).
 *  3. DJ-Style Resonant Cytomic TPT SVF Isolator (center bypass deadzone [0.45..0.55],
 *     LP sweep < 0.45, HP sweep > 0.55, cutoff <= 0.42 * fs, Q <= 12.0).
 *  4. Zero-Order Hold Sample-Rate Decimator / Bitcrusher + Division-Free Master Soft-Clipper.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dsp/drum_voices.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 16,384 stereo int16_t frames = 32,768 int16_t samples = 65,536 B (64.0 KB) static SRAM */
#define SP1_FX_STUTTER_FRAMES           16384U
#define SP1_FX_STUTTER_MASK             (SP1_FX_STUTTER_FRAMES - 1U)
#define SP1_FX_STUTTER_XFADE_FRAMES     64U
#define SP1_FX_STUTTER_INV_XFADE        0.015625f   /* 1.0f / 64.0f */
#define SP1_FX_STUTTER_MIN_LOOP_FRAMES  128U
#define SP1_FX_STUTTER_MAX_LOOP_FRAMES  (SP1_FX_STUTTER_FRAMES - SP1_FX_STUTTER_XFADE_FRAMES - 4U)

/* 2,048 mono int16_t samples per L/R tap = 4,096 int16_t samples = 8,192 B (8.0 KB) static SRAM.
 * Clocked at 16 kHz (3x BBD decimation), providing up to 128 ms/tap (256 ms ping-pong). */
#define SP1_FX_DELAY_TAP_SAMPLES        2048U
#define SP1_FX_DELAY_TAP_MASK           (SP1_FX_DELAY_TAP_SAMPLES - 1U)
#define SP1_FX_DELAY_DECIM_FACTOR       3U

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

/* Complete Stereo Master Performance FX Bus State.
 * Static RAM: 65,536 B (stutter) + 8,192 B (dub delay) + 164 B (SVF/state) = 73,892 B (~72.16 KB). */
typedef struct {
    /* 1. Static 64 KB circular stereo stutter buffer (interleaved L, R int16_t) */
    int16_t stutter_buf[SP1_FX_STUTTER_FRAMES * 2U];

    /* 2. Static 8 KB stereo ping-pong dub delay buffers (L tap + R tap int16_t) */
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

    /* 4. Dub Ping-Pong Delay runtime state */
    uint32_t delay_write_pos;
    uint32_t delay_tap_len;
    uint8_t delay_decim_cnt;
    uint8_t pad_delay[3];
    float delay_prev_l;
    float delay_curr_l;
    float delay_prev_r;
    float delay_curr_r;
    float delay_lp_l;
    float delay_lp_r;
    float delay_wet;
    float delay_feedback;

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

/**
 * Configure DJ-Style Resonant SVF Isolator:
 *  - pos_0_to_1 in [0.45f, 0.55f]: center deadzone bypass
 *  - pos_0_to_1 < 0.45f: Low-Pass sweep (20 kHz -> 60 Hz)
 *  - pos_0_to_1 > 0.55f: High-Pass sweep (30 Hz -> 12 kHz)
 *  - resonance_q clamped to [0.5f, 12.0f]
 */
void sp1_master_fx_set_dj_filter(sp1_master_fx_t *fx,
                                 float pos_0_to_1,
                                 float resonance_q);

/**
 * Set momentary or latched tempo-synced Beat-Repeat / Stutter division.
 */
void sp1_master_fx_set_stutter(sp1_master_fx_t *fx, sp1_stutter_div_t div);

/**
 * Set Bitcrusher / Sample-Rate Decimator amount [0.0f..1.0f] (0.0f = clean).
 */
void sp1_master_fx_set_bitcrusher(sp1_master_fx_t *fx, float crush_0_to_1);

/**
 * Set Dub Ping-Pong Delay wet send level [0.0f..1.0f] and feedback [0.0f..0.88f].
 */
void sp1_master_fx_set_dub_delay(sp1_master_fx_t *fx,
                                 float wet_0_to_1,
                                 float feedback_0_to_1);

/**
 * Process one 48 kHz stereo block through:
 *  1. Selective Dub Ping-Pong Delay (fed by in_send_l / in_send_r)
 *  2. Tempo-Synced Beat-Repeat / Stutter Looper (with 64-frame loop boundary crossfade)
 *  3. DJ Resonant Cytomic TPT SVF Isolator (LP / Center Deadzone / HP)
 *  4. Zero-Order Hold Sample-Rate Decimator + Bitcrusher + Master Soft-Clipper
 *
 * Outputs interleaved stereo int16_t PCM frames into out_interleaved_pcm (2 * num_frames).
 * Zero runtime divisions inside the per-sample loop.
 */
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
