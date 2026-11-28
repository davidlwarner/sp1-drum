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

static void poll_and_update_dt(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq, uint32_t dt_ms) {
    sp1_controls_snapshot_t snap;
    sp1_hal_controls_poll(&snap);
    sp1_ui_mapper_update_ex(ui, seq, &snap, dt_ms);
}

static void poll_and_update(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq) {
    poll_and_update_dt(ui, seq, 20U);
}

static void press_and_release(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq, sp1_button_id_t btn) {
    sp1_hal_controls_on_button_event(btn, true);
    poll_and_update(ui, seq);
    sp1_hal_controls_on_button_event(btn, false);
    poll_and_update(ui, seq);
}

static void test_four_led_widget_primitives(void) {
    uint8_t leds[SP1_NUM_TRACK_LEDS];

    /* 1. UNBALANCED_BAR: progressive fill LED 0 -> 3 */
    sp1_ui_led_render_unbalanced(0.0f, leds);
    TEST_ASSERT_EQ_INT(0, leds[0]);
    TEST_ASSERT_EQ_INT(0, leds[3]);

    sp1_ui_led_render_unbalanced(0.50f, leds);
    TEST_ASSERT_EQ_INT(255, leds[0]);
    TEST_ASSERT_EQ_INT(255, leds[1]);
    TEST_ASSERT_EQ_INT(0, leds[2]);
    TEST_ASSERT_EQ_INT(0, leds[3]);

    sp1_ui_led_render_unbalanced(1.0f, leds);
    for (uint8_t i = 0U; i < SP1_NUM_TRACK_LEDS; ++i) {
        TEST_ASSERT_EQ_INT(255, leds[i]);
    }

    /* 2. MIDDLE_BALANCED: center pair [1,2] lit at 0.0, leans left (<0) or right (>0) */
    sp1_ui_led_render_middle_balanced(0.0f, leds);
    TEST_ASSERT_EQ_INT(0, leds[0]);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_NUDGE_CENTER, leds[1]);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_NUDGE_CENTER, leds[2]);
    TEST_ASSERT_EQ_INT(0, leds[3]);

    sp1_ui_led_render_middle_balanced(-1.0f, leds);
    TEST_ASSERT_EQ_INT(255, leds[0]);
    TEST_ASSERT_EQ_INT(255, leds[1]);
    TEST_ASSERT_EQ_INT(0, leds[2]);
    TEST_ASSERT_EQ_INT(0, leds[3]);

    sp1_ui_led_render_middle_balanced(1.0f, leds);
    TEST_ASSERT_EQ_INT(0, leds[0]);
    TEST_ASSERT_EQ_INT(0, leds[1]);
    TEST_ASSERT_EQ_INT(255, leds[2]);
    TEST_ASSERT_EQ_INT(255, leds[3]);

    /* 3. BASE2_COUNTER: mod-16 loop (0, 16, 32 -> 0000; 1, 17 -> 0001; 15, 31 -> 1111) */
    const uint32_t zero_cases[3] = { 0U, 16U, 32U };
    for (uint32_t k = 0U; k < 3U; ++k) {
        sp1_ui_led_render_base2(zero_cases[k], leds);
        for (uint8_t i = 0U; i < SP1_NUM_TRACK_LEDS; ++i) {
            TEST_ASSERT_EQ_INT(0, leds[i]);
        }
    }

    const uint32_t one_cases[2] = { 1U, 17U };
    for (uint32_t k = 0U; k < 2U; ++k) {
        sp1_ui_led_render_base2(one_cases[k], leds);
        TEST_ASSERT_EQ_INT(0, leds[0]);
        TEST_ASSERT_EQ_INT(0, leds[1]);
        TEST_ASSERT_EQ_INT(0, leds[2]);
        TEST_ASSERT_EQ_INT(220, leds[3]);
    }

    const uint32_t fifteen_cases[2] = { 15U, 31U };
    for (uint32_t k = 0U; k < 2U; ++k) {
        sp1_ui_led_render_base2(fifteen_cases[k], leds);
        for (uint8_t i = 0U; i < SP1_NUM_TRACK_LEDS; ++i) {
            TEST_ASSERT_EQ_INT(220, leds[i]);
        }
    }

    /* 4. DISCRETE_SELECT: 1-of-4 selector and 6-slot tempo division selector */
    sp1_ui_led_render_discrete(2U, 4U, leds);
    TEST_ASSERT_EQ_INT(0, leds[0]);
    TEST_ASSERT_EQ_INT(0, leds[1]);
    TEST_ASSERT_EQ_INT(255, leds[2]);
    TEST_ASSERT_EQ_INT(0, leds[3]);

    sp1_ui_led_render_discrete(1U, 6U, leds); /* 1/16 bridges LEDs 0 & 1 */
    TEST_ASSERT_EQ_INT(180, leds[0]);
    TEST_ASSERT_EQ_INT(180, leds[1]);
    TEST_ASSERT_EQ_INT(0, leds[2]);
    TEST_ASSERT_EQ_INT(0, leds[3]);
}

