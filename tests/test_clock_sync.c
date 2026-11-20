#include "test_harness.h"
#include "core/clock_sync.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <stdint.h>

static void init_silent_host_hal(void) {
    const sp1_hal_host_config_t cfg = {
        .wav_path = NULL,
        .midi_csv_path = NULL,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());
}

static void test_frame_to_us_exact_division_free(void) {
    /* Spot-check exact floor(frame * 125 / 6) across all mod-6 remainders & large frames */
    const uint64_t probes[] = {
        0ULL, 1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 96ULL,
        1000ULL, 6000ULL, 48000ULL, 192000ULL, 4800000ULL, 0xFFFFFFFFULL
    };
    for (uint32_t i = 0U; i < (uint32_t)(sizeof(probes) / sizeof(probes[0])); ++i) {
        const uint64_t f = probes[i];
        const uint32_t expected = (uint32_t)((f * 125ULL) / 6ULL);
        TEST_ASSERT_EQ_U32(expected, sp1_clock_frame_to_us(f));
    }
}

static void test_internal_master_120bpm_exact_ticks(void) {
    init_silent_host_hal();

    sp1_clock_sync_t clk;
    sp1_clock_init(&clk);
    sp1_clock_set_bpm_x100(&clk, 12000U);
    TEST_ASSERT_EQ_INT(SP1_CLOCK_MODE_INTERNAL, sp1_clock_get_mode(&clk));
    TEST_ASSERT_EQ_U32(6000U, sp1_clock_get_step_frames(&clk));

    sp1_clock_start(&clk);

    /* Render 1.0 s (48,000 frames = 500 blocks of 96 frames) */
    uint32_t start_evts = 0U;
    uint32_t tick_evts = 0U;
    sp1_clock_event_t evts[SP1_CLOCK_MAX_BLOCK_EVENTS];

    for (uint64_t frame = 0ULL; frame < 48000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        const uint32_t n = sp1_clock_process_block(
            &clk, frame, SP1_AUDIO_BLOCK_FRAMES, evts, SP1_CLOCK_MAX_BLOCK_EVENTS);
        for (uint32_t i = 0U; i < n; ++i) {
            if (evts[i].type == SP1_CLOCK_EVT_START) {
                TEST_ASSERT_EQ_U32(0U, (uint32_t)evts[i].sample_frame);
                start_evts++;
            } else if (evts[i].type == SP1_CLOCK_EVT_TICK) {
                /* 120 BPM @ 48 kHz = exact 1000 frames per 24 PPQN tick */
                TEST_ASSERT_EQ_U32(tick_evts * 1000U, (uint32_t)evts[i].sample_frame);
                TEST_ASSERT_EQ_U32(tick_evts, evts[i].tick_index);
                tick_evts++;
            }
        }
        sp1_hal_service_background();
    }

    sp1_clock_stop(&clk);
    sp1_hal_shutdown();

    TEST_ASSERT_EQ_U32(1U, start_evts);
    TEST_ASSERT_EQ_U32(48U, tick_evts);

    sp1_hal_host_stats_t stats;
    sp1_hal_host_get_stats(&stats);
    TEST_ASSERT_EQ_U32(1U, stats.midi_tx_start_count);
    TEST_ASSERT_EQ_U32(48U, stats.midi_tx_clock_count);
    TEST_ASSERT_EQ_U32(1U, stats.midi_tx_stop_count);
    TEST_ASSERT_EQ_U32(0U, stats.midi_tx_dropped);
}

static void test_internal_master_zero_drift_fractional_tempos(void) {
    /* 10.0 s (480,000 frames):
     *   93.00 BPM -> 10 * 93 * 24 / 60 = 372 exact ticks
     *  127.50 BPM -> 10 * 127.5 * 24 / 60 = 510 exact ticks */
    const uint32_t bpms[2] = { 9300U, 12750U };
    const uint32_t expected_ticks[2] = { 372U, 510U };

    for (uint32_t tc = 0U; tc < 2U; ++tc) {
        init_silent_host_hal();

        sp1_clock_sync_t clk;
        sp1_clock_init(&clk);
        sp1_clock_set_bpm_x100(&clk, bpms[tc]);
        sp1_clock_start(&clk);

        uint32_t tick_count = 0U;
        sp1_clock_event_t evts[SP1_CLOCK_MAX_BLOCK_EVENTS];
        for (uint64_t frame = 0ULL; frame < 480000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
            const uint32_t n = sp1_clock_process_block(
                &clk, frame, SP1_AUDIO_BLOCK_FRAMES, evts, SP1_CLOCK_MAX_BLOCK_EVENTS);
            for (uint32_t i = 0U; i < n; ++i) {
                if (evts[i].type == SP1_CLOCK_EVT_TICK) {
                    tick_count++;
                }
            }
            sp1_hal_service_background();
        }

        sp1_clock_stop(&clk);
        sp1_hal_shutdown();

        TEST_ASSERT_EQ_U32(expected_ticks[tc], tick_count);
    }
}

