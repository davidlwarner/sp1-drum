#ifndef SP1_HAL_CONTROLS_H
#define SP1_HAL_CONTROLS_H

/**
 * @file hal_controls.h
 * @brief Pure C11 Hardware Abstraction Layer for SP-1 faders, buttons, and 8x PWM LEDs.
 *
 * Verified SP-1 hardware constraints (see .gemini/skills/sp1-hardware-map/SKILL.md):
 *  - 4x analog faders on nRF52840 12-bit SAADC: AIN3 (F1), AIN6 (F2), AIN2 (F3), AIN7 (F4)
 *    with clamped range [0, 0x0FF0] (4080) and 8-LSB deadband.
 *  - Track 1..4 + Play share the AIN0 resistor ladder; Play (1587-1592 mV) dominates the
 *    ladder voltage and CANNOT be decoded in chords with T1..T4. Shift modifiers for T1..T4
 *    MUST use Function (GPIO P0.27) or Transport keys (AIN1).
 *  - 8x monochrome PWM LEDs (PWM2 Track 1..4, PWM3 Play 1..4), 0..255 brightness.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_NUM_FADERS          4U
#define SP1_FADER_RAW_MIN       0
#define SP1_FADER_RAW_MAX       0x0FF0  /* 4080 (12-bit SAADC effective ceiling) */
#define SP1_FADER_DEADBAND      8       /* ~0.2% of 12-bit range */

#define SP1_NUM_TRACK_LEDS      4U
#define SP1_NUM_PLAY_LEDS       4U
#define SP1_NUM_LEDS            (SP1_NUM_TRACK_LEDS + SP1_NUM_PLAY_LEDS)

typedef enum {
    SP1_BTN_TRACK_1 = 0,   /* AIN0 resistor ladder */
    SP1_BTN_TRACK_2 = 1,   /* AIN0 resistor ladder */
    SP1_BTN_TRACK_3 = 2,   /* AIN0 resistor ladder */
    SP1_BTN_TRACK_4 = 3,   /* AIN0 resistor ladder */
    SP1_BTN_PLAY    = 4,   /* AIN0 resistor ladder (dominates ladder; NEVER a T1..T4 shift!) */
    SP1_BTN_RWD     = 5,   /* AIN1 resistor ladder (valid shift modifier) */
    SP1_BTN_VOL_DOWN = 6,  /* AIN1 resistor ladder (valid shift modifier) */
    SP1_BTN_FWD     = 7,   /* AIN1 resistor ladder (valid shift modifier) */
    SP1_BTN_VOL_UP  = 8,   /* AIN1 resistor ladder (valid shift modifier) */
    SP1_BTN_FN      = 9,   /* GPIO P0.27 active-low (primary shift modifier; 3 s hold = poweroff) */
    SP1_BTN_COUNT   = 10
} sp1_button_id_t;

#define SP1_BTN_BIT(id) ((uint16_t)(1U << (unsigned)(id)))

#define SP1_BTN_TRACK_MASK                                              \
    ((uint16_t)(SP1_BTN_BIT(SP1_BTN_TRACK_1) |                          \
                SP1_BTN_BIT(SP1_BTN_TRACK_2) |                          \
                SP1_BTN_BIT(SP1_BTN_TRACK_3) |                          \
                SP1_BTN_BIT(SP1_BTN_TRACK_4)))

#define SP1_BTN_TRANSPORT_MASK                                          \
    ((uint16_t)(SP1_BTN_BIT(SP1_BTN_RWD) |                              \
                SP1_BTN_BIT(SP1_BTN_VOL_DOWN) |                         \
                SP1_BTN_BIT(SP1_BTN_FWD) |                              \
                SP1_BTN_BIT(SP1_BTN_VOL_UP)))

/* Valid shift modifiers for Track 1..4 chords: Function (P0.27) or Transport (AIN1).
 * SP1_BTN_PLAY is explicitly excluded due to AIN0 resistor-ladder dominance. */
