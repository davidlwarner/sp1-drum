#ifndef SP1_CORE_UI_MAPPER_H
#define SP1_CORE_UI_MAPPER_H

/**
 * @file ui_mapper.h
 * @brief Pure C11 Screenless Control Gesture Mapper & 8-Channel PWM LED Renderer.
 *
 * Verified SP-1 hardware constraints:
 *  - Only Function (GPIO P0.27) and Transport keys (AIN1: RWD/FWD/Vol-/Vol+) are valid
 *    shift modifiers. Play (AIN0) dominates the Track 1..4 resistor ladder and is NEVER
 *    accepted as a shift modifier.
 *  - Mode-switch fader latching prevents releasing Function/RWD/FWD from clobbering
 *    Normal Mode Euclidean pulse densities.
 *  - Zero Zephyr headers, zero dynamic allocation, zero runtime division.
 */

#include <stdbool.h>
#include <stdint.h>

#include "core/sequencer.h"
#include "hal/hal_controls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_UI_BPM_MIN_X100             6000U   /*  60.00 BPM UI floor */
#define SP1_UI_BPM_MAX_X100             18000U  /* 180.00 BPM UI ceiling */
#define SP1_UI_BPM_FINE_STEP_X100       200U    /* +/-  2 BPM (RWD / FWD unshifted tap) */
#define SP1_UI_BPM_COARSE_STEP_X100     1000U   /* +/- 10 BPM (Function + RWD / FWD) */
#define SP1_UI_SWING_STEP               0.02f   /* +/-  2% Global Swing (Function + Vol- / Vol+) */
#define SP1_UI_VOLUME_MIN               0U      /* Mute / min Master Output Volume */
#define SP1_UI_VOLUME_MAX               127U    /* Full-scale Master Output Volume */
#define SP1_UI_VOLUME_DEFAULT           108U    /* Boot default Master Output Volume */
#define SP1_UI_VOLUME_STEP              8U      /* +/-  8 volume step (Vol- / Vol+ tap) */

/* Generous 16% center detent [0.42..0.58] snapping Function + Fader to exact 0 ms grid lock. */
#define SP1_UI_NUDGE_DEADZONE_LOW       0.42f
#define SP1_UI_NUDGE_DEADZONE_HIGH      0.58f

/* Fader engagement delta after mode transitions (2x hardware 8-LSB SAADC deadband). */
#define SP1_UI_FADER_ENGAGE_DELTA       16

/* LED brightness constants (0..255 Q8 PWM targets) */
#define SP1_UI_LED_TRACK_MUTE_DIM       8U
#define SP1_UI_LED_TRACK_ACTIVE_IDLE    32U
#define SP1_UI_LED_TRACK_HIT_NORMAL     200U
#define SP1_UI_LED_TRACK_HIT_ACCENT     255U
#define SP1_UI_LED_SHIFT_FOCUS          255U
#define SP1_UI_LED_SHIFT_OTHER          40U
#define SP1_UI_LED_NUDGE_CENTER         160U

typedef enum {
    SP1_UI_MOD_NONE = 0,
    SP1_UI_MOD_FN   = 1,    /* Function (GPIO P0.27): Dilla nudge, coarse BPM, swing, nudge reset */
    SP1_UI_MOD_RWD  = 2,    /* RWD (AIN1): pattern rotation, rotation reset, or unshifted -2 BPM tap */
    SP1_UI_MOD_FWD  = 3     /* FWD (AIN1): polymeter step length (1..32), reset, or unshifted +2 BPM tap */
} sp1_ui_modifier_t;

/* Normalized Master Performance FX, Master Volume & 4-Voice Timbre/Morph target snapshot.
 * Static RAM: 44 bytes. */
