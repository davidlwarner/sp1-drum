#include "test_harness.h"
#include "core/ui_mapper.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <stdint.h>

static void init_silent_hal(void) {
    const sp1_hal_host_config_t cfg = {
        .wav_path = NULL,
        .midi_csv_path = NULL,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());
}

static void poll_and_update(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq) {
    sp1_controls_snapshot_t snap;
    sp1_hal_controls_poll(&snap);
    sp1_ui_mapper_update(ui, seq, &snap);
}

static void press_and_release(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq, sp1_button_id_t btn) {
    sp1_hal_controls_on_button_event(btn, true);
    poll_and_update(ui, seq);
    sp1_hal_controls_on_button_event(btn, false);
    poll_and_update(ui, seq);
}

static void test_normal_mode_faders_mutes_transport_and_bpm(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);

    /* Initial poll with untouched faders preserves default Euclidean pattern */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(4, sp1_seq_get_track_pattern(&seq, 0U)->pulses);
    TEST_ASSERT(!sp1_seq_is_running(&seq));

    /* Fader 1 -> Track 0 pulse density (50% of 16 steps = 8 pulses) */
    sp1_hal_controls_on_fader_raw(0U, 2040);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(8, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* Track buttons T1..T4 toggle mute */
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT(sp1_seq_is_track_muted(&seq, 1U));
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));

    /* Play toggles transport start/stop */
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(sp1_seq_is_running(&seq));
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(!sp1_seq_is_running(&seq));

    /* Vol- / Vol+ adjust Fine Master BPM (-2 / +2 BPM, clamped to 60..180 BPM) */
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));
    press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    TEST_ASSERT_EQ_U32(12200U, sp1_seq_get_bpm_x100(&seq));
    press_and_release(&ui, &seq, SP1_BTN_VOL_DOWN);
    press_and_release(&ui, &seq, SP1_BTN_VOL_DOWN);
    TEST_ASSERT_EQ_U32(11800U, sp1_seq_get_bpm_x100(&seq));

    sp1_hal_shutdown();
}

