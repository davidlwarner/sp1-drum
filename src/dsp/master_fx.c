#include "dsp/master_fx.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define SP1_FX_S16_SCALE            32767.0f
#define SP1_FX_INV_S16_SCALE        0.000030518509476f  /* 1.0f / 32767.0f */
#define SP1_FX_INV_DJ_SPAN          2.22222222f         /* 1.0f / 0.45f */

/* Precomputed 3x BBD linear interpolation fractions (0/3, 1/3, 2/3) in Flash */
static const float k_bbd_interp_frac[SP1_FX_DELAY_DECIM_FACTOR] = {
    0.0f, 0.33333334f, 0.66666669f
};

/* Precomputed bitcrusher quantization scales & reciprocals in Flash (zero division) */
static const float k_crush_quant_scale[16] = {
    32767.0f, 2048.0f, 1024.0f, 512.0f,
    256.0f,   128.0f,  96.0f,   64.0f,
    48.0f,    32.0f,   24.0f,   16.0f,
    12.0f,    10.0f,   8.0f,    6.0f
};

static const float k_crush_inv_quant_scale[16] = {
    0.000030518509f, 0.00048828125f, 0.00097656250f, 0.00195312500f,
    0.00390625000f,  0.00781250000f, 0.01041666667f, 0.01562500000f,
    0.02083333333f,  0.03125000000f, 0.04166666667f, 0.06250000000f,
    0.08333333333f,  0.10000000000f, 0.12500000000f, 0.16666666667f
};

static inline float clamp01(float x) {
    if (x < 0.0f) {
        return 0.0f;
    }
    if (x > 1.0f) {
        return 1.0f;
    }
    return x;
}

static inline int16_t float_to_s16_clamped(float x) {
    float scaled = x * SP1_FX_S16_SCALE;
    if (scaled >= 32767.0f) {
        return 32767;
    }
    if (scaled <= -32767.0f) {
        return -32767;
    }
    return (int16_t)scaled;
}

static uint32_t compute_stutter_loop_frames(uint32_t step_16th_frames,
                                            sp1_stutter_div_t div) {
    uint32_t base = (step_16th_frames > 0U) ? step_16th_frames : 6000U;
    uint32_t len = base;

    switch (div) {
    case SP1_STUTTER_DIV_1_4:
        len = base << 2U;
        break;
    case SP1_STUTTER_DIV_1_8:
        len = base << 1U;
        break;
    case SP1_STUTTER_DIV_1_16:
        len = base;
        break;
    case SP1_STUTTER_DIV_1_32:
        len = base >> 1U;
        break;
    case SP1_STUTTER_DIV_1_64:
        len = base >> 2U;
        break;
    case SP1_STUTTER_OFF:
    default:
        len = base;
        break;
    }

    if (len < SP1_FX_STUTTER_MIN_LOOP_FRAMES) {
        len = SP1_FX_STUTTER_MIN_LOOP_FRAMES;
    }
    if (len > SP1_FX_STUTTER_MAX_LOOP_FRAMES) {
        len = SP1_FX_STUTTER_MAX_LOOP_FRAMES;
    }
    return len;
}

static uint32_t compute_dub_delay_tap_samples(uint32_t step_16th_frames) {
    /* Dotted-16th / 1/8th dub bounce at 16 kHz BBD rate (step_16th_frames / 3 ~= step * 341 >> 10) */
    uint32_t base = (step_16th_frames > 0U) ? step_16th_frames : 6000U;
    /* 1.5 * step_16th_frames at 16 kHz = 0.5 * step_16th_frames in BBD samples */
    uint32_t tap = base >> 1U;
    if (tap < 128U) {
        tap = 128U;
    }
    if (tap > (SP1_FX_DELAY_TAP_SAMPLES - 8U)) {
        tap = SP1_FX_DELAY_TAP_SAMPLES - 8U;
    }
    return tap;
}

