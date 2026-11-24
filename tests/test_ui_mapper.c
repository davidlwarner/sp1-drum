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

static void test_normal_mode_faders_mutes_transport_bpm_and_volume(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);

    /* Initial poll with untouched faders preserves default Euclidean pattern */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(4, sp1_seq_get_track_pattern(&seq, 0U)->pulses);
    TEST_ASSERT(!sp1_seq_is_running(&seq));
    TEST_ASSERT_EQ_INT(SP1_UI_VOLUME_DEFAULT, sp1_ui_mapper_get_master_volume(&ui));

    /* Fader 1 -> Track 0 pulse density (50% of 16 steps = 8 pulses) */
    sp1_hal_controls_on_fader_raw(0U, 2040);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(8, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* Track buttons T1..T4 toggle mute on release (when no fader moved while held) */
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U)); /* Mute toggles on release, not press */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(sp1_seq_is_track_muted(&seq, 1U));
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));

    /* Play toggles transport start/stop */
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(sp1_seq_is_running(&seq));
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(!sp1_seq_is_running(&seq));

    /* RWD / FWD (unshifted tap without fader movement) -> Fine Master BPM (-2 / +2 BPM) */
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));
    press_and_release(&ui, &seq, SP1_BTN_FWD);
    TEST_ASSERT_EQ_U32(12200U, sp1_seq_get_bpm_x100(&seq));
    press_and_release(&ui, &seq, SP1_BTN_RWD);
    press_and_release(&ui, &seq, SP1_BTN_RWD);
    TEST_ASSERT_EQ_U32(11800U, sp1_seq_get_bpm_x100(&seq));

    /* Vol- / Vol+ (unshifted tap) -> Master Output Volume (0..127, step +/-8, default 64) */
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui));
    press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    TEST_ASSERT_EQ_INT(72, sp1_ui_mapper_get_master_volume(&ui));
    press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    TEST_ASSERT_EQ_INT(80, sp1_ui_mapper_get_master_volume(&ui));
    for (int i = 0; i < 8; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    }
    TEST_ASSERT_EQ_INT(127, sp1_ui_mapper_get_master_volume(&ui)); /* Clamped at 127 */
    for (int i = 0; i < 20; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_DOWN);
    }
    TEST_ASSERT_EQ_INT(0, sp1_ui_mapper_get_master_volume(&ui)); /* Clamped at 0 */
    TEST_ASSERT_EQ_U32(11800U, sp1_seq_get_bpm_x100(&seq)); /* Vol-/Vol+ never alter BPM */

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
    const uint8_t init_vol = sp1_ui_mapper_get_master_volume(&ui);

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

    /* Function + FWD / RWD -> Coarse Master BPM (+10 / -10 BPM, clamped to 60..180) */
    press_and_release(&ui, &seq, SP1_BTN_FWD);
    TEST_ASSERT_EQ_U32(13000U, sp1_seq_get_bpm_x100(&seq));
    for (int i = 0; i < 8; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_FWD);
    }
    TEST_ASSERT_EQ_U32(18000U, sp1_seq_get_bpm_x100(&seq));
    for (int i = 0; i < 15; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_RWD);
    }
    TEST_ASSERT_EQ_U32(6000U, sp1_seq_get_bpm_x100(&seq));

    /* Function + Vol+ / Vol- -> Global Swing (+2% / -2%, clamped to 50%..66%) without altering volume */
    TEST_ASSERT_FLOAT_NEAR(0.50f, sp1_seq_get_swing(&seq), 1e-5f);
    press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    TEST_ASSERT_FLOAT_NEAR(0.52f, sp1_seq_get_swing(&seq), 1e-5f);
    for (int i = 0; i < 10; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    }
    TEST_ASSERT_FLOAT_NEAR(0.66f, sp1_seq_get_swing(&seq), 1e-5f);
    press_and_release(&ui, &seq, SP1_BTN_VOL_DOWN);
    TEST_ASSERT_FLOAT_NEAR(0.64f, sp1_seq_get_swing(&seq), 1e-5f);
    TEST_ASSERT_EQ_INT(init_vol, sp1_ui_mapper_get_master_volume(&ui));

    /* Function + T1 -> quick-reset Track 0 Dilla nudge to exact 0 ms (no mute toggle!) */
    press_and_release(&ui, &seq, SP1_BTN_TRACK_1);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_track_nudge_frames(&seq, 0U));
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 0U));

    /* Release Function while Fader 0 sits at 0 -> anti-clobber preserves Track 0 pulses == 5! */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, false);
    poll_and_update(&ui, &seq);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));
    TEST_ASSERT_EQ_INT(5, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* Reset BPM to 120 to verify RWD/FWD + Fader suppresses fine BPM tap on release */
    sp1_seq_set_bpm_x100(&seq, 12000U);

    /* RWD + Fader 2 -> Track 1 rotation; releasing RWD MUST NOT decrement BPM! */
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_RWD, sp1_ui_mapper_active_modifier(&ui));
    sp1_hal_controls_on_fader_raw(1U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(15, sp1_seq_get_track_pattern(&seq, 1U)->rotation);
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));

    /* RWD + T2 resets rotation to 0 and also suppresses BPM tap & Track 1 mute! */
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, true);
    poll_and_update(&ui, &seq);
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_track_pattern(&seq, 1U)->rotation);
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));

    /* FWD + Fader 3 -> Track 2 polymeter step length (1..32); releasing FWD MUST NOT increment BPM! */
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_FWD, sp1_ui_mapper_active_modifier(&ui));
    sp1_hal_controls_on_fader_raw(2U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(32, sp1_seq_get_track_pattern(&seq, 2U)->steps);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));

    /* FWD + T3 resets to 16 steps and suppresses BPM tap & Track 2 mute! */
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update(&ui, &seq);
    press_and_release(&ui, &seq, SP1_BTN_TRACK_3);
    TEST_ASSERT_EQ_INT(16, sp1_seq_get_track_pattern(&seq, 2U)->steps);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 2U));

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
    const uint8_t init_vol = sp1_ui_mapper_get_master_volume(&ui);
    const float init_kick_decay = sp1_ui_mapper_get_dsp_params(&ui)->kick_decay;

    /* Hold Play and synthetically press every Track button T1..T4 + RWD/FWD/Vol-/Vol+ + move Fader 0 */
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));

    const sp1_button_id_t tracks[4] = {
        SP1_BTN_TRACK_1, SP1_BTN_TRACK_2, SP1_BTN_TRACK_3, SP1_BTN_TRACK_4
    };
    for (uint8_t t = 0U; t < 4U; ++t) {
        sp1_hal_controls_on_button_event(tracks[t], true);
        sp1_hal_controls_on_fader_raw(t, SP1_FADER_RAW_MAX);
        poll_and_update(&ui, &seq);
        TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));
        sp1_hal_controls_on_button_event(tracks[t], false);
        poll_and_update(&ui, &seq);
    }

    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    sp1_hal_controls_on_fader_raw(0U, 0);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_UI_MOD_NONE, sp1_ui_mapper_active_modifier(&ui));

    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);

    /* Confirm ZERO parameters, volume, morph, or mute states were altered by Play + buttons/faders */
    TEST_ASSERT_EQ_U32(12400U, sp1_seq_get_bpm_x100(&seq));
    TEST_ASSERT_FLOAT_NEAR(0.58f, sp1_seq_get_swing(&seq), 1e-5f);
    TEST_ASSERT_EQ_INT(init_vol, sp1_ui_mapper_get_master_volume(&ui));
    TEST_ASSERT_FLOAT_NEAR(init_kick_decay, sp1_ui_mapper_get_dsp_params(&ui)->kick_decay, 1e-5f);
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