static void test_shift_combos_dilla_detent_coarse_bpm_swing_and_anti_clobber(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);

    /* Set Track 0 pulse density to 5 via Fader 0 */
    sp1_hal_controls_on_fader_raw(0U, 1275); /* ~5/16 */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(5, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* Center detent mapping unit check: [0.42..0.58] snaps cleanly to 0.0f ms */
    TEST_ASSERT_FLOAT_NEAR(-35.0f, sp1_ui_fader_norm_to_nudge_ms(0.0f), 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_fader_norm_to_nudge_ms(0.42f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_fader_norm_to_nudge_ms(0.50f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_fader_norm_to_nudge_ms(0.58f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(35.0f, sp1_ui_fader_norm_to_nudge_ms(1.0f), 1e-4f);

    /* Hold Function -> enter FN shift mode */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_FN, sp1_ui_mapper_active_modifier(&ui));

    /* Function + Fader 1 -> +35 ms late nudge at top of travel */
    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_SEQ_NUDGE_MAX_FRAMES, sp1_seq_get_track_nudge_frames(&seq, 0U));

    /* Function + Fader 1 inside center detent -> exact 0 ms */
    sp1_hal_controls_on_fader_raw(0U, 2040);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_track_nudge_frames(&seq, 0U));

    /* Function + Fader 1 at bottom -> -35 ms early nudge */
    sp1_hal_controls_on_fader_raw(0U, 0);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(-SP1_SEQ_NUDGE_MAX_FRAMES, sp1_seq_get_track_nudge_frames(&seq, 0U));

    /* Function + Vol+ / Vol- -> Coarse Master BPM (+10 / -10 BPM, clamped to 60..180) */
    press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    TEST_ASSERT_EQ_U32(13000U, sp1_seq_get_bpm_x100(&seq));
    for (int i = 0; i < 8; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    }
    TEST_ASSERT_EQ_U32(18000U, sp1_seq_get_bpm_x100(&seq));
    for (int i = 0; i < 15; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_DOWN);
    }
    TEST_ASSERT_EQ_U32(6000U, sp1_seq_get_bpm_x100(&seq));

    /* Function + FWD / RWD -> Global Swing (+2% / -2%, clamped to 50%..66%) */
    TEST_ASSERT_FLOAT_NEAR(0.50f, sp1_seq_get_swing(&seq), 1e-5f);
    press_and_release(&ui, &seq, SP1_BTN_FWD);
    TEST_ASSERT_FLOAT_NEAR(0.52f, sp1_seq_get_swing(&seq), 1e-5f);
    for (int i = 0; i < 10; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_FWD);
    }
    TEST_ASSERT_FLOAT_NEAR(0.66f, sp1_seq_get_swing(&seq), 1e-5f);
    press_and_release(&ui, &seq, SP1_BTN_RWD);
    TEST_ASSERT_FLOAT_NEAR(0.64f, sp1_seq_get_swing(&seq), 1e-5f);

    /* Function + T1 -> quick-reset Track 0 Dilla nudge to exact 0 ms */
    press_and_release(&ui, &seq, SP1_BTN_TRACK_1);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_track_nudge_frames(&seq, 0U));

    /* Release Function while Fader 0 sits at 0 -> anti-clobber preserves Track 0 pulses == 5! */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, false);
    poll_and_update(&ui, &seq);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));
    TEST_ASSERT_EQ_INT(5, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* RWD + Fader 2 -> Track 1 rotation; RWD + T2 resets rotation to 0 */
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_RWD, sp1_ui_mapper_active_modifier(&ui));
    sp1_hal_controls_on_fader_raw(1U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(15, sp1_seq_get_track_pattern(&seq, 1U)->rotation);
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_track_pattern(&seq, 1U)->rotation);
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, false);
    poll_and_update(&ui, &seq);

    /* FWD + Fader 3 -> Track 2 polymeter step length (1..32); FWD + T3 resets to 16 */
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_FWD, sp1_ui_mapper_active_modifier(&ui));
    sp1_hal_controls_on_fader_raw(2U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(32, sp1_seq_get_track_pattern(&seq, 2U)->steps);
    press_and_release(&ui, &seq, SP1_BTN_TRACK_3);
    TEST_ASSERT_EQ_INT(16, sp1_seq_get_track_pattern(&seq, 2U)->steps);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);

    sp1_hal_shutdown();
}

static void test_play_shift_chord_hardware_safety(void) {
    /* CRITICAL SP-1 HARDWARE CONSTRAINT: Play shares AIN0 resistor ladder with T1..T4
     * and MUST NEVER trigger a modifier mode, reset action, mute toggle, or fader edit! */
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);

    /* Set distinct non-default parameters on all 4 tracks */
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_set_track_pattern(&seq, t, 5U, 12U, 3U);
        sp1_seq_set_track_nudge_ms(&seq, t, 15.0f);
        sp1_seq_set_track_mute(&seq, t, false);
    }
    sp1_seq_set_bpm_x100(&seq, 12400U);
    sp1_seq_set_swing(&seq, 0.58f);

    poll_and_update(&ui, &seq);

    /* Hold Play and synthetically press every Track button T1..T4 + Vol-/Vol+ + move Fader 0 */
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));

    const sp1_button_id_t tracks[4] = {
        SP1_BTN_TRACK_1, SP1_BTN_TRACK_2, SP1_BTN_TRACK_3, SP1_BTN_TRACK_4
    };
    for (uint8_t t = 0U; t < 4U; ++t) {
        sp1_hal_controls_on_button_event(tracks[t], true);
        poll_and_update(&ui, &seq);
        TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));
        sp1_hal_controls_on_button_event(tracks[t], false);
        poll_and_update(&ui, &seq);
    }

    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));

    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);

    /* Confirm ZERO parameters or mute states were altered by Play + T1..T4 / Fader */
    TEST_ASSERT_EQ_U32(12400U, sp1_seq_get_bpm_x100(&seq));
    TEST_ASSERT_FLOAT_NEAR(0.58f, sp1_seq_get_swing(&seq), 1e-5f);
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(&seq, t);
        TEST_ASSERT_EQ_INT(5, pat->pulses);
        TEST_ASSERT_EQ_INT(12, pat->steps);
        TEST_ASSERT_EQ_INT(3, pat->rotation);
        TEST_ASSERT_EQ_INT(720, sp1_seq_get_track_nudge_frames(&seq, t));
        TEST_ASSERT(!sp1_seq_is_track_muted(&seq, t));
    }

    sp1_hal_shutdown();
}