void sp1_master_fx_init(sp1_master_fx_t *fx) {
    if (fx == NULL) {
        return;
    }
    memset(fx, 0, sizeof(*fx));

    fx->stutter_loop_len = 6000U;
    fx->stutter_target_div = SP1_STUTTER_OFF;
    fx->stutter_active_div = SP1_STUTTER_OFF;
    fx->stutter_wet_env = 0.0f;

    fx->delay_tap_len = 1500U;
    fx->delay_wet = 0.22f;
    fx->delay_feedback = 0.45f;

    fx->dj_filter_target = 0.50f;
    fx->dj_filter_smooth = 0.50f;
    fx->dj_resonance_q = 1.85f;
    sp1_svf_reset(&fx->dj_svf_l);
    sp1_svf_reset(&fx->dj_svf_r);
    sp1_svf_set_params(&fx->dj_svf_l, 18000.0f, fx->dj_resonance_q);
    sp1_svf_set_params(&fx->dj_svf_r, 18000.0f, fx->dj_resonance_q);

    fx->crush_amount = 0.0f;
    fx->crush_hold_period = 1U;
    fx->crush_quant_scale = k_crush_quant_scale[0];
    fx->crush_inv_quant_scale = k_crush_inv_quant_scale[0];
    fx->master_drive = 1.05f;
}

void sp1_master_fx_reset(sp1_master_fx_t *fx) {
    if (fx == NULL) {
        return;
    }
    memset(fx->stutter_buf, 0, sizeof(fx->stutter_buf));
    memset(fx->delay_buf_l, 0, sizeof(fx->delay_buf_l));
    memset(fx->delay_buf_r, 0, sizeof(fx->delay_buf_r));
    fx->stutter_write_pos = 0U;
    fx->stutter_anchor_pos = 0U;
    fx->stutter_read_offset = 0U;
    fx->stutter_active_div = SP1_STUTTER_OFF;
    fx->stutter_target_div = SP1_STUTTER_OFF;
    fx->stutter_wet_env = 0.0f;
    fx->delay_write_pos = 0U;
    fx->delay_decim_cnt = 0U;
    fx->delay_prev_l = 0.0f;
    fx->delay_curr_l = 0.0f;
    fx->delay_prev_r = 0.0f;
    fx->delay_curr_r = 0.0f;
    fx->delay_lp_l = 0.0f;
    fx->delay_lp_r = 0.0f;
    sp1_svf_reset(&fx->dj_svf_l);
    sp1_svf_reset(&fx->dj_svf_r);
}

void sp1_master_fx_set_dj_filter(sp1_master_fx_t *fx,
                                 float pos_0_to_1,
                                 float resonance_q) {
    if (fx == NULL) {
        return;
    }
    fx->dj_filter_target = clamp01(pos_0_to_1);
    if (resonance_q < 0.5f) {
        resonance_q = 0.5f;
    } else if (resonance_q > 12.0f) {
        resonance_q = 12.0f;
    }
    fx->dj_resonance_q = resonance_q;
}

void sp1_master_fx_set_stutter(sp1_master_fx_t *fx, sp1_stutter_div_t div) {
    if (fx == NULL) {
        return;
    }
    if ((unsigned)div > (unsigned)SP1_STUTTER_DIV_1_64) {
        div = SP1_STUTTER_OFF;
    }
    fx->stutter_target_div = div;
}

void sp1_master_fx_set_bitcrusher(sp1_master_fx_t *fx, float crush_0_to_1) {
    if (fx == NULL) {
        return;
    }
    const float c = clamp01(crush_0_to_1);
    fx->crush_amount = c;

    /* Map [0.0f..1.0f] to ZOH hold period 1..16 samples and LUT quantization step 0..15 */
    uint32_t idx = (uint32_t)(c * 15.0f + 0.5f);
    if (idx > 15U) {
        idx = 15U;
    }
    fx->crush_hold_period = 1U + idx;
    fx->crush_quant_scale = k_crush_quant_scale[idx];
    fx->crush_inv_quant_scale = k_crush_inv_quant_scale[idx];
    fx->master_drive = 1.05f + (c * 0.55f);
}

void sp1_master_fx_set_dub_delay(sp1_master_fx_t *fx,
                                 float wet_0_to_1,
                                 float feedback_0_to_1) {
    if (fx == NULL) {
        return;
    }
    fx->delay_wet = clamp01(wet_0_to_1) * 0.65f;
    float fb = clamp01(feedback_0_to_1);
    if (fb > 0.85f) {
        fb = 0.85f; /* Guard against infinite dub runaway */
    }
    fx->delay_feedback = fb;
}

#if defined(__GNUC__) || defined(__clang__)
#define SP1_FX_NOINLINE __attribute__((noinline))
#else
#define SP1_FX_NOINLINE
#endif

