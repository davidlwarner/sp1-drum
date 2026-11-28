#include "dsp/master_fx.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define SP1_FX_S16_SCALE            32767.0f
#define SP1_FX_INV_S16_SCALE        0.000030518509476f  /* 1.0f / 32767.0f */
#define SP1_FX_INV_DJ_SPAN          2.22222222f         /* 1.0f / 0.45f */
#define SP1_FX_INV_6                0.16666667f         /* 1.0f / 6.0f (8 kHz BBD rate) */

/* Precomputed 6x BBD linear interpolation fractions (0/6 .. 5/6) in Flash */
static const float k_bbd_interp_frac[SP1_FX_DELAY_DECIM_FACTOR] = {
    0.0f, 0.16666667f, 0.33333334f, 0.50f, 0.66666669f, 0.83333331f
};

/* 6 Musical Dub Delay Tempo Divisions as multiples of one 16th-note step:
 *  0: 1/32 (0.5x), 1: 1/16 (1.0x), 2: dotted 1/16 (1.5x, default),
 *  3: 1/8  (2.0x), 4: dotted 1/8 (3.0x), 5: 1/4 (4.0x) */
static const float k_delay_div_16th_mult[SP1_DELAY_DIV_COUNT] = {
    0.50f, 1.00f, 1.50f, 2.00f, 3.00f, 4.00f
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

static float compute_dub_delay_target_samples(const sp1_master_fx_t *fx,
                                              uint32_t step_16th_frames) {
    const float base_16th_bbd =
        (float)((step_16th_frames > 0U) ? step_16th_frames : 6000U) * SP1_FX_INV_6;

    float mult = 1.50f;
    if (fx->delay_free_dragging) {
        /* Continuous piecewise-linear tape-speed curve passing through all 6 division knots */
        const float pos5 = fx->delay_time_norm * 5.0f;
        uint32_t seg = (uint32_t)pos5;
        if (seg >= ((uint32_t)SP1_DELAY_DIV_COUNT - 1U)) {
            seg = (uint32_t)SP1_DELAY_DIV_COUNT - 2U;
        }
        const float frac = pos5 - (float)seg;
        const float a = k_delay_div_16th_mult[seg];
        const float b = k_delay_div_16th_mult[seg + 1U];
        mult = a + ((b - a) * frac);
    } else {
        const uint8_t div = (fx->delay_locked_div < (uint8_t)SP1_DELAY_DIV_COUNT)
                                ? fx->delay_locked_div
                                : (uint8_t)SP1_DELAY_DIV_DOT_1_16;
        mult = k_delay_div_16th_mult[div];
    }

    float tap = base_16th_bbd * mult;
    const float max_tap = (float)(SP1_FX_DELAY_TAP_SAMPLES - 8U);
    if (tap < 48.0f) {
        tap = 48.0f;
    } else if (tap > max_tap) {
        tap = max_tap;
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

    fx->delay_locked_div = (uint8_t)SP1_DELAY_DIV_DOT_1_16;
    fx->delay_free_dragging = false;
    fx->delay_tap_initialized = false;
    fx->delay_time_norm = 0.40f; /* Knot 2 = Dotted 1/16 */
    fx->delay_tap_target = 1500.0f;
    fx->delay_tap_smooth = 1500.0f;
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
    fx->delay_active_samples = 0U;
    fx->delay_decim_cnt = 0U;
    fx->delay_tap_initialized = false;
    fx->delay_prev_l = 0.0f;
    fx->delay_curr_l = 0.0f;
    fx->delay_prev_r = 0.0f;
    fx->delay_curr_r = 0.0f;
    fx->delay_lp_l = 0.0f;
    fx->delay_lp_r = 0.0f;
    fx->siren_wet_env = 0.0f;
    fx->siren_fb_env = 0.0f;
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
        fb = 0.85f;
    }
    fx->delay_feedback = fb;
}

void sp1_master_fx_set_dub_delay_time(sp1_master_fx_t *fx,
                                      float time_0_to_1,
                                      bool free_dragging) {
    if (fx == NULL) {
        return;
    }
    const float c = clamp01(time_0_to_1);
    fx->delay_time_norm = c;
    fx->delay_free_dragging = free_dragging;

    uint32_t div = (uint32_t)(c * 5.0f + 0.5f);
    if (div >= (uint32_t)SP1_DELAY_DIV_COUNT) {
        div = (uint32_t)SP1_DELAY_DIV_COUNT - 1U;
    }
    fx->delay_locked_div = (uint8_t)div;
}

sp1_delay_div_t sp1_master_fx_get_dub_delay_div(const sp1_master_fx_t *fx) {
    if (fx == NULL || fx->delay_locked_div >= (uint8_t)SP1_DELAY_DIV_COUNT) {
        return SP1_DELAY_DIV_DOT_1_16;
    }
    return (sp1_delay_div_t)fx->delay_locked_div;
}

void sp1_master_fx_set_siren_throw(sp1_master_fx_t *fx,
                                   bool siren_gate,
                                   float throw_0_to_1) {
    if (fx == NULL) {
        return;
    }
    if (siren_gate) {
        const float th = clamp01(throw_0_to_1);
        fx->siren_wet_target = th * 0.58f;
        fx->siren_fb_target = 0.30f + (th * 0.57f); /* Up to 0.87f dub echo throw */
    } else {
        fx->siren_wet_target = 0.0f;
        fx->siren_fb_target = 0.0f;
    }
}

#if defined(__GNUC__) || defined(__clang__)
#define SP1_FX_NOINLINE __attribute__((noinline))
#else
#define SP1_FX_NOINLINE
#endif

static inline float fast_round_nearest(float x) {
    return (float)((int32_t)(x + ((x >= 0.0f) ? 0.5f : -0.5f)));
}

static bool block_has_send_energy(const float *in_send_l,
                                  const float *in_send_r,
                                  uint32_t num_frames) {
    if (in_send_l == NULL || in_send_r == NULL) {
        return false;
    }
    for (uint32_t i = 0U; i < num_frames; ++i) {
        if (fabsf(in_send_l[i]) > 1.0e-5f || fabsf(in_send_r[i]) > 1.0e-5f) {
            return true;
        }
    }
    return false;
}

/* Register-resident fast path for normal playback (DJ filter centered, crush off, stutter off). */
static SP1_FX_NOINLINE void process_block_normal_fast(sp1_master_fx_t *fx,
                                                      bool run_delay,
                                                      float eff_wet,
                                                      float eff_fb,
                                                      const float *in_main_l,
                                                      const float *in_main_r,
                                                      const float *in_send_l,
                                                      const float *in_send_r,
                                                      int16_t *out_interleaved_pcm,
                                                      uint32_t num_frames) {
    int16_t *const stutter_buf = fx->stutter_buf;
    uint32_t stut_wpos = fx->stutter_write_pos;
    const float master_drive = fx->master_drive;

    if (!run_delay) {
        for (uint32_t i = 0U; i < num_frames; ++i) {
            const float sig_l = in_main_l[i];
            const float sig_r = in_main_r[i];

            const uint32_t w_idx = stut_wpos << 1U;
            stutter_buf[w_idx] = float_to_s16_clamped(sig_l);
            stutter_buf[w_idx + 1U] = float_to_s16_clamped(sig_r);
            stut_wpos = (stut_wpos + 1U) & SP1_FX_STUTTER_MASK;

            out_interleaved_pcm[i << 1U] = float_to_s16_clamped(sp1_soft_clip(sig_l * master_drive));
            out_interleaved_pcm[(i << 1U) + 1U] = float_to_s16_clamped(sp1_soft_clip(sig_r * master_drive));
        }
        fx->stutter_write_pos = stut_wpos;
        return;
    }

    int16_t *const delay_buf_l = fx->delay_buf_l;
    int16_t *const delay_buf_r = fx->delay_buf_r;
    uint32_t delay_wpos = fx->delay_write_pos;
    uint32_t active_rem = fx->delay_active_samples;
    uint32_t decim_cnt = fx->delay_decim_cnt;
    float tap_smooth = fx->delay_tap_smooth;
    const float tap_target = fx->delay_tap_target;
    float prev_l = fx->delay_prev_l;
    float prev_r = fx->delay_prev_r;
    float curr_l = fx->delay_curr_l;
    float curr_r = fx->delay_curr_r;
    float lp_l = fx->delay_lp_l;
    float lp_r = fx->delay_lp_r;
    const bool has_send = (in_send_l != NULL && in_send_r != NULL);

    for (uint32_t i = 0U; i < num_frames; ++i) {
        float sig_l = in_main_l[i];
        float sig_r = in_main_r[i];

        if (decim_cnt == 0U) {
            const float send_l = has_send ? in_send_l[i] : 0.0f;
            const float send_r = has_send ? in_send_r[i] : 0.0f;

            /* Smooth physical tape capstan inertia (tau ~ 70 ms @ 8 kHz) */
            tap_smooth += 0.0018f * (tap_target - tap_smooth);
            const int32_t tap_i = (int32_t)tap_smooth;
            const float tap_frac = tap_smooth - (float)tap_i;
            const uint32_t r0 = (delay_wpos - (uint32_t)tap_i) & SP1_FX_DELAY_TAP_MASK;
            const uint32_t r1 = (r0 - 1U) & SP1_FX_DELAY_TAP_MASK;

            prev_l = curr_l;
            prev_r = curr_r;
            const float l0 = (float)delay_buf_l[r0] * SP1_FX_INV_S16_SCALE;
            const float l1 = (float)delay_buf_l[r1] * SP1_FX_INV_S16_SCALE;
            const float r_0 = (float)delay_buf_r[r0] * SP1_FX_INV_S16_SCALE;
            const float r_1 = (float)delay_buf_r[r1] * SP1_FX_INV_S16_SCALE;
            curr_l = l0 + ((l1 - l0) * tap_frac);
            curr_r = r_0 + ((r_1 - r_0) * tap_frac);

            lp_l += 0.42f * (curr_l - lp_l);
            lp_r += 0.42f * (curr_r - lp_r);
#if !defined(__ARM_FP) || (__ARM_FP == 0)
            if (fabsf(lp_l) < SP1_DSP_DENORMAL_FLOOR) {
                lp_l = 0.0f;
            }
            if (fabsf(lp_r) < SP1_DSP_DENORMAL_FLOOR) {
                lp_r = 0.0f;
            }
#endif

            const float in_bbd_l = sp1_soft_clip((send_l * 0.80f) + (lp_r * eff_fb));
            const float in_bbd_r = sp1_soft_clip((send_r * 0.35f) + (lp_l * eff_fb));

            if ((fabsf(in_bbd_l) + fabsf(in_bbd_r)) > 1.0e-4f) {
                active_rem = SP1_FX_DELAY_TAP_SAMPLES + 64U;
            } else if (active_rem > 0U) {
                active_rem--;
            }

            delay_buf_l[delay_wpos] = float_to_s16_clamped(in_bbd_l);
            delay_buf_r[delay_wpos] = float_to_s16_clamped(in_bbd_r);
            delay_wpos = (delay_wpos + 1U) & SP1_FX_DELAY_TAP_MASK;
        }

        const float bbd_t = k_bbd_interp_frac[decim_cnt];
        sig_l += (prev_l + ((curr_l - prev_l) * bbd_t)) * eff_wet;
        sig_r += (prev_r + ((curr_r - prev_r) * bbd_t)) * eff_wet;

        decim_cnt++;
        if (decim_cnt >= SP1_FX_DELAY_DECIM_FACTOR) {
            decim_cnt = 0U;
        }

        const uint32_t w_idx = stut_wpos << 1U;
        stutter_buf[w_idx] = float_to_s16_clamped(sig_l);
        stutter_buf[w_idx + 1U] = float_to_s16_clamped(sig_r);
        stut_wpos = (stut_wpos + 1U) & SP1_FX_STUTTER_MASK;

        out_interleaved_pcm[i << 1U] = float_to_s16_clamped(sp1_soft_clip(sig_l * master_drive));
        out_interleaved_pcm[(i << 1U) + 1U] = float_to_s16_clamped(sp1_soft_clip(sig_r * master_drive));
    }

    fx->delay_write_pos = delay_wpos;
    fx->delay_active_samples = active_rem;
    fx->delay_decim_cnt = (uint8_t)decim_cnt;
    fx->delay_tap_smooth = tap_smooth;
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

    /* ── Block-Rate Setup (zero per-sample division) ── */

    fx->delay_tap_target = compute_dub_delay_target_samples(fx, step_16th_frames);
    if (!fx->delay_tap_initialized) {
        fx->delay_tap_smooth = fx->delay_tap_target;
        fx->delay_tap_initialized = true;
    }

    /* Update Dub Siren echo throw & ~1.5 s smooth feedback decay */
    if (fx->siren_wet_target > fx->siren_wet_env) {
        fx->siren_wet_env += 0.20f * (fx->siren_wet_target - fx->siren_wet_env);
    } else {
        fx->siren_wet_env *= 0.9972f;
        if (fx->siren_wet_env < 1.0e-4f) {
            fx->siren_wet_env = 0.0f;
        }
    }
    if (fx->siren_fb_target > fx->siren_fb_env) {
        fx->siren_fb_env += 0.20f * (fx->siren_fb_target - fx->siren_fb_env);
    } else {
        fx->siren_fb_env *= 0.9972f;
        if (fx->siren_fb_env < 1.0e-4f) {
            fx->siren_fb_env = 0.0f;
        }
    }

    const float eff_wet = (fx->delay_wet > fx->siren_wet_env) ? fx->delay_wet : fx->siren_wet_env;
    float eff_fb = (fx->delay_feedback > fx->siren_fb_env) ? fx->delay_feedback : fx->siren_fb_env;
    if (eff_fb > 0.87f) {
        eff_fb = 0.87f;
    }

    const bool has_send_in =
        (eff_wet > 0.0005f) && block_has_send_energy(in_send_l, in_send_r, num_frames);
    const bool run_delay = (eff_wet > 0.0005f) && (has_send_in || (fx->delay_active_samples > 0U));

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

    fx->dj_filter_smooth += 0.28f * (fx->dj_filter_target - fx->dj_filter_smooth);
    if (fabsf(fx->dj_filter_target - fx->dj_filter_smooth) < 0.001f) {
        fx->dj_filter_smooth = fx->dj_filter_target;
    }

    const float dj_pos = fx->dj_filter_smooth;
    int dj_mode = 0;

    if (dj_pos < SP1_FX_DJ_DEADZONE_LOW) {
        const float t = dj_pos * SP1_FX_INV_DJ_SPAN;
        const float cutoff_hz = 60.0f + (19940.0f * t * t);
        sp1_svf_set_params(&fx->dj_svf_l, cutoff_hz, fx->dj_resonance_q);
        sp1_svf_set_params(&fx->dj_svf_r, cutoff_hz, fx->dj_resonance_q);
        dj_mode = -1;
    } else if (dj_pos > SP1_FX_DJ_DEADZONE_HIGH) {
        const float t = (dj_pos - SP1_FX_DJ_DEADZONE_HIGH) * SP1_FX_INV_DJ_SPAN;
        const float cutoff_hz = 30.0f + (11970.0f * t * t);
        sp1_svf_set_params(&fx->dj_svf_l, cutoff_hz, fx->dj_resonance_q);
        sp1_svf_set_params(&fx->dj_svf_r, cutoff_hz, fx->dj_resonance_q);
        dj_mode = 1;
    }

    const bool crush_enabled = (fx->crush_amount > 0.005f);

    if (dj_mode == 0 && !crush_enabled &&
        fx->stutter_target_div == SP1_STUTTER_OFF &&
        fx->stutter_active_div == SP1_STUTTER_OFF) {
        process_block_normal_fast(fx, run_delay, eff_wet, eff_fb,
                                  in_main_l, in_main_r, in_send_l, in_send_r,
                                  out_interleaved_pcm, num_frames);
        return;
    }

    /* ── Division-Free Per-Sample Master FX Inner Loop (Active Performance FX) ── */

    for (uint32_t i = 0U; i < num_frames; ++i) {
        float sig_l = in_main_l[i];
        float sig_r = in_main_r[i];

        if (run_delay) {
            const float send_l = (in_send_l != NULL) ? in_send_l[i] : 0.0f;
            const float send_r = (in_send_r != NULL) ? in_send_r[i] : 0.0f;

            if (fx->delay_decim_cnt == 0U) {
                fx->delay_tap_smooth += 0.0018f * (fx->delay_tap_target - fx->delay_tap_smooth);
                const int32_t tap_i = (int32_t)fx->delay_tap_smooth;
                const float tap_frac = fx->delay_tap_smooth - (float)tap_i;
                const uint32_t r0 = (fx->delay_write_pos - (uint32_t)tap_i) & SP1_FX_DELAY_TAP_MASK;
                const uint32_t r1 = (r0 - 1U) & SP1_FX_DELAY_TAP_MASK;

                fx->delay_prev_l = fx->delay_curr_l;
                fx->delay_prev_r = fx->delay_curr_r;
                const float l0 = (float)fx->delay_buf_l[r0] * SP1_FX_INV_S16_SCALE;
                const float l1 = (float)fx->delay_buf_l[r1] * SP1_FX_INV_S16_SCALE;
                const float r_0 = (float)fx->delay_buf_r[r0] * SP1_FX_INV_S16_SCALE;
                const float r_1 = (float)fx->delay_buf_r[r1] * SP1_FX_INV_S16_SCALE;
                fx->delay_curr_l = l0 + ((l1 - l0) * tap_frac);
                fx->delay_curr_r = r_0 + ((r_1 - r_0) * tap_frac);

                fx->delay_lp_l += 0.42f * (fx->delay_curr_l - fx->delay_lp_l);
                fx->delay_lp_r += 0.42f * (fx->delay_curr_r - fx->delay_lp_r);
#if !defined(__ARM_FP) || (__ARM_FP == 0)
                if (fabsf(fx->delay_lp_l) < SP1_DSP_DENORMAL_FLOOR) {
                    fx->delay_lp_l = 0.0f;
                }
                if (fabsf(fx->delay_lp_r) < SP1_DSP_DENORMAL_FLOOR) {
                    fx->delay_lp_r = 0.0f;
                }
#endif

                const float in_bbd_l =
                    sp1_soft_clip((send_l * 0.80f) + (fx->delay_lp_r * eff_fb));
                const float in_bbd_r =
                    sp1_soft_clip((send_r * 0.35f) + (fx->delay_lp_l * eff_fb));

                if ((fabsf(in_bbd_l) + fabsf(in_bbd_r)) > 1.0e-4f) {
                    fx->delay_active_samples = SP1_FX_DELAY_TAP_SAMPLES + 64U;
                } else if (fx->delay_active_samples > 0U) {
                    fx->delay_active_samples--;
                }

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

            sig_l += dub_l * eff_wet;
            sig_r += dub_r * eff_wet;
        }

        /* 2. Tempo-Synced Beat-Repeat / Stutter Looper */
        if (fx->stutter_target_div == SP1_STUTTER_OFF) {
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

            if ((roff + SP1_FX_STUTTER_XFADE_FRAMES) >= loop_len) {
                const uint32_t rem = loop_len - roff;
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

        /* 5. Master Soft-Clipper -> int16_t Stereo Output */
        const float out_l = sp1_soft_clip(sig_l * fx->master_drive);
        const float out_r = sp1_soft_clip(sig_r * fx->master_drive);

        out_interleaved_pcm[i * 2U] = float_to_s16_clamped(out_l);
        out_interleaved_pcm[(i * 2U) + 1U] = float_to_s16_clamped(out_r);
    }
}