static void test_performance_fx_mode_and_voice_morph_gestures(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* Set distinct Euclidean pulse counts on Tracks 0..3 in Normal Mode */
    sp1_hal_controls_on_fader_raw(0U, 1530); /* 6 pulses */
    sp1_hal_controls_on_fader_raw(1U, 510);  /* 2 pulses */
    sp1_hal_controls_on_fader_raw(2U, 2040); /* 8 pulses */
    sp1_hal_controls_on_fader_raw(3U, 765);  /* 3 pulses */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(6, sp1_seq_get_track_pattern(&seq, 0U)->pulses);
    TEST_ASSERT_EQ_INT(2, sp1_seq_get_track_pattern(&seq, 1U)->pulses);
    TEST_ASSERT_EQ_INT(8, sp1_seq_get_track_pattern(&seq, 2U)->pulses);
    TEST_ASSERT_EQ_INT(3, sp1_seq_get_track_pattern(&seq, 3U)->pulses);
    TEST_ASSERT(!sp1_ui_mapper_is_fx_mode(&ui));

    /* Hold T1 + move Fader 1 -> Kick decay & drive punch (suppresses mute & preserves pulses) */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(1.0f, sp1_ui_mapper_get_dsp_params(&ui)->kick_decay, 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.95f, sp1_ui_mapper_get_dsp_params(&ui)->kick_drive, 1e-4f);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    poll_and_update(&ui, &seq);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 0U));
    TEST_ASSERT_EQ_INT(6, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* Hold T2 + move Fader 2 -> Track 2 Rimshot (0.0) <-> Snare (1.0) morph */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_fader_raw(1U, SP1_FADER_RAW_MAX); /* 1.0f = full Snare */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(1.0f, sp1_ui_mapper_get_dsp_params(&ui)->snare_morph, 1e-4f);
    sp1_hal_controls_on_fader_raw(1U, 0); /* 0.0f = pure Rimshot */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_mapper_get_dsp_params(&ui)->snare_morph, 1e-4f);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));
    TEST_ASSERT_EQ_INT(2, sp1_seq_get_track_pattern(&seq, 1U)->pulses);

    /* Hold T3 + move Fader 3 -> Track 3 Closed (0.0) <-> Open (1.0) Hi-Hat decay morph */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_fader_raw(2U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(1.0f, sp1_ui_mapper_get_dsp_params(&ui)->hat_decay, 1e-4f);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 2U));
    TEST_ASSERT_EQ_INT(8, sp1_seq_get_track_pattern(&seq, 2U)->pulses);

    /* Hold T4 + move Fader 4 -> Track 4 2-Op FM / Wavefold timbre morph */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_4, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_fader_raw(3U, 0);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_mapper_get_dsp_params(&ui)->perc_timbre, 1e-4f);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_4, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 3U));
    TEST_ASSERT_EQ_INT(3, sp1_seq_get_track_pattern(&seq, 3U)->pulses);

    /* Hardware-safe chord: Vol- (AIN1) + T1 (AIN0) toggles latched Performance FX Mode
     * without changing Master Output Volume or muting Track 0! */
    const uint8_t vol_before_fx = sp1_ui_mapper_get_master_volume(&ui);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(sp1_ui_mapper_is_fx_mode(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(vol_before_fx, sp1_ui_mapper_get_master_volume(&ui));
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 0U));

    /* In Performance FX Mode, Faders 1..4 drive DJ SVF, Stutter, Bitcrusher, and Dub Delay
     * while preserving Track 0 pulses == 6! */
    sp1_hal_controls_on_fader_raw(0U, 0);
    sp1_hal_controls_on_fader_raw(2U, 2040);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_mapper_get_dsp_params(&ui)->dj_filter_pos, 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.5f, sp1_ui_mapper_get_dsp_params(&ui)->crush_amount, 1e-3f);
    TEST_ASSERT_EQ_INT(6, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    /* Momentary T3 punch-in engages 1/32 Stutter ratchet (div = 4) and releases cleanly */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(4, sp1_ui_mapper_get_dsp_params(&ui)->stutter_div);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(0, sp1_ui_mapper_get_dsp_params(&ui)->stutter_div);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 2U));

    /* Function (GPIO P0.27) + T1 + T4 (AIN0) exits Performance FX Mode with fader anti-clobber
     * and zero accidental Track 1 / Track 4 mute toggles! */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_4, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_ui_mapper_is_fx_mode(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_4, false);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    sp1_hal_controls_on_button_event(SP1_BTN_FN, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(6, sp1_seq_get_track_pattern(&seq, 0U)->pulses);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 0U));
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 3U));

    sp1_hal_shutdown();
}

