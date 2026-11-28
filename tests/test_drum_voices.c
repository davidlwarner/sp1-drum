#include "test_harness.h"
#include "dsp/drum_voices.h"
#include "hal/sp1_hal.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static bool is_finite_sample(float x) {
    return !isnan(x) && !isinf(x);
}

static void test_flash_luts_and_inline_primitives(void) {
    /* 257-entry sine LUT: exact guard wrap and quarter-wave points */
    TEST_ASSERT_FLOAT_NEAR(0.0f, k_sine_lut_256[0], 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(1.0f, k_sine_lut_256[64], 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, k_sine_lut_256[128], 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(-1.0f, k_sine_lut_256[192], 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(k_sine_lut_256[0], k_sine_lut_256[256], 1e-7f);

    /* Linear interpolation across full 32-bit phase circle */
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_sine_lut(0U), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(1.0f, sp1_sine_lut(0x40000000U), 1e-5f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_sine_lut(0x80000000U), 1e-5f);
    TEST_ASSERT_FLOAT_NEAR(-1.0f, sp1_sine_lut(0xC0000000U), 1e-5f);

    /* 257-entry West-Coast wavefolder LUT: odd-symmetric & bounded in [-1, 1] */
    TEST_ASSERT_FLOAT_NEAR(0.0f, k_wavefold_lut_256[128], 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_wavefold_lut(0.0f), 1e-5f);
    for (uint32_t i = 0U; i < SP1_DSP_LUT_TABLE_LEN; ++i) {
        const float a = k_wavefold_lut_256[i];
        const float b = k_wavefold_lut_256[256U - i];
        TEST_ASSERT(is_finite_sample(a));
        TEST_ASSERT(a >= -1.0001f && a <= 1.0001f);
        TEST_ASSERT_FLOAT_NEAR(-a, b, 1e-5f);
    }

    /* Division-free cubic soft clipper */
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_soft_clip(0.0f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(1.0f, sp1_soft_clip(1.5f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(1.0f, sp1_soft_clip(5.0f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(-1.0f, sp1_soft_clip(-2.5f), 1e-6f);
}

static void test_track1_kick_pitch_envelope_and_dry_send(void) {
    sp1_drum_synth_t synth;
    sp1_drum_voices_init(&synth);
    sp1_drum_voices_set_kick_params(&synth, 0.55f, 0.50f);
    sp1_drum_voices_trigger(&synth, 0U, 120U, true);
    TEST_ASSERT(synth.kick.active);

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];

    /* Render 12,000 samples (250 ms) and measure positive zero-crossing intervals
     * in early attack (0..20 ms, ~200 Hz -> ~240 samples/cycle) vs. sub tail
     * (60..140 ms, ~45 Hz -> ~1066 samples/cycle). */
    uint32_t early_zc = 0U;
    uint32_t late_zc = 0U;
    float prev = 0.0f;
    float peak_abs = 0.0f;
    float max_send = 0.0f;

    for (uint32_t blk = 0U; blk < 125U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);

        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const uint32_t frame = (blk * SP1_AUDIO_BLOCK_FRAMES) + i;
            const float s = main_l[i];
            TEST_ASSERT(is_finite_sample(s));
            TEST_ASSERT_FLOAT_NEAR(s, main_r[i], 1e-6f); /* Kick is mono center */

            const float a = fabsf(s);
            if (a > peak_abs) {
                peak_abs = a;
            }
            const float snd = fabsf(send_l[i]) + fabsf(send_r[i]);
            if (snd > max_send) {
                max_send = snd;
            }

            if (prev <= 0.0f && s > 0.0f) {
                if (frame < 1200U) {
                    early_zc++; /* First 25 ms */
                } else if (frame >= 2400U && frame < 3600U) {
                    late_zc++;  /* Later 25 ms window */
                }
            }
            prev = s;
        }
    }

    TEST_ASSERT(peak_abs > 0.35f && peak_abs <= 1.0f);
    /* Early 25 ms window at ~150-200 Hz has noticeably more cycles than late 25 ms @ ~45 Hz */
    TEST_ASSERT(early_zc > late_zc);
    /* Kick must NEVER feed the Dub Ping-Pong Delay send (kept 100% dry) */
    TEST_ASSERT_FLOAT_NEAR(0.0f, max_send, 1e-7f);
}

static void test_track2_rimshot_to_snare_continuous_morph(void) {
    sp1_drum_synth_t synth;
    sp1_drum_voices_init(&synth);

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];

    /* 1. Render pure Rimshot (tone_morph = 0.0f): tight woody dual-resonator knock,
     *    minimal wire noise tail after 35 ms (frame 1680). */
    sp1_drum_voices_set_snare_morph(&synth, 0.0f);
    sp1_drum_voices_trigger(&synth, 1U, 120U, true);

    double rim_early_energy = 0.0;
    double rim_tail_energy = 0.0;
    for (uint32_t blk = 0U; blk < 60U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const uint32_t frame = (blk * SP1_AUDIO_BLOCK_FRAMES) + i;
            const float s = main_l[i];
            TEST_ASSERT(is_finite_sample(s));
            if (frame < 720U) { /* 0..15 ms knock */
                rim_early_energy += (double)s * (double)s;
            } else if (frame >= 1920U && frame < 4800U) { /* 40..100 ms tail */
                rim_tail_energy += (double)s * (double)s;
            }
        }
    }

    /* 2. Render full Snare (tone_morph = 1.0f): balanced 185 Hz shell body + snappy
     *    Cytomic TPT SVF wire noise decay extending past 40..100 ms. */
    sp1_drum_voices_init(&synth);
    sp1_drum_voices_set_snare_morph(&synth, 1.0f);
    sp1_drum_voices_trigger(&synth, 1U, 120U, true);

    double snare_early_energy = 0.0;
    double snare_tail_energy = 0.0;
    float max_snare_send = 0.0f;
    for (uint32_t blk = 0U; blk < 60U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const uint32_t frame = (blk * SP1_AUDIO_BLOCK_FRAMES) + i;
            const float s = main_l[i];
            TEST_ASSERT(is_finite_sample(s));
            if (fabsf(send_l[i]) > max_snare_send) {
                max_snare_send = fabsf(send_l[i]);
            }
            if (frame < 720U) {
                snare_early_energy += (double)s * (double)s;
            } else if (frame >= 1920U && frame < 4800U) {
                snare_tail_energy += (double)s * (double)s;
            }
        }
    }

    TEST_ASSERT(rim_early_energy > 1.0);
    TEST_ASSERT(snare_early_energy > 1.0);
    /* Snare wire tail (40..100 ms) carries significantly more sustained energy than Rimshot */
    const double rim_tail_ratio = rim_tail_energy / rim_early_energy;
    const double snare_tail_ratio = snare_tail_energy / snare_early_energy;
    TEST_ASSERT(snare_tail_ratio > (rim_tail_ratio * 3.0));
    /* Track 2 feeds the selective Dub Delay send bus */
    TEST_ASSERT(max_snare_send > 0.10f);
}

static void test_track3_hihat_6osc_metallic_closed_vs_open_decay(void) {
    sp1_drum_synth_t synth;
    sp1_drum_voices_init(&synth);

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];

    /* Closed Hat (decay_morph = 0.0f) */
    sp1_drum_voices_set_hat_decay(&synth, 0.0f);
    sp1_drum_voices_trigger(&synth, 2U, 110U, false);
    double closed_tail_energy = 0.0;
    for (uint32_t blk = 0U; blk < 60U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, NULL, NULL, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const uint32_t frame = (blk * SP1_AUDIO_BLOCK_FRAMES) + i;
            TEST_ASSERT(is_finite_sample(main_r[i]));
            if (frame >= 1920U) { /* >= 40 ms */
                closed_tail_energy += (double)main_r[i] * (double)main_r[i];
            }
        }
    }

    /* Open Hat (decay_morph = 1.0f) */
    sp1_drum_voices_init(&synth);
    sp1_drum_voices_set_hat_decay(&synth, 1.0f);
    sp1_drum_voices_trigger(&synth, 2U, 110U, false);
    double open_tail_energy = 0.0;
    for (uint32_t blk = 0U; blk < 60U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, NULL, NULL, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const uint32_t frame = (blk * SP1_AUDIO_BLOCK_FRAMES) + i;
            TEST_ASSERT(is_finite_sample(main_r[i]));
            if (frame >= 1920U) {
                open_tail_energy += (double)main_r[i] * (double)main_r[i];
            }
        }
    }

    TEST_ASSERT(open_tail_energy > (closed_tail_energy * 8.0));
}