static void test_mode_cycling_toast_volume_hud_and_battery_side_leds(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    sp1_ui_mapper_set_power_state(&ui, 0.75f, false, false);
    poll_and_update(&ui, &seq);

    sp1_led_frame_t leds;

    /* Boot Stopped state shows 3.0 s Battery Level Toast on Side LEDs */
    sp1_ui_mapper_render_leds(&ui, &seq, 20U, &leds);
    TEST_ASSERT_EQ_INT(255, leds.play[0]);
    TEST_ASSERT_EQ_INT(255, leds.play[1]);
    TEST_ASSERT_EQ_INT(255, leds.play[2]);
    TEST_ASSERT_EQ_INT(0, leds.play[3]);

    /* Advance past 3.0 s battery toast -> settles to dim active Mode 1 glow (48) on Side LED 0 */
    sp1_ui_mapper_render_leds(&ui, &seq, 3000U, &leds);
    sp1_ui_mapper_render_leds(&ui, &seq, 20U, &leds);
    TEST_ASSERT_EQ_INT(SP1_UI_LED_SIDE_REST_MODE, leds.play[0]);
    TEST_ASSERT_EQ_INT(0, leds.play[1]);
    TEST_ASSERT_EQ_INT(0, leds.play[2]);
    TEST_ASSERT_EQ_INT(0, leds.play[3]);

    /* USB plug-in triggers fresh 3.0 s Battery Toast with charging breathing on top LED */
    sp1_ui_mapper_set_power_state(&ui, 0.50f, true, true);
    sp1_ui_mapper_render_leds(&ui, &seq, 100U, &leds);
    TEST_ASSERT_EQ_INT(255, leds.play[0]);
    TEST_ASSERT(leds.play[1] >= 32U && leds.play[1] <= 232U);
    TEST_ASSERT_EQ_INT(0, leds.play[2]);

    /* Short-press Function (< 200 ms) cycles Mode 0 -> 1 -> 2 -> 3 -> 0 */
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PATTERN, sp1_ui_mapper_get_mode(&ui));
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_TRACK, sp1_ui_mapper_get_mode(&ui));

    /* Verify Mode Toast on Side LED 1: 1.2 s solid 255 -> OFF -> Blink 1 -> OFF -> Blink 2 */
    sp1_ui_mapper_render_leds(&ui, &seq, 1200U, &leds); /* t = 0 ms: solid 255 */
    TEST_ASSERT_EQ_INT(255, leds.play[1]);
    sp1_ui_mapper_render_leds(&ui, &seq, 120U, &leds);  /* t = 1200 ms: phase 0 OFF */
    TEST_ASSERT_EQ_INT(0, leds.play[1]);
    sp1_ui_mapper_render_leds(&ui, &seq, 120U, &leds);  /* t = 1320 ms: phase 1 ON (blink 1) */
    TEST_ASSERT_EQ_INT(255, leds.play[1]);
    sp1_ui_mapper_render_leds(&ui, &seq, 120U, &leds);  /* t = 1440 ms: phase 2 OFF */
    TEST_ASSERT_EQ_INT(0, leds.play[1]);
    sp1_ui_mapper_render_leds(&ui, &seq, 120U, &leds);  /* t = 1560 ms: phase 3 ON (blink 2) */
    TEST_ASSERT_EQ_INT(255, leds.play[1]);

    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_SOUND, sp1_ui_mapper_get_mode(&ui));
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PERF, sp1_ui_mapper_get_mode(&ui));
    TEST_ASSERT(sp1_ui_mapper_is_fx_mode(&ui));
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PATTERN, sp1_ui_mapper_get_mode(&ui));
    TEST_ASSERT(!sp1_ui_mapper_is_fx_mode(&ui));

    /* Short-press Function ALSO cycles modes cleanly during active playback */
    sp1_seq_start(&seq);
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_TRACK, sp1_ui_mapper_get_mode(&ui));
    TEST_ASSERT(!sp1_ui_mapper_get_dsp_params(&ui)->siren_gate);

    /* Standalone Vol+ / Vol- works identically in every mode and while holding Function/Tracks,
     * showing an 800 ms Priority-1 vertical HUD on the 4 Side LEDs! */
    TEST_ASSERT_EQ_INT(SP1_UI_VOLUME_DEFAULT, sp1_ui_mapper_get_master_volume(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);
    poll_and_update(&ui, &seq);
    press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(72, sp1_ui_mapper_get_master_volume(&ui));

    /* Priority-1 Volume HUD overrides Mode Toast and Beat Chaser for 800 ms */
    sp1_ui_mapper_render_leds(&ui, &seq, 800U, &leds);
    TEST_ASSERT_EQ_INT(255, leds.play[0]);
    TEST_ASSERT_EQ_INT(255, leds.play[1]);

    /* Clamp Vol+ at 127 and Vol- at 0 */
    for (int i = 0; i < 10; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_UP);
    }
    TEST_ASSERT_EQ_INT(127, sp1_ui_mapper_get_master_volume(&ui));
    for (int i = 0; i < 20; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_VOL_DOWN);
    }
    TEST_ASSERT_EQ_INT(0, sp1_ui_mapper_get_master_volume(&ui));

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();
}

