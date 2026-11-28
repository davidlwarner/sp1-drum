#ifndef SP1_CORE_UI_MAPPER_H
#define SP1_CORE_UI_MAPPER_H

/**
 * @file ui_mapper.h
 * @brief Pure C11 4-Mode Screenless Gesture Mapper & Dual 4-LED Priority Renderer.
 *
 * Verified SP-1 hardware & design rules:
 *  - 4 Modes cycled via short-press (< 200 ms) of Function (GPIO P0.27):
 *      0: Pattern Mode     (Euclidean rhythm, per-track speed, rotation, Dilla offset, length)
 *      1: Track Mode       (4-ch mixer level, per-track tone filter, pan, delay send, probability)
 *      2: Sound Edit Mode  (Signature macros, live finger-drum audition, 4 kits, 4x4 voice synth)
 *      3: Performance Mode (DJ SVF, Bitcrusher, Tape Pitch-Glide Dub Delay, Momentary Stutters)
 *  - Volume buttons (Vol- / Vol+) are 100% standalone Master Volume controls with a vertical
 *    800 ms Side LED HUD and zero combinations.
 *  - During playback, holding Function (>= 200 ms or + F1–F4) gates on the live Dub Siren
 *    instead of powering off the unit.
 *  - Play (AIN0) is never accepted in button/fader chords due to resistor-ladder physics.
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
#define SP1_UI_BPM_CENTER_X100          12000U  /* 120.00 BPM center detent */
#define SP1_UI_BPM_FINE_STEP_X100       200U    /* +/-  2 BPM (Mode 2 RWD / FWD tap) */
#define SP1_UI_BPM_HOLD_STEP_X100       500U    /* +/-  5 BPM (Mode 2 RWD / FWD hold repeat) */
#define SP1_UI_ROCKER_HOLD_DELAY_MS     350U    /* Hold delay before BPM auto-repeat starts */
#define SP1_UI_ROCKER_REPEAT_MS         200U    /* Auto-repeat interval while holding RWD/FWD */

#define SP1_UI_SWING_STEP               0.02f   /* +/-  2% Global Swing (Mode 1 RWD / FWD) */
#define SP1_UI_VOLUME_MIN               0U
#define SP1_UI_VOLUME_MAX               127U
#define SP1_UI_VOLUME_DEFAULT           64U
#define SP1_UI_VOLUME_STEP              8U

/* Generous center detents snapping bipolar faders cleanly to exact neutral */
#define SP1_UI_NUDGE_DEADZONE_LOW       0.42f
#define SP1_UI_NUDGE_DEADZONE_HIGH      0.58f
#define SP1_UI_BIPOLAR_DEADZONE_LOW     0.45f
#define SP1_UI_BIPOLAR_DEADZONE_HIGH    0.55f

/* Fader engagement delta after mode/track transitions (2x hardware 8-LSB SAADC deadband) */
#define SP1_UI_FADER_ENGAGE_DELTA       16

/* Gesture & LED display timing constants (milliseconds) */
#define SP1_UI_FN_SIREN_HOLD_MS         200U    /* >= 200 ms Function hold while playing = Siren */
#define SP1_UI_DELAY_DRAG_SETTLE_MS     120U    /* Settle window before Tape Delay locks division */
#define SP1_UI_VOLUME_HUD_MS            800U    /* Side LED vertical Volume bar duration */
#define SP1_UI_MODE_TOAST_SOLID_MS      1200U   /* Solid Side LED duration on mode switch */
#define SP1_UI_MODE_TOAST_BLINK_MS      120U    /* Half-period of the 2x mode-toast blinks */
#define SP1_UI_MODE_TOAST_TOTAL_MS      (SP1_UI_MODE_TOAST_SOLID_MS + (4U * SP1_UI_MODE_TOAST_BLINK_MS))
#define SP1_UI_BATTERY_TOAST_MS         3000U   /* Stopped Side LED battery bar duration */
#define SP1_UI_PARAM_METER_FADER_MS     600U    /* Track LED fader meter hold-off */
#define SP1_UI_PARAM_METER_ROCKER_MS    900U    /* Track LED rocker meter hold-off */