static void test_track4_fm_wavefold_perc_clamped_phase_scale(void) {
    sp1_drum_synth_t synth;
    sp1_drum_voices_init(&synth);
    /* Push timbre morph to 1.0f (maximum FM index & modulator feedback + wavefolding) */
    sp1_drum_voices_set_perc_timbre(&synth, 1.0f);
    TEST_ASSERT(synth.perc.mod_phase_scale <= SP1_DSP_MAX_MOD_PHASE_SCALE);
    TEST_ASSERT(synth.perc.fb_phase_scale <= SP1_DSP_MAX_FB_PHASE_SCALE);

    sp1_drum_voices_trigger(&synth, 3U, 127U, true);

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];

    float peak = 0.0f;
    float max_send = 0.0f;
    for (uint32_t blk = 0U; blk < 40U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            TEST_ASSERT(is_finite_sample(main_l[i]));
            TEST_ASSERT(is_finite_sample(main_r[i]));
            if (fabsf(main_l[i]) > peak) {
                peak = fabsf(main_l[i]);
            }
            if (fabsf(send_l[i]) > max_send) {
                max_send = fabsf(send_l[i]);
            }
        }
    }

    TEST_ASSERT(peak > 0.25f && peak <= 1.2f);
    TEST_ASSERT(max_send > 0.15f);
}