static void test_mode1_pattern_hits_speed_rotation_dilla_steps32_and_swing(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* Center detent helper checks */
    TEST_ASSERT_FLOAT_NEAR(-35.0f, sp1_ui_fader_norm_to_nudge_ms(0.0f), 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_fader_norm_to_nudge_ms(0.42f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, sp1_ui_fader_norm_to_nudge_ms(0.58f), 1e-6f);
    TEST_ASSERT_FLOAT_NEAR(35.0f, sp1_ui_fader_norm_to_nudge_ms(1.0f), 1e-4f);

    /* Unshifted F1 -> Track 0 Euclidean hits (50% of 16 = 8 hits) + BASE2 Track LED meter */
    sp1_hal_controls_on_fader_raw(0U, 2040);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(8, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    sp1_led_frame_t leds;
    sp1_ui_mapper_render_leds(&ui, &seq, 20U, &leds);
    /* 8 in 4-bit binary (MSB..LSB) = 1000 -> [220, 0, 0, 0] */
    TEST_ASSERT_EQ_INT(220, leds.track[0]);
    TEST_ASSERT_EQ_INT(0, leds.track[1]);

    /* Tap T2 without moving faders -> toggles Track 1 mute on release */
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT(sp1_seq_is_track_muted(&seq, 1U));
    press_and_release(&ui, &seq, SP1_BTN_TRACK_2);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));

    /* Mode 1 RWD / FWD -> Global Swing (-2% / +2%, clamped 50%..66%) */
    TEST_ASSERT_FLOAT_NEAR(0.50f, sp1_seq_get_swing(&seq), 1e-5f);
    press_and_release(&ui, &seq, SP1_BTN_FWD);
    TEST_ASSERT_FLOAT_NEAR(0.52f, sp1_seq_get_swing(&seq), 1e-5f);
    for (int i = 0; i < 10; ++i) {
        press_and_release(&ui, &seq, SP1_BTN_FWD);
    }
    TEST_ASSERT_FLOAT_NEAR(0.66f, sp1_seq_get_swing(&seq), 1e-5f);
    press_and_release(&ui, &seq, SP1_BTN_RWD);
    TEST_ASSERT_FLOAT_NEAR(0.64f, sp1_seq_get_swing(&seq), 1e-5f);

    /* Hold T1 + F1..F4 -> Track 0 Speed, Rotation, Dilla Offset, Step Length (up to 32!)
     * without clobbering unshifted F1..F4 hits or muting Track 0 on release! */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);
    poll_and_update(&ui, &seq);

    /* T1 + F1 -> Track 0 Clock Speed = 2x (32nd-note steps) at top of travel */
    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX);
    /* T1 + F4 -> Track 0 Step Length = 32 steps at top of travel */
    sp1_hal_controls_on_fader_raw(3U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(SP1_TRACK_SPEED_2X, sp1_seq_get_track_speed(&seq, 0U));
    TEST_ASSERT_EQ_INT(32, sp1_seq_get_track_pattern(&seq, 0U)->steps);

    /* 32 steps renders as 0000 on mod-16 BASE2_COUNTER */
    sp1_ui_mapper_render_leds(&ui, &seq, 20U, &leds);
    TEST_ASSERT_EQ_INT(0, leds.track[0]);
    TEST_ASSERT_EQ_INT(0, leds.track[3]);

    /* T1 + F2 -> Track 0 Rotation (0..31 when steps=32) */
    sp1_hal_controls_on_fader_raw(1U, SP1_FADER_RAW_MAX);
    /* T1 + F3 -> Track 0 Dilla Offset (+35 ms at top, -35 ms at bottom, 0 ms in detent) */
    sp1_hal_controls_on_fader_raw(2U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(31, sp1_seq_get_track_pattern(&seq, 0U)->rotation);
    TEST_ASSERT_EQ_INT(SP1_SEQ_NUDGE_MAX_FRAMES, sp1_seq_get_track_nudge_frames(&seq, 0U));

    sp1_hal_controls_on_fader_raw(2U, 0);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(-SP1_SEQ_NUDGE_MAX_FRAMES, sp1_seq_get_track_nudge_frames(&seq, 0U));

    sp1_hal_controls_on_fader_raw(2U, 2040); /* Center detent -> 0 ms */
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_track_nudge_frames(&seq, 0U));

    /* Release T1: anti-clobber preserves Track 0 pulses == 8 and does NOT mute Track 0 */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    poll_and_update(&ui, &seq);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 0U));
    TEST_ASSERT_EQ_INT(8, sp1_seq_get_track_pattern(&seq, 0U)->pulses);

    sp1_hal_shutdown();
}