#define SP1_BTN_VALID_SHIFT_MASK                                        \
    ((uint16_t)(SP1_BTN_BIT(SP1_BTN_FN) | SP1_BTN_TRANSPORT_MASK))

/* Per-fader deadband filter state. Static RAM: 4 bytes per fader (16 B for 4 faders). */
typedef struct {
    int16_t raw_clamped;
    uint8_t cc7;
    bool initialized;
} sp1_fader_filter_t;

/* Atomic-published control snapshot passed from control thread (~50 Hz) to RT core.
 * Static RAM: 40 bytes per snapshot. */
typedef struct {
    uint16_t fader_raw[SP1_NUM_FADERS];
    float fader_norm[SP1_NUM_FADERS];
    uint8_t fader_cc[SP1_NUM_FADERS];
    uint16_t buttons_down;
    uint16_t buttons_pressed;
    uint16_t buttons_released;
    uint32_t seq;
} sp1_controls_snapshot_t;

/* Lock-free double-buffered snapshot bus (single writer, single reader).
 * Uses plain uint32_t index in header for C++17 compatibility; atomic acquire/release
 * barriers are enforced in sp1_hal_common.c. Static RAM: 84 bytes per bus. */
typedef struct {
    sp1_controls_snapshot_t buffers[2];
    uint32_t active_idx;
} sp1_controls_bus_t;

/* 8-channel PWM LED frame (0..255 per LED). Static RAM: 8 bytes. */
typedef struct {
    uint8_t track[SP1_NUM_TRACK_LEDS];
    uint8_t play[SP1_NUM_PLAY_LEDS];
} sp1_led_frame_t;

/* ── Portable C11 Control Helpers (sp1_hal_common.c) ─────────────────────── */

void sp1_fader_filter_init(sp1_fader_filter_t *filter);

/**
 * Clamp raw 12-bit SAADC sample to [0, SP1_FADER_RAW_MAX], apply SP1_FADER_DEADBAND,
 * and compute normalized [0.0f, 1.0f] and 7-bit CC [0, 127].
 * Returns true if the filtered value changed (or on first initialization).
 */
bool sp1_fader_filter_update(sp1_fader_filter_t *filter,
                             int16_t raw_sample,
                             float *out_norm,
                             uint8_t *out_cc7);

/**
 * Hardware-safe chord check: returns true iff shift_btn is a valid SP-1 shift modifier
 * (Function P0.27 or Transport AIN1 — NEVER Play on AIN0), track_btn is in T1..T4,
 * and both bits are currently asserted in buttons_down.
 */
bool sp1_controls_is_valid_shift_chord(uint16_t buttons_down,
                                       sp1_button_id_t shift_btn,
                                       sp1_button_id_t track_btn);

void sp1_controls_snapshot_init(sp1_controls_snapshot_t *snap);
void sp1_controls_bus_init(sp1_controls_bus_t *bus);
void sp1_controls_bus_publish(sp1_controls_bus_t *bus, const sp1_controls_snapshot_t *snap);
void sp1_controls_bus_read(const sp1_controls_bus_t *bus, sp1_controls_snapshot_t *out_snap);

/* ── Platform HAL Controls & LEDs (sp1_hal_zephyr.c / sp1_hal_host.c) ────── */

int sp1_hal_controls_init(void);

/**
 * Consume the latest control snapshot (and clear latched pressed/released edge bits).
 */
void sp1_hal_controls_poll(sp1_controls_snapshot_t *out_snapshot);

/**
 * Thin delegation hooks called by app/main.cpp (Zephyr) or synthetic test scripts (Host).
 */
void sp1_hal_controls_on_button_event(sp1_button_id_t btn, bool pressed);
void sp1_hal_controls_on_fader_raw(uint8_t fader_idx, int16_t raw_sample);

void sp1_hal_leds_set_frame(const sp1_led_frame_t *frame);
void sp1_hal_leds_all_off(void);

#ifdef __cplusplus
}
#endif

#endif /* SP1_HAL_CONTROLS_H */
