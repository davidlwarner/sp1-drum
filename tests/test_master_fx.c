#include "test_harness.h"
#include "core/sequencer.h"
#include "core/ui_mapper.h"
#include "dsp/drum_voices.h"
#include "dsp/master_fx.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void init_silent_hal(void) {
    const sp1_hal_host_config_t cfg = {
        .wav_path = NULL,
        .midi_csv_path = NULL,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());
}

static void test_dj_svf_isolator_deadzone_lp_hp_and_sweep_stability(void) {
    static sp1_master_fx_t fx;
    sp1_master_fx_init(&fx);
    sp1_master_fx_set_dub_delay(&fx, 0.0f, 0.0f);
    sp1_master_fx_set_stutter(&fx, SP1_STUTTER_OFF);
    sp1_master_fx_set_bitcrusher(&fx, 0.0f);

    float in_l[SP1_AUDIO_BLOCK_FRAMES];
    float in_r[SP1_AUDIO_BLOCK_FRAMES];
    int16_t out_pcm[SP1_AUDIO_BLOCK_SAMPLES];

    /* Synthesize a two-tone test signal: 80 Hz sub-bass + 9000 Hz high frequency */
    uint32_t phase_low = 0U;
    uint32_t phase_high = 0U;
    const uint32_t inc_low = sp1_hz_to_phase_inc(80.0f);
    const uint32_t inc_high = sp1_hz_to_phase_inc(9000.0f);

    /* 1. Low-Pass mode (pos = 0.05f -> ~85 Hz cutoff): passes 80 Hz, attenuates 9 kHz */
    sp1_master_fx_set_dj_filter(&fx, 0.05f, 1.2f);
    double lp_high_only_energy = 0.0;
    double lp_low_only_energy = 0.0;

    for (uint32_t blk = 0U; blk < 20U; ++blk) {
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            phase_high += inc_high;
            in_l[i] = sp1_sine_lut(phase_high) * 0.5f;
            in_r[i] = in_l[i];
        }
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
        if (blk >= 10U) {
            for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
                const double s = (double)out_pcm[i * 2U] / 32767.0;
                lp_high_only_energy += s * s;
            }
        }
    }

    for (uint32_t blk = 0U; blk < 20U; ++blk) {
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            phase_low += inc_low;
            in_l[i] = sp1_sine_lut(phase_low) * 0.5f;
            in_r[i] = in_l[i];
        }
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
        if (blk >= 10U) {
            for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
                const double s = (double)out_pcm[i * 2U] / 32767.0;
                lp_low_only_energy += s * s;
            }
        }
    }
    TEST_ASSERT(lp_low_only_energy > (lp_high_only_energy * 20.0));

    /* 2. High-Pass mode (pos = 0.95f -> ~9.6 kHz cutoff): attenuates 80 Hz, passes 9 kHz */
    sp1_master_fx_reset(&fx);
    sp1_master_fx_set_dj_filter(&fx, 0.95f, 1.2f);
    double hp_low_only_energy = 0.0;
    double hp_high_only_energy = 0.0;

    for (uint32_t blk = 0U; blk < 20U; ++blk) {
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            phase_low += inc_low;
            in_l[i] = sp1_sine_lut(phase_low) * 0.5f;
            in_r[i] = in_l[i];
        }
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
        if (blk >= 10U) {
            for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
                const double s = (double)out_pcm[i * 2U] / 32767.0;
                hp_low_only_energy += s * s;
            }
        }
    }

    for (uint32_t blk = 0U; blk < 20U; ++blk) {
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            phase_high += inc_high;
            in_l[i] = sp1_sine_lut(phase_high) * 0.5f;
            in_r[i] = in_l[i];
        }
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
        if (blk >= 10U) {
            for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
                const double s = (double)out_pcm[i * 2U] / 32767.0;
                hp_high_only_energy += s * s;
            }
        }
    }
    TEST_ASSERT(hp_high_only_energy > (hp_low_only_energy * 20.0));

    /* 3. Extreme Q = 12.0f full-range sweep stability check (0.0 -> 1.0 -> 0.0) */
    uint32_t rng = 0x12345678U;
    for (uint32_t blk = 0U; blk < 200U; ++blk) {
        const float pos = (blk < 100U) ? ((float)blk * 0.01f) : ((float)(200U - blk) * 0.01f);
        sp1_master_fx_set_dj_filter(&fx, pos, 25.0f); /* Clamped internally to Q <= 12.0f */
        TEST_ASSERT(fx.dj_resonance_q <= 12.0f);

        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            in_l[i] = sp1_xorshift32_noise(&rng) * 0.8f;
            in_r[i] = sp1_xorshift32_noise(&rng) * 0.8f;
        }
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);

        TEST_ASSERT(!isnan(fx.dj_svf_l.ic1eq) && !isinf(fx.dj_svf_l.ic1eq));
        TEST_ASSERT(!isnan(fx.dj_svf_l.ic2eq) && !isinf(fx.dj_svf_l.ic2eq));
    }
}