static inline float fast_round_nearest(float x) {
    /* Bitcrusher inputs are bounded within ~[-65536, +65536], safe for int32_t. */
    return (float)((int32_t)(x + ((x >= 0.0f) ? 0.5f : -0.5f)));
}

/* Register-resident fast path for normal playback (DJ filter centered, crush off, stutter off).
 * Keeps all Dub Delay + Stutter record state in local registers with zero stack spills and
 * fits completely inside the nRF52840 2 KB Flash ICACHE. */
static SP1_FX_NOINLINE void process_block_normal_fast(sp1_master_fx_t *fx,
                                                      const float *in_main_l,
                                                      const float *in_main_r,
                                                      const float *in_send_l,
                                                      const float *in_send_r,
                                                      int16_t *out_interleaved_pcm,
                                                      uint32_t num_frames) {
    int16_t *const delay_buf_l = fx->delay_buf_l;
    int16_t *const delay_buf_r = fx->delay_buf_r;
    int16_t *const stutter_buf = fx->stutter_buf;

    uint32_t delay_wpos = fx->delay_write_pos;
    const uint32_t delay_tap = fx->delay_tap_len;
    uint32_t decim_cnt = fx->delay_decim_cnt;
    float prev_l = fx->delay_prev_l;
    float prev_r = fx->delay_prev_r;
    float curr_l = fx->delay_curr_l;
    float curr_r = fx->delay_curr_r;
    float lp_l = fx->delay_lp_l;
    float lp_r = fx->delay_lp_r;
    const float delay_wet = fx->delay_wet;
    const float delay_fb = fx->delay_feedback;

    uint32_t stut_wpos = fx->stutter_write_pos;
    const float master_drive = fx->master_drive;
    const bool has_send = (in_send_l != NULL && in_send_r != NULL);

    for (uint32_t i = 0U; i < num_frames; ++i) {
        float sig_l = in_main_l[i];
        float sig_r = in_main_r[i];

        if (decim_cnt == 0U) {
            const float send_l = has_send ? in_send_l[i] : 0.0f;
            const float send_r = has_send ? in_send_r[i] : 0.0f;
            const uint32_t read_idx = (delay_wpos - delay_tap) & SP1_FX_DELAY_TAP_MASK;

            prev_l = curr_l;
            prev_r = curr_r;
            curr_l = (float)delay_buf_l[read_idx] * SP1_FX_INV_S16_SCALE;
            curr_r = (float)delay_buf_r[read_idx] * SP1_FX_INV_S16_SCALE;

            lp_l += 0.36f * (curr_l - lp_l);
            lp_r += 0.36f * (curr_r - lp_r);
#if !defined(__ARM_FP) || (__ARM_FP == 0)
            if (fabsf(lp_l) < SP1_DSP_DENORMAL_FLOOR) {
                lp_l = 0.0f;
            }
            if (fabsf(lp_r) < SP1_DSP_DENORMAL_FLOOR) {
                lp_r = 0.0f;
            }
#endif

            const float in_bbd_l = sp1_soft_clip((send_l * 0.80f) + (lp_r * delay_fb));
            const float in_bbd_r = sp1_soft_clip((send_r * 0.35f) + (lp_l * delay_fb));

            delay_buf_l[delay_wpos] = float_to_s16_clamped(in_bbd_l);
            delay_buf_r[delay_wpos] = float_to_s16_clamped(in_bbd_r);
            delay_wpos = (delay_wpos + 1U) & SP1_FX_DELAY_TAP_MASK;
        }

        const float bbd_t = k_bbd_interp_frac[decim_cnt];
        sig_l += (prev_l + ((curr_l - prev_l) * bbd_t)) * delay_wet;
        sig_r += (prev_r + ((curr_r - prev_r) * bbd_t)) * delay_wet;

        decim_cnt++;
        if (decim_cnt >= SP1_FX_DELAY_DECIM_FACTOR) {
            decim_cnt = 0U;
        }

        /* Continuously record dry+delay stereo stream so stutter engages click-free */
        const uint32_t w_idx = stut_wpos << 1U;
        stutter_buf[w_idx] = float_to_s16_clamped(sig_l);
        stutter_buf[w_idx + 1U] = float_to_s16_clamped(sig_r);
        stut_wpos = (stut_wpos + 1U) & SP1_FX_STUTTER_MASK;

        const float out_l = sp1_soft_clip(sig_l * master_drive);
        const float out_r = sp1_soft_clip(sig_r * master_drive);
        out_interleaved_pcm[i << 1U] = float_to_s16_clamped(out_l);
        out_interleaved_pcm[(i << 1U) + 1U] = float_to_s16_clamped(out_r);
    }

    fx->delay_write_pos = delay_wpos;
    fx->delay_decim_cnt = decim_cnt;
    fx->delay_prev_l = prev_l;
    fx->delay_prev_r = prev_r;
    fx->delay_curr_l = curr_l;
    fx->delay_curr_r = curr_r;
    fx->delay_lp_l = lp_l;
    fx->delay_lp_r = lp_r;
    fx->stutter_write_pos = stut_wpos;
}