static void test_play_start_stop_and_ain1_ladder_volume_resilience(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* 1. Play start/stop toggling across multi-poll and intra-poll taps,
     *    including when transient AIN1 transport noise is present. */
    TEST_ASSERT(!sp1_seq_is_running(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(sp1_seq_is_running(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(sp1_seq_is_running(&seq));

    /* Press Play again while running -> must stop cleanly */
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_running(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_running(&seq));

    /* Intra-poll fast tap (DOWN + UP before 20 ms poll) starts and stops cleanly */
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(sp1_seq_is_running(&seq));

    /* Even if a transient AIN1 bit (e.g. Vol+) is active when Play is pressed,
     * Play still stops playback reliably. */
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_running(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    poll_and_update(&ui, &seq);

    /* 2. Vol+ / Vol- stepping when AIN1 resistor-ladder release voltage passes
     *    through FWD (1110 mV) or RWD (365 mV) on the same scan:
     *    volume must step by +/-8 and BPM must NOT change! */
    sp1_ui_mapper_init(&ui);
    sp1_seq_set_bpm_x100(&seq, 12000U);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui));

    /* Press Vol- (665 mV), then on release AIN1 transits RWD (365 mV) before 0 mV */
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, false);
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(56, sp1_ui_mapper_get_master_volume(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));

    /* Press Vol+ (1650 mV), then on release AIN1 transits FWD (1110 mV) before 0 mV */
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui)); /* No double step on release */
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));

    /* Intra-poll Vol+ and Vol- taps (DOWN + UP between 20 ms polls) */
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(72, sp1_ui_mapper_get_master_volume(&ui));

    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, true);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui));

    sp1_hal_shutdown();
}