/* LED brightness constants (0..255 Q8 PWM targets) */
#define SP1_UI_LED_TRACK_MUTE_DIM       8U
#define SP1_UI_LED_TRACK_ACTIVE_IDLE    32U
#define SP1_UI_LED_TRACK_HIT_NORMAL     200U
#define SP1_UI_LED_TRACK_HIT_ACCENT     255U
#define SP1_UI_LED_SIDE_REST_MODE       48U
#define SP1_UI_LED_NUDGE_CENTER         160U

#define SP1_UI_NUM_VOICE_PARAMS         4U
#define SP1_UI_NUM_KITS                 4U

typedef enum {
    SP1_UI_MODE_PATTERN = 0,    /* Side LED 1: Euclidean Rhythm, Speed, Rotation, Dilla, Length */
    SP1_UI_MODE_TRACK   = 1,    /* Side LED 2: 4-Ch Mixer, Tone Filter, Pan, Delay Send, Prob */
    SP1_UI_MODE_SOUND   = 2,    /* Side LED 3: Macros, Finger-Drum Audition, Kits, 4x4 Synth */
    SP1_UI_MODE_PERF    = 3,    /* Side LED 4: DJ SVF, Bitcrusher, Tape Delay, Stutter, Bend */
    SP1_UI_MODE_COUNT   = 4
} sp1_ui_mode_t;

typedef enum {
    SP1_LED_METER_NONE            = 0,
    SP1_LED_METER_UNBALANCED      = 1,  /* Smooth progressive fill LED 1 -> 4 */
    SP1_LED_METER_MIDDLE_BALANCED = 2,  /* Center LED 2+3 lit; fades left (2->1) or right (3->4) */
    SP1_LED_METER_BASE2           = 3,  /* Mod-16 4-bit binary (MSB..LSB): 0/16/32 = 0000 */
    SP1_LED_METER_DISCRETE        = 4   /* Stepped 1-of-4 or 6-division slot indicator */
} sp1_led_meter_style_t;

/* Complete DSP target parameter snapshot driven by UI Mapper.
 * Static RAM: 172 bytes. */
typedef struct {
    /* Master Performance FX & Volume */
    float dj_filter_pos;            /* [0.0f..1.0f], 0.50f = center bypass */
    float dj_resonance_q;           /* [0.707f..12.0f], default 1.85f */
    float crush_amount;             /* [0.0f..1.0f], 0.0f = clean 48 kHz */
    float dub_delay_time;           /* [0.0f..1.0f], 0.40f = dotted 1/16 default */
    float dub_delay_wet;            /* [0.0f..1.0f], default 0.22f */
    float dub_delay_feedback;       /* [0.0f..0.85f], default 0.45f */
    bool dub_delay_free_dragging;   /* True while F3 moves; false locks tempo division */
    uint8_t dub_delay_div;          /* Nearest locked division [0..5] */
    uint8_t stutter_div;            /* Momentary stutter division (0=OFF, 2=1/8 .. 5=1/64) */
    uint8_t master_volume;          /* Master output volume [0..127], default 64 */

    /* 4-Voice Synth Parameters ([track 0..3][param 0..3]) + Kit Selection */
    float voice_params[SP1_SEQ_NUM_TRACKS][SP1_UI_NUM_VOICE_PARAMS];
    float kick_decay;               /* Convenience mirror of voice_params[0][1] */
    float kick_drive;               /* Convenience mirror of voice_params[0][3] */
    float snare_morph;              /* Convenience mirror of voice_params[1][1] */
    float hat_decay;                /* Convenience mirror of voice_params[2][1] */
    float perc_timbre;              /* Convenience mirror of voice_params[3][3] */
    uint8_t active_kit;             /* Curated kit index [0..3] */
    uint8_t kit_change_seq;         /* Increments whenever a kit preset is loaded */

    /* 4-Track Mixer Channel Strip */
    float track_level[SP1_SEQ_NUM_TRACKS];      /* [0.0f..1.0f], default 0.87f (~unity) */
    float track_pan[SP1_SEQ_NUM_TRACKS];        /* [0.0f..1.0f], 0.50f = center */
    float track_delay_send[SP1_SEQ_NUM_TRACKS]; /* [0.0f..1.0f], Snare/Perc pre-sent */
    float track_tone[SP1_SEQ_NUM_TRACKS];       /* [0.0f..1.0f], 0.50f = flat bypass */

    /* Live Dub Siren (Function hold during playback) */
    bool siren_gate;
    uint8_t pad_siren;
    float siren_pitch;              /* [0.0f..1.0f], remembered across blasts */
    float siren_lfo_rate;           /* [0.0f..1.0f], remembered across blasts */
    float siren_lfo_shape;          /* [0.0f..1.0f], remembered across blasts */
    float siren_delay_send;         /* [0.0f..1.0f], Siren throw & feedback boost */
} sp1_ui_dsp_params_t;