void sp1_master_fx_process_block(sp1_master_fx_t *fx,
                                 uint32_t step_16th_frames,
                                 const float *in_main_l,
                                 const float *in_main_r,
                                 const float *in_send_l,
                                 const float *in_send_r,
                                 int16_t *out_interleaved_pcm,
                                 uint32_t num_frames) {
    if (fx == NULL || in_main_l == NULL || in_main_r == NULL ||
        out_interleaved_pcm == NULL || num_frames == 0U) {
        return;
    }

    /* ── Block-Rate Setup (500 Hz for 96-frame blocks; zero per-sample division) ── */

    fx->delay_tap_len = compute_dub_delay_tap_samples(step_16th_frames);

    if (fx->stutter_target_div != SP1_STUTTER_OFF &&
        fx->stutter_active_div != fx->stutter_target_div) {
        const uint32_t new_len =
            compute_stutter_loop_frames(step_16th_frames, fx->stutter_target_div);
        if (fx->stutter_active_div == SP1_STUTTER_OFF) {
            fx->stutter_anchor_pos =
                (fx->stutter_write_pos - new_len) & SP1_FX_STUTTER_MASK;
            fx->stutter_read_offset = 0U;
        } else if (fx->stutter_read_offset >= new_len) {
            fx->stutter_read_offset = 0U;
        }
        fx->stutter_loop_len = new_len;
        fx->stutter_active_div = fx->stutter_target_div;
    }

    /* Smooth DJ isolator position at block rate and update Cytomic TPT SVF coefficients */
    fx->dj_filter_smooth += 0.28f * (fx->dj_filter_target - fx->dj_filter_smooth);
    if (fabsf(fx->dj_filter_target - fx->dj_filter_smooth) < 0.001f) {
        fx->dj_filter_smooth = fx->dj_filter_target;
    }

    const float dj_pos = fx->dj_filter_smooth;
    int dj_mode = 0; /* 0 = center bypass deadzone, -1 = Low-Pass, +1 = High-Pass */

    if (dj_pos < SP1_FX_DJ_DEADZONE_LOW) {
        /* Low-Pass sweep: norm 0.0f (at 0.0) -> 1.0f (at 0.45)
         * Quadratic curve from 60 Hz up to 20,000 Hz */
        const float t = dj_pos * SP1_FX_INV_DJ_SPAN;
        const float t2 = t * t;
        const float cutoff_hz = 60.0f + (19940.0f * t2);
        sp1_svf_set_params(&fx->dj_svf_l, cutoff_hz, fx->dj_resonance_q);
        sp1_svf_set_params(&fx->dj_svf_r, cutoff_hz, fx->dj_resonance_q);
        dj_mode = -1;
    } else if (dj_pos > SP1_FX_DJ_DEADZONE_HIGH) {
        /* High-Pass sweep: norm 0.0f (at 0.55) -> 1.0f (at 1.0)
         * Quadratic curve from 30 Hz up to 12,000 Hz */
        const float t = (dj_pos - SP1_FX_DJ_DEADZONE_HIGH) * SP1_FX_INV_DJ_SPAN;
        const float t2 = t * t;
        const float cutoff_hz = 30.0f + (11970.0f * t2);
        sp1_svf_set_params(&fx->dj_svf_l, cutoff_hz, fx->dj_resonance_q);
        sp1_svf_set_params(&fx->dj_svf_r, cutoff_hz, fx->dj_resonance_q);
        dj_mode = 1;
    } else {
        /* Center deadzone [0.45..0.55]: SVF bypassed; skip coefficient recomputation */
        dj_mode = 0;
    }

    const bool crush_enabled = (fx->crush_amount > 0.005f);

    if (dj_mode == 0 && !crush_enabled &&
        fx->stutter_target_div == SP1_STUTTER_OFF &&
        fx->stutter_active_div == SP1_STUTTER_OFF) {
        process_block_normal_fast(fx, in_main_l, in_main_r, in_send_l, in_send_r,
                                  out_interleaved_pcm, num_frames);
        return;
    }

    /* ── Division-Free Per-Sample Master FX Inner Loop (Active Performance FX) ── */

    for (uint32_t i = 0U; i < num_frames; ++i) {
        float sig_l = in_main_l[i];
        float sig_r = in_main_r[i];
        const float send_l = (in_send_l != NULL) ? in_send_l[i] : 0.0f;
        const float send_r = (in_send_r != NULL) ? in_send_r[i] : 0.0f;

        /* 1. Selective Dub Ping-Pong Delay (3x BBD decimated @ 16 kHz + LP feedback) */
        if (fx->delay_decim_cnt == 0U) {
            const uint32_t read_idx =
                (fx->delay_write_pos - fx->delay_tap_len) & SP1_FX_DELAY_TAP_MASK;

            fx->delay_prev_l = fx->delay_curr_l;
            fx->delay_prev_r = fx->delay_curr_r;
            fx->delay_curr_l = (float)fx->delay_buf_l[read_idx] * SP1_FX_INV_S16_SCALE;
            fx->delay_curr_r = (float)fx->delay_buf_r[read_idx] * SP1_FX_INV_S16_SCALE;

            /* Warm one-pole low-pass damper (~3.2 kHz) inside the ping-pong feedback loop */
            fx->delay_lp_l += 0.36f * (fx->delay_curr_l - fx->delay_lp_l);
            fx->delay_lp_r += 0.36f * (fx->delay_curr_r - fx->delay_lp_r);
#if !defined(__ARM_FP) || (__ARM_FP == 0)
            if (fabsf(fx->delay_lp_l) < SP1_DSP_DENORMAL_FLOOR) {
                fx->delay_lp_l = 0.0f;
            }
            if (fabsf(fx->delay_lp_r) < SP1_DSP_DENORMAL_FLOOR) {
                fx->delay_lp_r = 0.0f;
            }
#endif

            /* Ping-Pong topology:
             * - Left tap receives selective send + feedback from Right tap
             * - Right tap receives cross-fed output from Left tap + subtle right send */
            const float in_bbd_l =
                sp1_soft_clip((send_l * 0.80f) + (fx->delay_lp_r * fx->delay_feedback));
            const float in_bbd_r =
                sp1_soft_clip((send_r * 0.35f) + (fx->delay_lp_l * fx->delay_feedback));

            fx->delay_buf_l[fx->delay_write_pos] = float_to_s16_clamped(in_bbd_l);
            fx->delay_buf_r[fx->delay_write_pos] = float_to_s16_clamped(in_bbd_r);
            fx->delay_write_pos = (fx->delay_write_pos + 1U) & SP1_FX_DELAY_TAP_MASK;
        }

        const float bbd_t = k_bbd_interp_frac[fx->delay_decim_cnt];
        const float dub_l = fx->delay_prev_l + ((fx->delay_curr_l - fx->delay_prev_l) * bbd_t);
        const float dub_r = fx->delay_prev_r + ((fx->delay_curr_r - fx->delay_prev_r) * bbd_t);

        fx->delay_decim_cnt++;
        if (fx->delay_decim_cnt >= SP1_FX_DELAY_DECIM_FACTOR) {
            fx->delay_decim_cnt = 0U;
        }

        sig_l += dub_l * fx->delay_wet;
        sig_r += dub_r * fx->delay_wet;

        /* 2. Tempo-Synced Beat-Repeat / Stutter Looper (64 KB circular buffer + 64-frame crossfade) */
        if (fx->stutter_target_div == SP1_STUTTER_OFF) {
            /* Continuously record dry+delay stereo stream while stutter is not held */
            const uint32_t w_idx = fx->stutter_write_pos * 2U;
            fx->stutter_buf[w_idx] = float_to_s16_clamped(sig_l);
            fx->stutter_buf[w_idx + 1U] = float_to_s16_clamped(sig_r);
            fx->stutter_write_pos = (fx->stutter_write_pos + 1U) & SP1_FX_STUTTER_MASK;

            if (fx->stutter_wet_env > 0.0f) {
                fx->stutter_wet_env -= SP1_FX_STUTTER_INV_XFADE;
                if (fx->stutter_wet_env <= 0.0f) {
                    fx->stutter_wet_env = 0.0f;
                    fx->stutter_active_div = SP1_STUTTER_OFF;
                }
            }
        } else {
            if (fx->stutter_wet_env < 1.0f) {
                fx->stutter_wet_env += SP1_FX_STUTTER_INV_XFADE;
                if (fx->stutter_wet_env > 1.0f) {
                    fx->stutter_wet_env = 1.0f;
                }
            }
        }

        if (fx->stutter_active_div != SP1_STUTTER_OFF && fx->stutter_wet_env > 0.0f) {
            const uint32_t loop_len = fx->stutter_loop_len;
            const uint32_t roff = fx->stutter_read_offset;
            const uint32_t main_frame = (fx->stutter_anchor_pos + roff) & SP1_FX_STUTTER_MASK;

            float stut_l = (float)fx->stutter_buf[main_frame * 2U] * SP1_FX_INV_S16_SCALE;
            float stut_r = (float)fx->stutter_buf[(main_frame * 2U) + 1U] * SP1_FX_INV_S16_SCALE;

            /* 64-frame linear boundary crossfade with pre-anchor lead-in to prevent loop clicks */
            if ((roff + SP1_FX_STUTTER_XFADE_FRAMES) >= loop_len) {
                const uint32_t rem = loop_len - roff; /* 1..64 */
                const float alpha =
                    (float)(SP1_FX_STUTTER_XFADE_FRAMES - rem) * SP1_FX_STUTTER_INV_XFADE;
                const uint32_t lead_frame =
                    (fx->stutter_anchor_pos - rem) & SP1_FX_STUTTER_MASK;
                const float lead_l =
                    (float)fx->stutter_buf[lead_frame * 2U] * SP1_FX_INV_S16_SCALE;
                const float lead_r =
                    (float)fx->stutter_buf[(lead_frame * 2U) + 1U] * SP1_FX_INV_S16_SCALE;

                stut_l += alpha * (lead_l - stut_l);
                stut_r += alpha * (lead_r - stut_r);
            }

            fx->stutter_read_offset = (roff + 1U >= loop_len) ? 0U : (roff + 1U);

            const float wet = fx->stutter_wet_env;
            sig_l += wet * (stut_l - sig_l);
            sig_r += wet * (stut_r - sig_r);
        }

        /* 3. DJ-Style Resonant Cytomic TPT SVF Isolator */
        if (dj_mode != 0) {
            float lp_l = 0.0f;
            float hp_l = 0.0f;
            float lp_r = 0.0f;
            float hp_r = 0.0f;
            sp1_svf_process(&fx->dj_svf_l, sig_l, &lp_l, NULL, &hp_l);
            sp1_svf_process(&fx->dj_svf_r, sig_r, &lp_r, NULL, &hp_r);

            if (dj_mode < 0) {
                sig_l = lp_l;
                sig_r = lp_r;
            } else {
                sig_l = hp_l;
                sig_r = hp_r;
            }
        }

        /* 4. Zero-Order Hold Sample-Rate Decimator + Bitcrusher */
        if (crush_enabled) {
            if (fx->crush_hold_cnt == 0U) {
                const float q_scale = fx->crush_quant_scale;
                const float inv_q = fx->crush_inv_quant_scale;
                fx->crush_hold_l = fast_round_nearest(sig_l * q_scale) * inv_q;
                fx->crush_hold_r = fast_round_nearest(sig_r * q_scale) * inv_q;
            }
            sig_l = fx->crush_hold_l;
            sig_r = fx->crush_hold_r;

            fx->crush_hold_cnt++;
            if (fx->crush_hold_cnt >= fx->crush_hold_period) {
                fx->crush_hold_cnt = 0U;
            }
        }

        /* 5. Anti-Aliased Master Soft-Clipper / Limiter -> int16_t Stereo Output */
        const float out_l = sp1_soft_clip(sig_l * fx->master_drive);
        const float out_r = sp1_soft_clip(sig_r * fx->master_drive);

        out_interleaved_pcm[i * 2U] = float_to_s16_clamped(out_l);
        out_interleaved_pcm[(i * 2U) + 1U] = float_to_s16_clamped(out_r);
    }
}