static void test_mode2_track_mixer_tone_pan_send_prob_and_bpm_rocker(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* Advance to Mode 2: Track Mode */
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_TRACK, sp1_ui_mapper_get_mode(&ui));

    /* Unshifted F1 -> Track 0 Mix Level */
    sp1_hal_controls_on_fader_raw(0U, 2040);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(0.50f, sp1_ui_mapper_get_dsp_params(&ui)->track_level[0], 1e-3f);

    /* Mode 2 RWD / FWD -> Fine BPM tap (+/-2 BPM) + Hold auto-repeat (+/-5 BPM every 200 ms) */
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));
    press_and_release(&ui, &seq, SP1_BTN_FWD);
    TEST_ASSERT_EQ_U32(12200U, sp1_seq_get_bpm_x100(&seq));

    /* Hold FWD across 350 ms delay + two 200 ms repeats (+2 on press, +5, +5 = +12 BPM -> 134) */
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update_dt(&ui, &seq, 20U);  /* Press edge: 122 -> 124 BPM */
    TEST_ASSERT_EQ_U32(12400U, sp1_seq_get_bpm_x100(&seq));
    poll_and_update_dt(&ui, &seq, 350U); /* Crosses 350 ms hold threshold (repeat_ms=350 -> +5 BPM) */
    TEST_ASSERT_EQ_U32(12900U, sp1_seq_get_bpm_x100(&seq));
    poll_and_update_dt(&ui, &seq, 50U);  /* repeat_ms = 150 + 50 = 200 -> +5 BPM */
    TEST_ASSERT_EQ_U32(13400U, sp1_seq_get_bpm_x100(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);

    /* Hold T3 + F1..F4 -> Track 2 Tone Filter, Stereo Pan, Dub Delay Send, Hit Probability */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, true);
    poll_and_update(&ui, &seq);

    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX); /* HP Tone */
    sp1_hal_controls_on_fader_raw(1U, SP1_FADER_RAW_MAX); /* Hard Right Pan, then sweep to Hard Left */
    sp1_hal_controls_on_fader_raw(2U, 3060);              /* 0.75 Delay Send */
    sp1_hal_controls_on_fader_raw(3U, 1020);              /* 0.25 Hit Probability */
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_fader_raw(1U, 0);                 /* Hard Left Pan */
    poll_and_update(&ui, &seq);

    const sp1_ui_dsp_params_t *dsp = sp1_ui_mapper_get_dsp_params(&ui);
    TEST_ASSERT_FLOAT_NEAR(1.0f, dsp->track_tone[2], 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.0f, dsp->track_pan[2], 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.75f, dsp->track_delay_send[2], 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(0.25f, sp1_seq_get_track_probability(&seq, 2U), 0.01f);

    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 2U));
    TEST_ASSERT_FLOAT_NEAR(0.50f, dsp->track_level[0], 1e-3f);

    sp1_hal_shutdown();
}