/* Screenless 4-Mode UI Gesture & Dual 4-LED State.
 * Static RAM: ~248 bytes (zero dynamic allocation). */
typedef struct {
    sp1_ui_mode_t mode;
    int8_t prev_held_track;                     /* Previously held T1..T4 index (-1 = none) */
    uint8_t focused_track;                      /* Last adjusted track [0..3] */
    bool initialized;

    uint8_t track_flash[SP1_SEQ_NUM_TRACKS];    /* Per-track decaying hit flash [0..255] */
    uint16_t mode_entry_raw[SP1_NUM_FADERS];    /* Latched raw fader values on page change */
    uint16_t last_applied_raw[SP1_NUM_FADERS];  /* Last raw fader value applied */
    bool fader_engaged[SP1_NUM_FADERS];         /* Anti-clobber engage flags per fader */
    uint16_t btn_consumed_mask;                 /* Suppresses release actions on chorded buttons */

    /* Function hold vs. short-tap & Dub Siren state */
    uint32_t fn_held_ms;
    bool fn_fader_used;
    bool fn_siren_latched;
    bool siren_active;
    uint8_t pad0;
    float siren_lfo_vis;                        /* [0.0f..1.0f] LFO phase for Track LED wave */

    /* Mode 2 RWD/FWD hold auto-repeat & Mode 4 F3 Tape Delay settle timers */
    uint32_t rocker_hold_ms;
    uint32_t rocker_repeat_ms;
    uint32_t delay_drag_ms;

    /* Side LED priority timers & Battery state */
    uint32_t volume_hud_ms;
    uint32_t mode_toast_ms;
    uint32_t battery_toast_ms;
    uint32_t anim_phase_ms;
    float battery_level;                        /* [0.0f..1.0f] LiPo state-of-charge */
    bool battery_charging;
    bool usb_present;
    bool power_state_seeded;
    uint8_t pad1;

    /* Track LED temporary parameter meter overlay state */
    sp1_led_meter_style_t meter_style;
    uint32_t meter_ms;
    float meter_norm;                           /* [0.0f..1.0f] for UNBALANCED_BAR */
    float meter_bipolar;                        /* [-1.0f..+1.0f] for MIDDLE_BALANCED */
    uint32_t meter_int;                         /* Integer value for BASE2_COUNTER */
    uint8_t meter_slot;                         /* Slot index for DISCRETE_SELECT */
    uint8_t meter_num_slots;                    /* Total slots (4 or 6) for DISCRETE_SELECT */
    uint8_t pad2[2];

    sp1_ui_dsp_params_t dsp;
} sp1_ui_mapper_t;

/* ── Standard 4-LED Widget Primitives ────────────────────────────────────── */

void sp1_ui_led_render_unbalanced(float norm_0_to_1, uint8_t out_leds[SP1_NUM_TRACK_LEDS]);
void sp1_ui_led_render_middle_balanced(float bipolar_neg1_to_pos1,
                                       uint8_t out_leds[SP1_NUM_TRACK_LEDS]);