static void test_function_power_hold_release_bounce_debounce(void) {
    sp1_ui_power_hold_t hold;

    /* 1. When initialized with wait_for_initial_release=true (e.g. just powered on while
     *    Function is still held), holding Function must NOT arm shutdown until released
     *    for >= 40 ms. */
    sp1_ui_power_hold_init(&hold, true);
    for (uint32_t t = 0U; t < 200U; ++t) {
        TEST_ASSERT(!sp1_ui_power_hold_update(&hold,
                                              true,
                                              20U,
                                              SP1_UI_POWER_HOLD_SHUTDOWN_MS,
                                              SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    }
    /* Single 20 ms release glitch (< 40 ms) does not clear wait_for_release */
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(hold.wait_for_release);
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, true, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(hold.wait_for_release);

    /* Two consecutive 20 ms released ticks (40 ms) arm the detector cleanly */
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(!hold.wait_for_release);

    /* 2. Start 3000 ms shutdown hold with periodic 20 ms contact-bounce dropouts (< 40 ms).
     *    First pressed tick arms holding=true at 0 ms; 150 subsequent 20 ms ticks (including
     *    contact-bounce dropouts that recover on the next 20 ms tick) must reach 3000 ms. */
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, true, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(hold.holding);
    TEST_ASSERT_EQ_U32(0U, hold.hold_elapsed_ms);

    for (uint32_t tick = 1U; tick <= 149U; ++tick) {
        /* Inject a 20 ms contact-bounce false release every 30 ticks (600 ms) */
        const bool bouncy_pressed = ((tick % 30U) != 0U);
        const bool triggered = sp1_ui_power_hold_update(&hold,
                                                        bouncy_pressed,
                                                        20U,
                                                        SP1_UI_POWER_HOLD_SHUTDOWN_MS,
                                                        SP1_UI_POWER_RELEASE_DEBOUNCE_MS);
        TEST_ASSERT(!triggered);
        TEST_ASSERT(hold.holding);
    }

    /* Tick 150 (3000 ms total despite five 20 ms contact bounces) triggers shutdown! */
    TEST_ASSERT(sp1_ui_power_hold_update(
        &hold, true, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT_EQ_U32(3000U, hold.hold_elapsed_ms);

    /* 3. A genuine release (>= 40 ms = two 20 ms ticks) resets the hold timer to 0. */
    sp1_ui_power_hold_init(&hold, false);
    (void)sp1_ui_power_hold_update(
        &hold, true, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS);
    (void)sp1_ui_power_hold_update(
        &hold, true, 1000U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS);
    TEST_ASSERT_EQ_U32(1000U, hold.hold_elapsed_ms);

    /* 20 ms release: still holding at 1000 ms */
    (void)sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS);
    TEST_ASSERT(hold.holding);
    TEST_ASSERT_EQ_U32(1000U, hold.hold_elapsed_ms);

    /* Second 20 ms release (40 ms total): hold resets to 0 */
    (void)sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS);
    TEST_ASSERT(!hold.holding);
    TEST_ASSERT_EQ_U32(0U, hold.hold_elapsed_ms);
}

int main(void) {
    RUN_TEST(test_normal_mode_faders_mutes_transport_bpm_and_volume);
    RUN_TEST(test_shift_combos_dilla_detent_coarse_bpm_swing_and_anti_clobber);
    RUN_TEST(test_play_shift_chord_hardware_safety);
    RUN_TEST(test_8ch_pwm_led_renderer_pulse_mute_beat_and_nudge_polarity);
    RUN_TEST(test_performance_fx_mode_and_voice_morph_gestures);
    RUN_TEST(test_play_start_stop_and_ain1_ladder_volume_resilience);
    RUN_TEST(test_function_power_hold_release_bounce_debounce);
    return sp1_test_summary();
}