static void test_stutter_looper_wrap_and_crossfade_continuity(void) {
    static sp1_master_fx_t fx;
    sp1_master_fx_init(&fx);
    sp1_master_fx_set_dub_delay(&fx, 0.0f, 0.0f);
    sp1_master_fx_set_dj_filter(&fx, 0.50f, 1.0f);
    sp1_master_fx_set_bitcrusher(&fx, 0.0f);

    float in_l[SP1_AUDIO_BLOCK_FRAMES];
    float in_r[SP1_AUDIO_BLOCK_FRAMES];
    int16_t out_pcm[SP1_AUDIO_BLOCK_SAMPLES];

    uint32_t phase = 0U;
    const uint32_t inc = sp1_hz_to_phase_inc(220.0f);

    /* Fill past the 16,384-frame circular buffer wrap boundary (200 blocks = 19,200 frames) */
    for (uint32_t blk = 0U; blk < 200U; ++blk) {
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            phase += inc;
            in_l[i] = sp1_sine_lut(phase) * 0.6f;
            in_r[i] = in_l[i];
        }
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
    }

    /* Engage 1/32 ratchet stutter while zeroing live input: output MUST continue repeating
     * the captured audio from the 64 KB circular buffer with click-free boundary wraps. */
    sp1_master_fx_set_stutter(&fx, SP1_STUTTER_DIV_1_32);
    for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
        in_l[i] = 0.0f;
        in_r[i] = 0.0f;
    }

    double stutter_energy = 0.0;
    int32_t max_step_delta = 0;
    int16_t prev_s = out_pcm[(SP1_AUDIO_BLOCK_FRAMES - 1U) * 2U];

    for (uint32_t blk = 0U; blk < 120U; ++blk) {
        sp1_master_fx_process_block(
            &fx, 6000U, in_l, in_r, NULL, NULL, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            const int16_t s = out_pcm[i * 2U];
            const int32_t d = abs((int32_t)s - (int32_t)prev_s);
            if (d > max_step_delta) {
                max_step_delta = d;
            }
            prev_s = s;
            stutter_energy += (double)s * (double)s;
        }
    }

    TEST_ASSERT(stutter_energy > 1.0e9);
    /* 220 Hz sine @ 48 kHz max natural slope is ~600 LSB/sample; crossfaded loop boundary
     * guarantees no full-scale click discontinuity (> 8000 LSB). */
    TEST_ASSERT(max_step_delta < 4500);
}

static void test_dub_ping_pong_delay_selective_send(void) {
    static sp1_master_fx_t fx;
    sp1_master_fx_init(&fx);
    sp1_master_fx_set_dub_delay(&fx, 0.80f, 0.60f);

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];
    int16_t out_pcm[SP1_AUDIO_BLOCK_SAMPLES];

    /* Inject a single 1-block burst into send_l (simulating Snare/Rimshot hit) */
    for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
        main_l[i] = 0.0f;
        main_r[i] = 0.0f;
        send_l[i] = 0.75f;
        send_r[i] = 0.0f;
    }
    sp1_master_fx_process_block(
        &fx, 1000U, main_l, main_r, send_l, send_r, out_pcm, SP1_AUDIO_BLOCK_FRAMES);

    for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
        send_l[i] = 0.0f;
    }

    /* Advance 80 blocks and verify ping-pong delay echoes appear on BOTH Left and Right */
    double echo_l = 0.0;
    double echo_r = 0.0;
    for (uint32_t blk = 0U; blk < 80U; ++blk) {
        sp1_master_fx_process_block(
            &fx, 1000U, main_l, main_r, send_l, send_r, out_pcm, SP1_AUDIO_BLOCK_FRAMES);
        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            echo_l += (double)out_pcm[i * 2U] * (double)out_pcm[i * 2U];
            echo_r += (double)out_pcm[(i * 2U) + 1U] * (double)out_pcm[(i * 2U) + 1U];
        }
    }

    TEST_ASSERT(echo_l > 1.0e6);
    TEST_ASSERT(echo_r > 1.0e6);
}