void sp1_ui_led_render_base2(uint32_t value, uint8_t out_leds[SP1_NUM_TRACK_LEDS]);
void sp1_ui_led_render_discrete(uint8_t slot_idx,
                                uint8_t num_slots,
                                uint8_t out_leds[SP1_NUM_TRACK_LEDS]);

/* ── Public UI Mapper API ────────────────────────────────────────────────── */

void sp1_ui_mapper_init(sp1_ui_mapper_t *ui);

float sp1_ui_fader_norm_to_nudge_ms(float norm);
float sp1_ui_fader_norm_to_bipolar(float norm);

/**
 * Process one control snapshot at default 20 ms (50 Hz) control cadence.
 */
void sp1_ui_mapper_update(sp1_ui_mapper_t *ui,
                          sp1_sequencer_t *seq,
                          const sp1_controls_snapshot_t *snap);

/**
 * Process one control snapshot with explicit elapsed dt_ms (for non-20ms simulation/tests).
 */
void sp1_ui_mapper_update_ex(sp1_ui_mapper_t *ui,
                             sp1_sequencer_t *seq,
                             const sp1_controls_snapshot_t *snap,
                             uint32_t dt_ms);

void sp1_ui_mapper_set_mode(sp1_ui_mapper_t *ui,
                            sp1_sequencer_t *seq,
                            const sp1_controls_snapshot_t *snap,
                            sp1_ui_mode_t mode);
sp1_ui_mode_t sp1_ui_mapper_get_mode(const sp1_ui_mapper_t *ui);

/* Compatibility wrappers mapping Performance Mode <-> fx_mode */
void sp1_ui_mapper_set_fx_mode(sp1_ui_mapper_t *ui,
                               const sp1_controls_snapshot_t *snap,
                               bool enabled);
bool sp1_ui_mapper_is_fx_mode(const sp1_ui_mapper_t *ui);

void sp1_ui_mapper_set_power_state(sp1_ui_mapper_t *ui,
                                   float battery_0_to_1,
                                   bool is_charging,
                                   bool usb_present);
void sp1_ui_mapper_set_siren_visual(sp1_ui_mapper_t *ui, float lfo_0_to_1);

const sp1_ui_dsp_params_t *sp1_ui_mapper_get_dsp_params(const sp1_ui_mapper_t *ui);
uint8_t sp1_ui_mapper_get_master_volume(const sp1_ui_mapper_t *ui);
uint8_t sp1_ui_mapper_focused_track(const sp1_ui_mapper_t *ui);

void sp1_ui_mapper_on_triggers(sp1_ui_mapper_t *ui,
                               const sp1_seq_trigger_event_t *triggers,
                               uint32_t count);

void sp1_ui_mapper_render_leds(sp1_ui_mapper_t *ui,
                               const sp1_sequencer_t *seq,
                               uint32_t dt_ms,
                               sp1_led_frame_t *out_frame);

/* ── Function (P0.27) Power Hold & Release-Bounce Debounce State ─────────── */

#define SP1_UI_POWER_HOLD_SHUTDOWN_MS       3000U   /* 3.0 s Function hold while stopped */
#define SP1_UI_POWER_HOLD_WAKE_MS           1500U   /* 1.5 s Function hold to power on */
#define SP1_UI_POWER_RELEASE_DEBOUNCE_MS    40U     /* 40 ms release debounce against bounce */

typedef struct {
    uint32_t hold_elapsed_ms;
    uint32_t release_bounce_ms;
    bool holding;
    bool wait_for_release;
} sp1_ui_power_hold_t;

void sp1_ui_power_hold_init(sp1_ui_power_hold_t *state, bool wait_for_initial_release);

bool sp1_ui_power_hold_update(sp1_ui_power_hold_t *state,
                              bool fn_pressed,
                              uint32_t dt_ms,
                              uint32_t target_hold_ms,
                              uint32_t release_debounce_ms);

#ifdef __cplusplus
}
#endif

#endif /* SP1_CORE_UI_MAPPER_H */
