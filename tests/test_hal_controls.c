#include "test_harness.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <stdint.h>

static void test_fader_clamping_deadband_and_scaling(void) {
    sp1_fader_filter_t f;
    sp1_fader_filter_init(&f);

    float norm = -1.0f;
    uint8_t cc = 99U;

    /* Negative sample clamps to 0. */
    TEST_ASSERT(sp1_fader_filter_update(&f, -15, &norm, &cc));
    TEST_ASSERT_EQ_INT(0, f.raw_clamped);
    TEST_ASSERT_EQ_INT(0, cc);
    TEST_ASSERT_FLOAT_NEAR(0.0f, norm, 1e-5f);

    /* Change smaller than SP1_FADER_DEADBAND (8) is suppressed. */
    TEST_ASSERT(!sp1_fader_filter_update(&f, 7, &norm, &cc));
    TEST_ASSERT_EQ_INT(0, f.raw_clamped);

    /* Change >= deadband updates state. */
    TEST_ASSERT(sp1_fader_filter_update(&f, 2040, &norm, &cc));
    TEST_ASSERT_EQ_INT(2040, f.raw_clamped);
    TEST_ASSERT_FLOAT_NEAR(0.5f, norm, 1e-4f);
    TEST_ASSERT_EQ_INT(63, cc);

    /* Sample above SP1_FADER_RAW_MAX (4080 / 0x0FF0) clamps cleanly. */
    TEST_ASSERT(sp1_fader_filter_update(&f, 4095, &norm, &cc));
    TEST_ASSERT_EQ_INT(SP1_FADER_RAW_MAX, f.raw_clamped);
    TEST_ASSERT_FLOAT_NEAR(1.0f, norm, 1e-5f);
    TEST_ASSERT_EQ_INT(127, cc);
}

static void test_hardware_shift_chord_constraints(void) {
    /* Valid shift modifiers: Function (P0.27) and Transport keys (AIN1). */
    const uint16_t fn_and_t1 = (uint16_t)(SP1_BTN_BIT(SP1_BTN_FN) | SP1_BTN_BIT(SP1_BTN_TRACK_1));
    TEST_ASSERT(sp1_controls_is_valid_shift_chord(fn_and_t1, SP1_BTN_FN, SP1_BTN_TRACK_1));
    TEST_ASSERT(!sp1_controls_is_valid_shift_chord(fn_and_t1, SP1_BTN_FN, SP1_BTN_TRACK_2));

    const uint16_t rwd_and_t4 =
        (uint16_t)(SP1_BTN_BIT(SP1_BTN_RWD) | SP1_BTN_BIT(SP1_BTN_TRACK_4));
    TEST_ASSERT(sp1_controls_is_valid_shift_chord(rwd_and_t4, SP1_BTN_RWD, SP1_BTN_TRACK_4));

    const uint16_t fwd_and_t3 =
        (uint16_t)(SP1_BTN_BIT(SP1_BTN_FWD) | SP1_BTN_BIT(SP1_BTN_TRACK_3));
    TEST_ASSERT(sp1_controls_is_valid_shift_chord(fwd_and_t3, SP1_BTN_FWD, SP1_BTN_TRACK_3));

    const uint16_t volup_and_t2 =
        (uint16_t)(SP1_BTN_BIT(SP1_BTN_VOL_UP) | SP1_BTN_BIT(SP1_BTN_TRACK_2));
    TEST_ASSERT(sp1_controls_is_valid_shift_chord(volup_and_t2, SP1_BTN_VOL_UP, SP1_BTN_TRACK_2));

    /* CRITICAL SP-1 HARDWARE CONSTRAINT: Play (AIN0) MUST NEVER be accepted as a T1..T4 shift. */
    const uint16_t play_and_all_tracks =
        (uint16_t)(SP1_BTN_BIT(SP1_BTN_PLAY) | SP1_BTN_TRACK_MASK);
    TEST_ASSERT(!sp1_controls_is_valid_shift_chord(
        play_and_all_tracks, SP1_BTN_PLAY, SP1_BTN_TRACK_1));
    TEST_ASSERT(!sp1_controls_is_valid_shift_chord(
        play_and_all_tracks, SP1_BTN_PLAY, SP1_BTN_TRACK_2));
    TEST_ASSERT(!sp1_controls_is_valid_shift_chord(
        play_and_all_tracks, SP1_BTN_PLAY, SP1_BTN_TRACK_3));
    TEST_ASSERT(!sp1_controls_is_valid_shift_chord(
        play_and_all_tracks, SP1_BTN_PLAY, SP1_BTN_TRACK_4));

    /* Track buttons cannot be used as shift modifiers for other track buttons. */
    TEST_ASSERT(!sp1_controls_is_valid_shift_chord(
        SP1_BTN_TRACK_MASK, SP1_BTN_TRACK_1, SP1_BTN_TRACK_2));
}