static void test_12_second_polyrhythm_stress_and_sram_budget(void) {
    /* Static SRAM Budget Verification:
     * Core + 4-Voice Synth + Master FX (including 64 KB stutter + 8 KB dub delay)
     * MUST stay well within <= 128 KB (131,072 bytes). */
    const size_t synth_bytes = sizeof(sp1_drum_synth_t);
    const size_t fx_bytes = sizeof(sp1_master_fx_t);
    const size_t seq_bytes = sizeof(sp1_sequencer_t);
    const size_t ui_bytes = sizeof(sp1_ui_mapper_t);
    const size_t total_engine_bytes = synth_bytes + fx_bytes + seq_bytes + ui_bytes;

    printf("  [SRAM Footprint] sp1_drum_synth_t=%zu B, sp1_master_fx_t=%zu B, "
           "sp1_sequencer_t=%zu B, sp1_ui_mapper_t=%zu B | Total Engine=%zu B (%.2f KB)\n",
           synth_bytes,
           fx_bytes,
           seq_bytes,
           ui_bytes,
           total_engine_bytes,
           (double)total_engine_bytes / 1024.0);

    TEST_ASSERT(synth_bytes <= 1024U);
    TEST_ASSERT(fx_bytes <= 81920U);
    TEST_ASSERT(total_engine_bytes <= 131072U); /* Strictly <= 128 KB */

    init_silent_hal();

    static sp1_sequencer_t seq;
    static sp1_drum_synth_t synth;
    static sp1_master_fx_t fx;

    sp1_seq_init(&seq);
    sp1_drum_voices_init(&synth);
    sp1_master_fx_init(&fx);

    /* Dense 4-track polymetric groove @ 145 BPM with swing & Dilla micro-timing */
    sp1_seq_set_bpm(&seq, 145.0f);
    sp1_seq_set_swing(&seq, 0.60f);
    sp1_seq_set_track_pattern(&seq, 0U, 7U, 16U, 0U);
    sp1_seq_set_track_pattern(&seq, 1U, 5U, 16U, 2U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, 19.0f);
    sp1_seq_set_track_pattern(&seq, 2U, 13U, 16U, 1U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, -14.0f);
    sp1_seq_set_track_pattern(&seq, 3U, 7U, 12U, 3U);
    sp1_seq_set_track_nudge_ms(&seq, 3U, 11.0f);
    sp1_seq_start(&seq);

    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];
    int16_t pcm[SP1_AUDIO_BLOCK_SAMPLES];
    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];

    /* 12.0 seconds @ 48 kHz = 576,000 stereo frames = 6,000 blocks of 96 frames */
    const uint32_t total_blocks = 6000U;
    uint64_t frame = 0ULL;
    int32_t max_pcm = 0;
    uint32_t total_triggers = 0U;

    for (uint32_t blk = 0U; blk < total_blocks; ++blk) {
        const float t = (float)blk / (float)total_blocks;
        sp1_drum_voices_set_snare_morph(&synth, t);
        sp1_drum_voices_set_hat_decay(&synth, 1.0f - t);
        sp1_drum_voices_set_perc_timbre(&synth, t);
        sp1_master_fx_set_dj_filter(&fx, t, 3.5f);
        sp1_master_fx_set_bitcrusher(&fx, 0.25f * t);
        sp1_master_fx_set_stutter(
            &fx, ((blk % 500U) >= 420U) ? SP1_STUTTER_DIV_1_32 : SP1_STUTTER_OFF);

        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        total_triggers += n;

        sp1_drum_voices_render_block(&synth,
                                     trigs,
                                     n,
                                     main_l,
                                     main_r,
                                     send_l,
                                     send_r,
                                     SP1_AUDIO_BLOCK_FRAMES);

        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_FRAMES; ++i) {
            TEST_ASSERT(!isnan(main_l[i]) && !isinf(main_l[i]));
            TEST_ASSERT(!isnan(main_r[i]) && !isinf(main_r[i]));
            TEST_ASSERT(!isnan(send_l[i]) && !isinf(send_l[i]));
            TEST_ASSERT(!isnan(send_r[i]) && !isinf(send_r[i]));
        }

        const uint32_t step_frames = sp1_clock_get_step_frames(sp1_seq_clock_const(&seq));
        sp1_master_fx_process_block(
            &fx, step_frames, main_l, main_r, send_l, send_r, pcm, SP1_AUDIO_BLOCK_FRAMES);

        for (uint32_t i = 0U; i < SP1_AUDIO_BLOCK_SAMPLES; ++i) {
            TEST_ASSERT(pcm[i] >= -32767);
            const int32_t a = abs((int32_t)pcm[i]);
            if (a > max_pcm) {
                max_pcm = a;
            }
        }

        sp1_hal_service_background();
        frame += (uint64_t)SP1_AUDIO_BLOCK_FRAMES;
    }

    TEST_ASSERT(total_triggers > 200U);
    TEST_ASSERT(max_pcm > 8000 && max_pcm <= 32767);

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();
}

int main(void) {
    RUN_TEST(test_dj_svf_isolator_deadzone_lp_hp_and_sweep_stability);
    RUN_TEST(test_stutter_looper_wrap_and_crossfade_continuity);
    RUN_TEST(test_dub_ping_pong_delay_selective_send);
    RUN_TEST(test_12_second_polyrhythm_stress_and_sram_budget);
    return sp1_test_summary();
}