typedef struct {
    float dj_filter_pos;        /* [0.0f..1.0f], 0.50f = center bypass deadzone */
    float dj_resonance_q;       /* [0.707f..12.0f], default 1.85f */
    float crush_amount;         /* [0.0f..1.0f], 0.0f = clean 48 kHz 16-bit */
    float dub_delay_wet;        /* [0.0f..1.0f], default 0.22f */
    float dub_delay_feedback;   /* [0.0f..0.85f], default 0.45f */
    float kick_decay;           /* [0.0f..1.0f], default 0.45f */
    float kick_drive;           /* [0.0f..1.0f], default 0.35f */
    float snare_morph;          /* [0.0f = Rimshot .. 1.0f = Snare], default 0.65f */
    float hat_decay;            /* [0.0f = Closed .. 1.0f = Open], default 0.20f */
    float perc_timbre;          /* [0.0f..1.0f], default 0.45f */
    uint8_t stutter_div;        /* Effective stutter division (0=OFF, 1=1/4 .. 5=1/64) */
    uint8_t master_volume;      /* Master output volume [0..127], default 108 */
    uint8_t pad[2];
} sp1_ui_dsp_params_t;

/* Screenless UI gesture & LED state.
 * Static RAM: 84 bytes (zero dynamic allocation). */
typedef struct {
    sp1_ui_modifier_t active_mod;
    uint8_t focused_track;                      /* Last adjusted track for shift LED readout [0..3] */
    uint8_t track_flash[SP1_SEQ_NUM_TRACKS];    /* Per-track decaying hit flash [0..255] */
    uint16_t mode_entry_raw[SP1_NUM_FADERS];    /* Latched raw fader values on mode switch / reset */
    uint16_t last_applied_raw[SP1_NUM_FADERS];  /* Last raw fader value applied to sequencer */
    bool fader_engaged[SP1_NUM_FADERS];         /* Anti-clobber engage flags per fader */
    bool initialized;
    bool fx_mode_active;                        /* Latched Performance FX Mode page */
    uint8_t fader_stutter_div;                  /* Fader-2 latched stutter division [0..5] */
    uint8_t reserved;
    uint16_t btn_consumed_mask;                 /* Buttons consumed by chord/fader (suppresses release tap) */
    sp1_ui_dsp_params_t dsp;
} sp1_ui_mapper_t;

/**
 * Initialize UI gesture mapper and LED envelope state.
 */
void sp1_ui_mapper_init(sp1_ui_mapper_t *ui);

/**
 * Map a normalized fader position [0.0f..1.0f] to Dilla micro-timing nudge [-35..+35 ms]
 * with a generous center deadzone [0.42..0.58] snapping to exact 0.0f ms.
 */
float sp1_ui_fader_norm_to_nudge_ms(float norm);

/**
 * Process one control snapshot (buttons + faders) and apply hardware-safe gestures
 * to the sequencer and Performance FX / Voice Timbre targets.
 * Never accepts SP1_BTN_PLAY as a shift modifier.
 */
void sp1_ui_mapper_update(sp1_ui_mapper_t *ui,
                          sp1_sequencer_t *seq,
                          const sp1_controls_snapshot_t *snap);

/**
 * Explicitly toggle or set latched Performance FX Mode and re-latch faders.
 */
void sp1_ui_mapper_set_fx_mode(sp1_ui_mapper_t *ui,
                               const sp1_controls_snapshot_t *snap,
                               bool enabled);
bool sp1_ui_mapper_is_fx_mode(const sp1_ui_mapper_t *ui);
const sp1_ui_dsp_params_t *sp1_ui_mapper_get_dsp_params(const sp1_ui_mapper_t *ui);
uint8_t sp1_ui_mapper_get_master_volume(const sp1_ui_mapper_t *ui);

/**
 * Notify LED state machine of newly fired sequencer triggers so Track 1..4 LEDs pulse.
 */
void sp1_ui_mapper_on_triggers(sp1_ui_mapper_t *ui,
                               const sp1_seq_trigger_event_t *triggers,
                               uint32_t count);

/**
 * Render the 8-channel PWM LED frame (Track 1..4 playhead/hit pulse + mute dim;
 * Play 1..4 beat clock / external sync floor / Dilla nudge polarity meter) and advance
 * LED flash decay by dt_ms milliseconds.
 */
void sp1_ui_mapper_render_leds(sp1_ui_mapper_t *ui,
                               const sp1_sequencer_t *seq,
                               uint32_t dt_ms,
                               sp1_led_frame_t *out_frame);

sp1_ui_modifier_t sp1_ui_mapper_active_modifier(const sp1_ui_mapper_t *ui);
uint8_t sp1_ui_mapper_focused_track(const sp1_ui_mapper_t *ui);

#ifdef __cplusplus
}
#endif

#endif /* SP1_CORE_UI_MAPPER_H */