static void test_host_controls_edges_faders_and_leds(void) {
    const sp1_hal_host_config_t cfg = {
        .wav_path = NULL,
        .midi_csv_path = NULL,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());

    /* Inject fader and button events through the same delegation hooks used by app/main.cpp. */
    sp1_hal_controls_on_fader_raw(0U, 4080);
    sp1_hal_controls_on_fader_raw(3U, 2040);
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, true);

    sp1_controls_snapshot_t snap;
    sp1_hal_controls_poll(&snap);

    TEST_ASSERT_EQ_INT(127, snap.fader_cc[0]);
    TEST_ASSERT_FLOAT_NEAR(1.0f, snap.fader_norm[0], 1e-5f);
    TEST_ASSERT_EQ_INT(63, snap.fader_cc[3]);
    TEST_ASSERT_FLOAT_NEAR(0.5f, snap.fader_norm[3], 1e-4f);

    TEST_ASSERT((snap.buttons_down & SP1_BTN_BIT(SP1_BTN_FN)) != 0U);
    TEST_ASSERT((snap.buttons_pressed & SP1_BTN_BIT(SP1_BTN_TRACK_2)) != 0U);
    TEST_ASSERT(sp1_controls_is_valid_shift_chord(
        snap.buttons_down, SP1_BTN_FN, SP1_BTN_TRACK_2));

    /* Second poll clears one-shot pressed edge bits while keeping buttons_down set. */
    sp1_hal_controls_poll(&snap);
    TEST_ASSERT_EQ_U32(0U, snap.buttons_pressed);
    TEST_ASSERT((snap.buttons_down & SP1_BTN_BIT(SP1_BTN_TRACK_2)) != 0U);

    /* Release Track 2 and verify falling-edge latch. */
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_2, false);
    sp1_hal_controls_poll(&snap);
    TEST_ASSERT((snap.buttons_down & SP1_BTN_BIT(SP1_BTN_TRACK_2)) == 0U);
    TEST_ASSERT((snap.buttons_released & SP1_BTN_BIT(SP1_BTN_TRACK_2)) != 0U);

    /* Verify 8-channel PWM LED frame set and all_off(). */
    const sp1_led_frame_t frame = {
        .track = { 255U, 128U, 64U, 16U },
        .play = { 10U, 20U, 30U, 40U },
    };
    sp1_hal_leds_set_frame(&frame);

    sp1_led_frame_t readback;
    sp1_hal_host_get_leds(&readback);
    TEST_ASSERT_EQ_INT(255, readback.track[0]);
    TEST_ASSERT_EQ_INT(16, readback.track[3]);
    TEST_ASSERT_EQ_INT(10, readback.play[0]);
    TEST_ASSERT_EQ_INT(40, readback.play[3]);

    sp1_hal_leds_all_off();
    sp1_hal_host_get_leds(&readback);
    for (uint32_t i = 0U; i < SP1_NUM_TRACK_LEDS; ++i) {
        TEST_ASSERT_EQ_INT(0, readback.track[i]);
        TEST_ASSERT_EQ_INT(0, readback.play[i]);
    }

    sp1_hal_shutdown();
}

static void test_single_writer_poll_isolation(void) {
    const sp1_hal_host_config_t cfg = {
        .wav_path = NULL,
        .midi_csv_path = NULL,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());

    /* Producer writes 2 button events (seq = 2). */
    sp1_hal_controls_on_button_event(SP1_BTN_FN, true);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, true);

    sp1_controls_snapshot_t snap;
    sp1_hal_controls_poll(&snap);
    TEST_ASSERT_EQ_U32(2U, snap.seq);
    TEST_ASSERT_EQ_U32((uint32_t)(SP1_BTN_BIT(SP1_BTN_FN) | SP1_BTN_BIT(SP1_BTN_TRACK_1)),
                       snap.buttons_pressed);
    TEST_ASSERT_EQ_U32(0U, snap.buttons_released);

    /* Repeated consumer polls report zero edges and never mutate producer bus or seq. */
    for (uint32_t i = 0U; i < 4U; ++i) {
        sp1_hal_controls_poll(&snap);
        TEST_ASSERT_EQ_U32(2U, snap.seq);
        TEST_ASSERT_EQ_U32(0U, snap.buttons_pressed);
        TEST_ASSERT_EQ_U32(0U, snap.buttons_released);
        TEST_ASSERT_EQ_U32((uint32_t)(SP1_BTN_BIT(SP1_BTN_FN) | SP1_BTN_BIT(SP1_BTN_TRACK_1)),
                           snap.buttons_down);
    }

    /* Concurrent fader + button updates between polls preserve fader data and report exact edges. */
    sp1_hal_controls_on_fader_raw(1U, 3060);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_1, false);
    sp1_hal_controls_on_button_event(SP1_BTN_TRACK_3, true);

    sp1_hal_controls_poll(&snap);
    TEST_ASSERT_EQ_U32(5U, snap.seq);
    TEST_ASSERT_EQ_INT(3060, snap.fader_raw[1]);
    TEST_ASSERT_EQ_U32(SP1_BTN_BIT(SP1_BTN_TRACK_3), snap.buttons_pressed);
    TEST_ASSERT_EQ_U32(SP1_BTN_BIT(SP1_BTN_TRACK_1), snap.buttons_released);
    TEST_ASSERT_EQ_U32((uint32_t)(SP1_BTN_BIT(SP1_BTN_FN) | SP1_BTN_BIT(SP1_BTN_TRACK_3)),
                       snap.buttons_down);

    /* Subsequent poll clears both pressed and released edges while preserving fader & down state. */
    sp1_hal_controls_poll(&snap);
    TEST_ASSERT_EQ_U32(5U, snap.seq);
    TEST_ASSERT_EQ_INT(3060, snap.fader_raw[1]);
    TEST_ASSERT_EQ_U32(0U, snap.buttons_pressed);
    TEST_ASSERT_EQ_U32(0U, snap.buttons_released);

    sp1_hal_shutdown();
}

int main(void) {
    RUN_TEST(test_fader_clamping_deadband_and_scaling);
    RUN_TEST(test_hardware_shift_chord_constraints);
    RUN_TEST(test_host_controls_edges_faders_and_leds);
    RUN_TEST(test_single_writer_poll_isolation);
    return sp1_test_summary();
}