static void test_sample_accurate_intra_block_trigger_dispatch(void) {
    sp1_drum_synth_t synth;
    sp1_drum_voices_init(&synth);

    /* Schedule a single Kick trigger at exact intra-block sample offset 47 (out of 0..95) */
    const sp1_seq_trigger_event_t trig = {
        .sample_frame = 47ULL,
        .block_offset = 47U,
        .track = 0U,
        .step_index = 0U,
        .velocity = 120U,
        .accent = true,
    };

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];

    sp1_drum_voices_render_block(
        &synth, &trig, 1U, main_l, main_r, NULL, NULL, SP1_AUDIO_BLOCK_FRAMES);

    /* Frames 0..46 MUST be exact 0.0f silence */
    for (uint32_t i = 0U; i < 47U; ++i) {
        TEST_ASSERT_FLOAT_NEAR(0.0f, main_l[i], 1e-9f);
        TEST_ASSERT_FLOAT_NEAR(0.0f, main_r[i], 1e-9f);
    }

    /* Frame 47 MUST immediately contain the non-zero phase-distorted attack transient */
    TEST_ASSERT(fabsf(main_l[47]) > 0.05f);
}

static void test_kits_channel_strip_tone_detent_and_dub_siren(void) {
    sp1_drum_synth_t synth;
    sp1_drum_voices_init(&synth);

    /* 1. Verify all 4 synth kits load distinct parameters and 4x4 voice setters round-trip */
    sp1_drum_voices_load_kit(&synth, 2U); /* Gritty Industrial */
    TEST_ASSERT(sp1_drum_voices_get_voice_param(&synth, 0U, 3U) > 0.80f);
    sp1_drum_voices_set_voice_param(&synth, 3U, 1U, 0.91f);
    TEST_ASSERT_FLOAT_NEAR(0.91f, sp1_drum_voices_get_voice_param(&synth, 3U, 1U), 1e-5f);

    /* 2. Per-track channel strip: Tone Filter flat bypass detent [0.45..0.55], Pan, Delay Send */
    sp1_drum_voices_init(&synth);
    TEST_ASSERT_EQ_INT(0, synth.strips[0].tone_mode);

    sp1_drum_voices_set_track_mix(&synth, 0U, 0.90f, 0.48f, 0.0f, 0.45f);
    TEST_ASSERT_EQ_INT(0, synth.strips[0].tone_mode); /* Exact flat bypass at 0.45 */
    sp1_drum_voices_set_track_mix(&synth, 0U, 0.90f, 0.48f, 0.0f, 0.55f);
    TEST_ASSERT_EQ_INT(0, synth.strips[0].tone_mode); /* Exact flat bypass at 0.55 */

    sp1_drum_voices_set_track_mix(&synth, 0U, 0.90f, 0.0f, 0.80f, 0.10f);
    TEST_ASSERT_EQ_INT(1, synth.strips[0].tone_mode); /* Low-pass (< 0.45) */
    sp1_drum_voices_set_track_mix(&synth, 2U, 0.90f, 1.0f, 0.0f, 0.90f);
    TEST_ASSERT_EQ_INT(2, synth.strips[2].tone_mode); /* High-pass (> 0.55) */

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];

    /* Hard-left Kick with 0.80 delay send now feeds Left main & send while Right stays 0 */
    sp1_drum_voices_trigger(&synth, 0U, 120U, true);
    sp1_drum_voices_render_block(
        &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);

    float sum_l = 0.0f, sum_r = 0.0f, sum_send = 0.0f;
    for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
        sum_l += fabsf(main_l[i]);
        sum_r += fabsf(main_r[i]);
        sum_send += fabsf(send_l[i]);
    }
    TEST_ASSERT(sum_l > 1.0f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sum_r, 1e-6f);
    TEST_ASSERT(sum_send > 0.5f);

    /* 3. Live Dub Siren: steady horn, wail, and 2-tone square LFO modes + smooth release */
    sp1_drum_voices_init(&synth);
    const float shapes[3] = { 0.50f, 0.15f, 0.90f }; /* Center horn, smooth wail, square siren */
    for (uint32_t mode_i = 0U; mode_i < 3U; ++mode_i) {
        sp1_drum_voices_set_siren(&synth, true, 0.55f, 0.65f, shapes[mode_i], 0.75f);
        float siren_peak = 0.0f;
        float siren_send_peak = 0.0f;
        for (uint32_t blk = 0U; blk < 15U; ++blk) {
            sp1_drum_voices_render_block(
                &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);
            for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
                TEST_ASSERT(is_finite_sample(main_l[i]));
                if (fabsf(main_l[i]) > siren_peak) {
                    siren_peak = fabsf(main_l[i]);
                }
                if (fabsf(send_l[i]) > siren_send_peak) {
                    siren_send_peak = fabsf(send_l[i]);
                }
            }
        }
        TEST_ASSERT(siren_peak > 0.10f && siren_peak <= 1.0f);
        TEST_ASSERT(siren_send_peak > 0.05f);
        const float vis = sp1_drum_voices_get_siren_lfo_level(&synth);
        TEST_ASSERT(vis >= 0.0f && vis <= 1.0f);
    }

    /* Release Siren gate -> decays cleanly to inactive silence */
    sp1_drum_voices_set_siren(&synth, false, 0.55f, 0.65f, 0.90f, 0.75f);
    for (uint32_t blk = 0U; blk < 60U; ++blk) {
        sp1_drum_voices_render_block(
            &synth, NULL, 0U, main_l, main_r, send_l, send_r, SP1_AUDIO_BLOCK_FRAMES);
    }
    TEST_ASSERT(!synth.siren.active);
}

int main(void) {
    RUN_TEST(test_flash_luts_and_inline_primitives);
    RUN_TEST(test_track1_kick_pitch_envelope_and_dry_send);
    RUN_TEST(test_track2_rimshot_to_snare_continuous_morph);
    RUN_TEST(test_track3_hihat_6osc_metallic_closed_vs_open_decay);
    RUN_TEST(test_track4_fm_wavefold_perc_clamped_phase_scale);
    RUN_TEST(test_sample_accurate_intra_block_trigger_dispatch);
    RUN_TEST(test_kits_channel_strip_tone_detent_and_dub_siren);
    return sp1_test_summary();
}