static void test_mode3_sound_edit_macros_live_audition_kits_and_4x4_synth(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* Advance to Mode 3: Sound Edit Mode (2 short Function taps) */
    press_and_release(&ui, &seq, SP1_BTN_FN);
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_SOUND, sp1_ui_mapper_get_mode(&ui));

    /* Tap T1..T4 triggers live audition immediately on press without muting on release */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(110, seq.tracks[1].pending_audition_vel);
    sp1_seq_trigger_event_t trigs[4];
    const uint32_t fired = sp1_seq_process_block(&seq, 0ULL, SP1_AUDIO_BLOCK_FRAMES, trigs, 4U);
    TEST_ASSERT_EQ_U32(1U, fired);
    TEST_ASSERT_EQ_INT(1, trigs[0].track);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 1U));

    /* RWD / FWD cycles through the 4 Synth Kit Presets */
    TEST_ASSERT_EQ_INT(0, sp1_ui_mapper_get_dsp_params(&ui)->active_kit);
    press_and_release(&ui, &seq, SP1_BTN_FWD);
    TEST_ASSERT_EQ_INT(1, sp1_ui_mapper_get_dsp_params(&ui)->active_kit);
    TEST_ASSERT_EQ_INT(1, sp1_ui_mapper_get_dsp_params(&ui)->kit_change_seq);
    press_and_release(&ui, &seq, SP1_BTN_RWD);
    press_and_release(&ui, &seq, SP1_BTN_RWD);
    TEST_ASSERT_EQ_INT(3, sp1_ui_mapper_get_dsp_params(&ui)->active_kit);

    /* Hold T1 + F1..F4 edits Kick Tune, Decay, Punch, and Drive independently */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_fader_raw(0U, 3060); /* Custom Kick Tune = 0.75 */
    sp1_hal_controls_on_fader_raw(2U, 1020); /* Custom Kick Punch = 0.25 */
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    poll_and_update(&ui, &seq);

    TEST_ASSERT_FLOAT_NEAR(0.75f, sp1_ui_mapper_get_dsp_params(&ui)->voice_params[0][0], 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(0.25f, sp1_ui_mapper_get_dsp_params(&ui)->voice_params[0][2], 1e-3f);

    /* Unshifted F1 sweeps Kick Character Macro (Decay + Drive) while preserving custom Tune & Punch! */
    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_FLOAT_NEAR(0.75f, sp1_ui_mapper_get_dsp_params(&ui)->voice_params[0][0], 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(1.00f, sp1_ui_mapper_get_dsp_params(&ui)->kick_decay, 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.25f, sp1_ui_mapper_get_dsp_params(&ui)->voice_params[0][2], 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(0.95f, sp1_ui_mapper_get_dsp_params(&ui)->kick_drive, 1e-4f);

    sp1_hal_shutdown();
}

static void test_mode4_performance_fx_tape_delay_lock_stutter_and_tempo_bend(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* Enter Mode 4: Performance Mode */
    sp1_controls_snapshot_t snap;
    sp1_hal_controls_poll(&snap);
    sp1_ui_mapper_set_mode(&ui, &seq, &snap, SP1_UI_MODE_PERF);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PERF, sp1_ui_mapper_get_mode(&ui));

    /* Move F3 (Tape-Gliding Dub Delay Duration) -> free_dragging = true while moving,
     * then settles to false after 120 ms and locks nearest division (1.0 -> div 5 = 1/4 note)! */
    sp1_hal_controls_on_fader_raw(2U, SP1_FADER_RAW_MAX);
    poll_and_update_dt(&ui, &seq, 20U);
    TEST_ASSERT(sp1_ui_mapper_get_dsp_params(&ui)->dub_delay_free_dragging);
    TEST_ASSERT_EQ_INT(5, sp1_ui_mapper_get_dsp_params(&ui)->dub_delay_div);

    poll_and_update_dt(&ui, &seq, 80U);
    TEST_ASSERT(sp1_ui_mapper_get_dsp_params(&ui)->dub_delay_free_dragging);
    poll_and_update_dt(&ui, &seq, 40U); /* 120 ms elapsed without F3 movement -> locks! */
    TEST_ASSERT(!sp1_ui_mapper_get_dsp_params(&ui)->dub_delay_free_dragging);

    /* Hold T3 (1/32 Stutter) AND sweep F1 (DJ Filter) + F2 (Bitcrusher) + F4 (Dub Wet) simultaneously! */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, true);
    sp1_hal_controls_on_fader_raw(0U, SP1_FADER_RAW_MAX);
    sp1_hal_controls_on_fader_raw(1U, 2040);
    sp1_hal_controls_on_fader_raw(3U, SP1_FADER_RAW_MAX);
    poll_and_update(&ui, &seq);

    const sp1_ui_dsp_params_t *dsp = sp1_ui_mapper_get_dsp_params(&ui);
    TEST_ASSERT_EQ_INT(4, dsp->stutter_div);
    TEST_ASSERT_FLOAT_NEAR(1.0f, dsp->dj_filter_pos, 1e-4f);
    TEST_ASSERT_FLOAT_NEAR(0.5f, dsp->crush_amount, 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(1.0f, dsp->dub_delay_wet, 1e-4f);

    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(0, dsp->stutter_div);
    TEST_ASSERT(!sp1_seq_is_track_muted(&seq, 2U));

    /* Momentary RWD / FWD DJ Tempo Bend (-1 / +1 while held, 0 on release) */
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(-1, sp1_seq_get_tempo_bend(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_tempo_bend(&seq));

    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(1, sp1_seq_get_tempo_bend(&seq));
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(0, sp1_seq_get_tempo_bend(&seq));

    sp1_hal_shutdown();
}

static void test_playback_dub_siren_vs_stopped_function_behavior(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* 1. While STOPPED: holding Function >= 200 ms does NOT activate Dub Siren,
     *    and releasing after >= 200 ms does NOT change the active mode. */
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PATTERN, sp1_ui_mapper_get_mode(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    poll_and_update_dt(&ui, &seq, 20U);
    poll_and_update_dt(&ui, &seq, 250U);
    TEST_ASSERT(!sp1_ui_mapper_get_dsp_params(&ui)->siren_gate);
    sp1_hal_controls_on_button_event(SP1_BTN_FN, false);
    poll_and_update_dt(&ui, &seq, 20U);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PATTERN, sp1_ui_mapper_get_mode(&ui));

    /* 2. While PLAYING: holding Function >= 200 ms gates on the Dub Siren,
     *    Func + F1..F4 modulates Pitch, LFO Rate, LFO Shape, and Siren Delay Send,
     *    and releasing Function closes the gate WITHOUT switching modes! */
    sp1_seq_start(&seq);
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    poll_and_update_dt(&ui, &seq, 20U);
    poll_and_update_dt(&ui, &seq, 200U); /* >= 200 ms -> Siren active! */
    TEST_ASSERT(sp1_ui_mapper_get_dsp_params(&ui)->siren_gate);

    sp1_hal_controls_on_fader_raw(0U, 3060);              /* Pitch = 0.75 */
    sp1_hal_controls_on_fader_raw(1U, 1020);              /* LFO Rate = 0.25 */
    sp1_hal_controls_on_fader_raw(2U, 1020);              /* LFO Shape = 0.25 (wail) */
    sp1_hal_controls_on_fader_raw(3U, SP1_FADER_RAW_MAX); /* Delay Send = 1.0 */
    poll_and_update_dt(&ui, &seq, 20U);

    const sp1_ui_dsp_params_t *dsp = sp1_ui_mapper_get_dsp_params(&ui);
    TEST_ASSERT_FLOAT_NEAR(0.75f, dsp->siren_pitch, 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(0.25f, dsp->siren_lfo_rate, 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(0.25f, dsp->siren_lfo_shape, 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(1.00f, dsp->siren_delay_send, 1e-4f);

    /* Expire fader meter (600 ms) while Siren is still held -> Track LEDs animate LFO wave */
    sp1_led_frame_t leds;
    sp1_ui_mapper_set_siren_visual(&ui, 0.0f);
    sp1_ui_mapper_render_leds(&ui, &seq, 650U, &leds);
    sp1_ui_mapper_render_leds(&ui, &seq, 20U, &leds);
    TEST_ASSERT_EQ_INT(253, leds.track[0]);
    TEST_ASSERT_EQ_INT(16, leds.track[3]);

    /* Release Function: Siren gates off, Mode stays PATTERN, custom Siren settings remembered */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, false);
    poll_and_update_dt(&ui, &seq, 20U);
    TEST_ASSERT(!dsp->siren_gate);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_PATTERN, sp1_ui_mapper_get_mode(&ui));
    TEST_ASSERT_FLOAT_NEAR(0.75f, dsp->siren_pitch, 1e-3f);
    TEST_ASSERT_FLOAT_NEAR(0.25f, dsp->siren_lfo_rate, 1e-3f);

    sp1_seq_stop(&seq);
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

    /* Hold Play (starts transport on press) and press T1..T4 + RWD/FWD/Vol+ + move Faders */
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    poll_and_update(&ui, &seq);

    const sp1_button_id_t tracks[4] = {
        SP1_BTN_TRACK_1, SP1_BTN_TRACK_2, SP1_BTN_TRACK_3, SP1_BTN_TRACK_4
    };
    for (uint8_t t = 0U; t < 4U; ++t) {
        sp1_hal_controls_on_button_event(tracks[t], true);
        sp1_hal_controls_on_fader_raw(t, SP1_FADER_RAW_MAX);
        poll_and_update(&ui, &seq);
        sp1_hal_controls_on_button_event(tracks[t], false);
        poll_and_update(&ui, &seq);
    }

    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    sp1_hal_controls_on_fader_raw(0U, 0);
    poll_and_update(&ui, &seq);

    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);

    /* Confirm ZERO parameters, volume, morph, or mute states were altered while Play was held */
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

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();
}

static void test_play_start_stop_and_ain1_ladder_volume_resilience(void) {
    init_silent_hal();

    sp1_sequencer_t seq;
    sp1_ui_mapper_t ui;
    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    poll_and_update(&ui, &seq);

    /* Switch to Mode 2 (Track Mode) where RWD/FWD steps BPM, so we verify AIN1 Vol->RWD/FWD immunity */
    press_and_release(&ui, &seq, SP1_BTN_FN);
    TEST_ASSERT_EQ_INT(SP1_UI_MODE_TRACK, sp1_ui_mapper_get_mode(&ui));

    TEST_ASSERT(!sp1_seq_is_running(&seq));
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(sp1_seq_is_running(&seq));
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(!sp1_seq_is_running(&seq));

    /* Intra-poll fast Play tap */
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, true);
    sp1_hal_controls_on_button_event(SP1_BTN_PLAY, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT(sp1_seq_is_running(&seq));
    press_and_release(&ui, &seq, SP1_BTN_PLAY);
    TEST_ASSERT(!sp1_seq_is_running(&seq));

    /* Press Vol- (665 mV), then on release AIN1 transits RWD (365 mV) -> BPM must NOT change! */
    sp1_seq_set_bpm_x100(&seq, 12000U);
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui));

    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_DOWN, false);
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(56, sp1_ui_mapper_get_master_volume(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_RWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));

    /* Press Vol+ (1650 mV), then on release AIN1 transits FWD (1110 mV) -> BPM must NOT change! */
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, true);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_INT(64, sp1_ui_mapper_get_master_volume(&ui));
    sp1_hal_controls_on_button_event(SP1_BTN_VOL_UP, false);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, true);
    poll_and_update(&ui, &seq);
    sp1_hal_controls_on_button_event(SP1_BTN_FWD, false);
    poll_and_update(&ui, &seq);
    TEST_ASSERT_EQ_U32(12000U, sp1_seq_get_bpm_x100(&seq));

    sp1_hal_shutdown();
}