static void test_external_auto_follower_lock_and_500ms_fallback(void) {
    init_silent_host_hal();

    sp1_clock_sync_t clk;
    sp1_clock_init(&clk);
    sp1_clock_set_bpm_x100(&clk, 12000U);

    /* Inject MIDI Start (0xFA) + 25 external 0xF8 ticks at 140.00 BPM.
     * 140 BPM => 60,000,000 / (140 * 24) = 17,857.142857 us (~857.142857 frames/tick). */
    const uint64_t ext_period_num = 288000000ULL;
    const uint64_t ext_period_den = 14000ULL * 24ULL; /* 336,000 */

    uint32_t ext_ticks_sent = 0U;
    uint32_t ext_ticks_received = 0U;
    uint32_t start_received = 0U;
    uint32_t mode_changes = 0U;
    uint64_t next_ext_frame = 96ULL;
    uint64_t last_ext_sent_frame = 0ULL;

    /* Inject Start right before block at frame 96 */
    const sp1_midi_msg_t start_msg =
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_START, sp1_clock_frame_to_us(96ULL));
    TEST_ASSERT(sp1_hal_host_inject_rx_midi(&start_msg));

    sp1_clock_event_t evts[SP1_CLOCK_MAX_BLOCK_EVENTS];
    uint64_t frame = 0ULL;

    /* Phase 1: stream 25 external 0xF8 ticks (~450 ms) */
    while (ext_ticks_sent < 25U) {
        if (next_ext_frame >= frame && next_ext_frame < (frame + SP1_AUDIO_BLOCK_FRAMES)) {
            const sp1_midi_msg_t tick_msg = sp1_midi_msg_realtime(
                SP1_MIDI_STATUS_CLOCK, sp1_clock_frame_to_us(next_ext_frame));
            TEST_ASSERT(sp1_hal_host_inject_rx_midi(&tick_msg));
            last_ext_sent_frame = frame;
            ext_ticks_sent++;
            next_ext_frame =
                96ULL + ((uint64_t)ext_ticks_sent * ext_period_num) / ext_period_den;
        }

        const uint32_t n = sp1_clock_process_block(
            &clk, frame, SP1_AUDIO_BLOCK_FRAMES, evts, SP1_CLOCK_MAX_BLOCK_EVENTS);
        for (uint32_t i = 0U; i < n; ++i) {
            if (evts[i].type == SP1_CLOCK_EVT_START) {
                start_received++;
            } else if (evts[i].type == SP1_CLOCK_EVT_TICK) {
                ext_ticks_received++;
            } else if (evts[i].type == SP1_CLOCK_EVT_MODE_CHANGE) {
                mode_changes++;
            }
        }
        sp1_hal_service_background();
        frame += SP1_AUDIO_BLOCK_FRAMES;
    }

    TEST_ASSERT_EQ_INT(SP1_CLOCK_MODE_EXTERNAL, sp1_clock_get_mode(&clk));
    TEST_ASSERT(sp1_clock_is_running(&clk));
    TEST_ASSERT_EQ_U32(1U, start_received);
    TEST_ASSERT_EQ_U32(25U, ext_ticks_received);
    TEST_ASSERT_EQ_U32(1U, mode_changes);

    /* Confirm zero internal 0xF8 echo broadcasts while in EXTERNAL mode (no feedback loop). */
    sp1_hal_host_stats_t stats_mid;
    sp1_hal_host_get_stats(&stats_mid);
    TEST_ASSERT_EQ_U32(0U, stats_mid.midi_tx_clock_count);
    TEST_ASSERT_EQ_U32(0U, stats_mid.midi_tx_start_count);

    /* Confirm PLL converged within +/- 0.50 BPM of 140.00 BPM. */
    const uint32_t locked_bpm_x100 = sp1_clock_get_bpm_x100(&clk);
    TEST_ASSERT(locked_bpm_x100 >= 13950U && locked_bpm_x100 <= 14050U);

    /* Phase 2: stop injecting external 0xF8 and verify exact 500 ms (24,000-frame) timeout. */
    const uint64_t timeout_target = last_ext_sent_frame + SP1_CLOCK_EXT_TIMEOUT_FRAMES;

    while ((frame + SP1_AUDIO_BLOCK_FRAMES) < timeout_target) {
        const uint32_t n = sp1_clock_process_block(
            &clk, frame, SP1_AUDIO_BLOCK_FRAMES, evts, SP1_CLOCK_MAX_BLOCK_EVENTS);
        TEST_ASSERT_EQ_U32(0U, n);
        TEST_ASSERT_EQ_INT(SP1_CLOCK_MODE_EXTERNAL, sp1_clock_get_mode(&clk));
        sp1_hal_service_background();
        frame += SP1_AUDIO_BLOCK_FRAMES;
    }

    /* Advance across the 500 ms boundary -> must fall back cleanly to INTERNAL with no burst. */
    uint32_t resumed_ticks = 0U;
    bool saw_fallback_evt = false;
    for (uint32_t b = 0U; b < 20U; ++b) {
        const uint32_t n = sp1_clock_process_block(
            &clk, frame, SP1_AUDIO_BLOCK_FRAMES, evts, SP1_CLOCK_MAX_BLOCK_EVENTS);
        uint32_t ticks_in_block = 0U;
        for (uint32_t i = 0U; i < n; ++i) {
            if (evts[i].type == SP1_CLOCK_EVT_MODE_CHANGE) {
                saw_fallback_evt = true;
            } else if (evts[i].type == SP1_CLOCK_EVT_TICK) {
                ticks_in_block++;
                resumed_ticks++;
            }
        }
        /* Never more than 1 tick in a 96-frame block (zero catch-up burst). */
        TEST_ASSERT(ticks_in_block <= 1U);
        sp1_hal_service_background();
        frame += SP1_AUDIO_BLOCK_FRAMES;
    }

    TEST_ASSERT(saw_fallback_evt);
    TEST_ASSERT_EQ_INT(SP1_CLOCK_MODE_INTERNAL, sp1_clock_get_mode(&clk));
    TEST_ASSERT(resumed_ticks >= 2U);

    /* Adopted external ~140 BPM tempo smoothly across fallback. */
    const uint32_t fallback_bpm_x100 = sp1_clock_get_bpm_x100(&clk);
    TEST_ASSERT(fallback_bpm_x100 >= 13950U && fallback_bpm_x100 <= 14050U);

    /* Verify internal 0xF8 TX resumed cleanly after fallback. */
    sp1_hal_host_stats_t stats_end;
    sp1_hal_host_get_stats(&stats_end);
    TEST_ASSERT_EQ_U32(resumed_ticks, stats_end.midi_tx_clock_count);

    /* Verify external 0xFC (Stop) halts transport cleanly. */
    const sp1_midi_msg_t ext_stop =
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_STOP, sp1_clock_frame_to_us(frame));
    TEST_ASSERT(sp1_hal_host_inject_rx_midi(&ext_stop));
    const uint32_t stop_n = sp1_clock_process_block(
        &clk, frame, SP1_AUDIO_BLOCK_FRAMES, evts, SP1_CLOCK_MAX_BLOCK_EVENTS);
    bool saw_stop_evt = false;
    for (uint32_t i = 0U; i < stop_n; ++i) {
        if (evts[i].type == SP1_CLOCK_EVT_STOP) {
            saw_stop_evt = true;
        }
    }
    TEST_ASSERT(saw_stop_evt);
    TEST_ASSERT(!sp1_clock_is_running(&clk));

    sp1_hal_shutdown();
}

int main(void) {
    RUN_TEST(test_frame_to_us_exact_division_free);
    RUN_TEST(test_internal_master_120bpm_exact_ticks);
    RUN_TEST(test_internal_master_zero_drift_fractional_tempos);
    RUN_TEST(test_external_auto_follower_lock_and_500ms_fallback);
    return sp1_test_summary();
}