static void test_8ch_pwm_led_renderer_pulse_mute_beat_and_nudge_polarity(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);

    sp1_seq_set_track_mute(&seq, 1U, true);
    sp1_seq_start(&seq);

    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];
    const uint32_t n = sp1_seq_process_block(
        &seq, 0ULL, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
    TEST_ASSERT(n > 0U);
    sp1_ui_mapper_on_triggers(&ui, trigs, n);

    sp1_led_frame_t leds;
    sp1_ui_mapper_render_leds(&ui, &seq, 0U, &leds);

    /* Track 0 (accented downbeat hit): bright flash; Track 1 (muted): dim 8 */
    TEST_ASSERT_EQ_INT(SP1_UI_LED_TRACK_HIT_ACCENT, leds.track[0]);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_TRACK_MUTE_DIM, leds.track[1]);
    /* Beat 0 running: Play LED 0 bright, Play LEDs 1..3 off in internal mode */
    TEST_ASSERT_EQ_INT(255, leds.play[0]);
    TEST_ASSERT_EQ_INT(0, leds.play[1]);

    /* After 100 ms decay, Track 0 relaxes to active idle glow (32) */
    sp1_ui_mapper_render_leds(&ui, &seq, 100U, &leds);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_TRACK_ACTIVE_IDLE, leds.track[0]);

    /* Hold Function -> Play LEDs render bipolar Dilla nudge polarity for focused Track 0 */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    poll_and_update(&ui, &seq);

    /* Exact 0 ms grid lock -> centered pair Play[1..2] lit, outer Play[0,3] off */
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);
    sp1_ui_mapper_render_leds(&ui, &seq, 2U, &leds);
    TEST_ASSERT_EQ_INT(0, leds.play[0]);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_NUDGE_CENTER, leds.play[1]);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_NUDGE_CENTER, leds.play[2]);
    TEST_ASSERT_EQ_INT(0, leds.play[3]);

    /* Negative nudge (-25 ms early/rushed) -> left LEDs Play[0..1] lit, right Play[2..3] off */
    sp1_seq_set_track_nudge_ms(&seq, 0U, -25.0f);
    sp1_ui_mapper_render_leds(&ui, &seq, 2U, &leds);
    TEST_ASSERT(leds.play[0] > 0U && leds.play[1] == 255U);
    TEST_ASSERT_EQ_INT(0, leds.play[2]);
    TEST_ASSERT_EQ_INT(0, leds.play[3]);

    /* Positive nudge (+25 ms late/dragged) -> right LEDs Play[2..3] lit, left Play[0..1] off */
    sp1_seq_set_track_nudge_ms(&seq, 0U, 25.0f);
    sp1_ui_mapper_render_leds(&ui, &seq, 2U, &leds);
    TEST_ASSERT_EQ_INT(0, leds.play[0]);
    TEST_ASSERT_EQ_INT(0, leds.play[1]);
    TEST_ASSERT(leds.play[2] == 255U && leds.play[3] > 0U);

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();
}

int main(void) {
    RUN_TEST(test_normal_mode_faders_mutes_transport_and_bpm);
    RUN_TEST(test_shift_combos_dilla_detent_coarse_bpm_swing_and_anti_clobber);
    RUN_TEST(test_play_shift_chord_hardware_safety);
    RUN_TEST(test_8ch_pwm_led_renderer_pulse_mute_beat_and_nudge_polarity);
    return sp1_test_summary();
}