static void test_function_power_hold_release_bounce_debounce(void) {
    sp1_ui_power_hold_t hold;

    sp1_ui_power_hold_init(&hold, true);
    for (uint32_t t = 0U; t < 200U; ++t) {
        TEST_ASSERT(!sp1_ui_power_hold_update(&hold,
                                              true,
                                              20U,
                                              SP1_UI_POWER_HOLD_SHUTDOWN_MS,
                                              SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    }
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(hold.wait_for_release);
    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, false, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT(!hold.wait_for_release);

    TEST_ASSERT(!sp1_ui_power_hold_update(
        &hold, true, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    for (uint32_t tick = 1U; tick <= 149U; ++tick) {
        const bool bouncy_pressed = ((tick % 30U) != 0U);
        TEST_ASSERT(!sp1_ui_power_hold_update(&hold,
                                              bouncy_pressed,
                                              20U,
                                              SP1_UI_POWER_HOLD_SHUTDOWN_MS,
                                              SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    }
    TEST_ASSERT(sp1_ui_power_hold_update(
        &hold, true, 20U, SP1_UI_POWER_HOLD_SHUTDOWN_MS, SP1_UI_POWER_RELEASE_DEBOUNCE_MS));
    TEST_ASSERT_EQ_U32(3000U, hold.hold_elapsed_ms);
}

int main(void) {
    RUN_TEST(test_four_led_widget_primitives);
    RUN_TEST(test_mode_cycling_toast_volume_hud_and_battery_side_leds);
    RUN_TEST(test_mode1_pattern_hits_speed_rotation_dilla_steps32_and_swing);
    RUN_TEST(test_mode2_track_mixer_tone_pan_send_prob_and_bpm_rocker);
    RUN_TEST(test_mode3_sound_edit_macros_live_audition_kits_and_4x4_synth);
    RUN_TEST(test_mode4_performance_fx_tape_delay_lock_stutter_and_tempo_bend);
    RUN_TEST(test_playback_dub_siren_vs_stopped_function_behavior);
    RUN_TEST(test_play_shift_chord_hardware_safety);
    RUN_TEST(test_play_start_stop_and_ain1_ladder_volume_resilience);
    RUN_TEST(test_function_power_hold_release_bounce_debounce);
    return sp1_test_summary();
}
