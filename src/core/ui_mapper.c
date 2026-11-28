#include "core/ui_mapper.h"

#include <stddef.h>
#include <string.h>

#define SP1_UI_NUDGE_INV_SPAN   (1.0f / SP1_UI_NUDGE_DEADZONE_LOW)
#define SP1_UI_BIPOLAR_INV_SPAN (1.0f / SP1_UI_BIPOLAR_DEADZONE_LOW)
#define SP1_UI_INV_127          0.007874015748f
#define SP1_UI_INV_6000         0.00016666667f  /* 1.0f / 6000.0f (60..120..180 BPM span) */

static const sp1_button_id_t k_track_buttons[SP1_SEQ_NUM_TRACKS] = {
    SP1_BTN_TRACK_1,
    SP1_BTN_TRACK_2,
    SP1_BTN_TRACK_3,
    SP1_BTN_TRACK_4,
};

/* Mirror of k_drum_kit_presets in drum_voices.c so UI state stays synchronized on kit load */
static const float k_ui_kit_presets[SP1_UI_NUM_KITS][SP1_SEQ_NUM_TRACKS][SP1_UI_NUM_VOICE_PARAMS] = {
    {
        { 0.50f, 0.45f, 0.50f, 0.35f },
        { 0.50f, 0.65f, 0.65f, 0.50f },
        { 0.50f, 0.20f, 0.50f, 0.50f },
        { 0.50f, 0.45f, 0.45f, 0.45f },
    },
    {
        { 0.64f, 0.22f, 0.35f, 0.18f },
        { 0.52f, 0.08f, 0.15f, 0.35f },
        { 0.66f, 0.08f, 0.65f, 0.30f },
        { 0.38f, 0.12f, 0.28f, 0.12f },
    },
    {
        { 0.34f, 0.72f, 0.88f, 0.88f },
        { 0.72f, 0.82f, 0.75f, 0.84f },
        { 0.22f, 0.55f, 0.32f, 0.15f },
        { 0.68f, 0.88f, 0.68f, 0.92f },
    },
    {
        { 0.20f, 0.84f, 0.28f, 0.45f },
        { 0.38f, 0.55f, 0.80f, 0.26f },
        { 0.56f, 0.72f, 0.68f, 0.78f },
        { 0.76f, 0.28f, 0.86f, 0.42f },
    },
};

static inline float clamp01(float x) {
    if (x < 0.0f) {
        return 0.0f;
    }
    if (x > 1.0f) {
        return 1.0f;
    }
    return x;
}

static int32_t abs_diff_u16(uint16_t a, uint16_t b) {
    return (a >= b) ? (int32_t)(a - b) : (int32_t)(b - a);
}

static uint32_t div_by_24_u32(uint32_t v) {
    /* Exact floor(v / 24) without hardware division: 2^36 / 24 = 0xAAAAAAAB. */
    return (uint32_t)(((uint64_t)v * 0xAAAAAAABULL) >> 36U);
}

static void latch_faders(sp1_ui_mapper_t *ui, const sp1_controls_snapshot_t *snap) {
    for (uint8_t i = 0U; i < SP1_NUM_FADERS; ++i) {
        if (snap != NULL) {
            ui->mode_entry_raw[i] = snap->fader_raw[i];
        }
        ui->fader_engaged[i] = false;
    }
}

static void sync_legacy_voice_mirrors(sp1_ui_dsp_params_t *dsp) {
    dsp->kick_decay  = dsp->voice_params[0][1];
    dsp->kick_drive  = dsp->voice_params[0][3];
    dsp->snare_morph = dsp->voice_params[1][1];
    dsp->hat_decay   = dsp->voice_params[2][1];
    dsp->perc_timbre = dsp->voice_params[3][3];
}

static void apply_kit_to_dsp(sp1_ui_dsp_params_t *dsp, uint8_t kit_idx) {
    const uint8_t k = (kit_idx < SP1_UI_NUM_KITS) ? kit_idx : 0U;
    dsp->active_kit = k;
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        for (uint8_t p = 0U; p < SP1_UI_NUM_VOICE_PARAMS; ++p) {
            dsp->voice_params[t][p] = k_ui_kit_presets[k][t][p];
        }
    }
    sync_legacy_voice_mirrors(dsp);
    dsp->kit_change_seq++;
}

static int8_t resolve_held_track(uint16_t buttons_down) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        if ((buttons_down & SP1_BTN_BIT(k_track_buttons[t])) != 0U) {
            return (int8_t)t;
        }
    }
    return -1;
}

static void set_meter_unbalanced(sp1_ui_mapper_t *ui, float norm, uint32_t hold_ms) {
    ui->meter_style = SP1_LED_METER_UNBALANCED;
    ui->meter_norm = clamp01(norm);
    ui->meter_ms = hold_ms;
}

static void set_meter_bipolar(sp1_ui_mapper_t *ui, float bipolar, uint32_t hold_ms) {
    if (bipolar < -1.0f) {
        bipolar = -1.0f;
    } else if (bipolar > 1.0f) {
        bipolar = 1.0f;
    }
    ui->meter_style = SP1_LED_METER_MIDDLE_BALANCED;
    ui->meter_bipolar = bipolar;
    ui->meter_ms = hold_ms;
}

static void set_meter_base2(sp1_ui_mapper_t *ui, uint32_t val, uint32_t hold_ms) {
    ui->meter_style = SP1_LED_METER_BASE2;
    ui->meter_int = val;
    ui->meter_ms = hold_ms;
}

static void set_meter_discrete(sp1_ui_mapper_t *ui,
                               uint8_t slot,
                               uint8_t num_slots,
                               uint32_t hold_ms) {
    ui->meter_style = SP1_LED_METER_DISCRETE;
    ui->meter_slot = slot;
    ui->meter_num_slots = num_slots;
    ui->meter_ms = hold_ms;
}

static void adjust_bpm_clamped(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq, int32_t delta_x100) {
    const int32_t cur = (int32_t)sp1_seq_get_bpm_x100(seq);
    int32_t next = cur + delta_x100;
    if (next < (int32_t)SP1_UI_BPM_MIN_X100) {
        next = (int32_t)SP1_UI_BPM_MIN_X100;
    } else if (next > (int32_t)SP1_UI_BPM_MAX_X100) {
        next = (int32_t)SP1_UI_BPM_MAX_X100;
    }
    sp1_seq_set_bpm_x100(seq, (uint32_t)next);

    /* Middle-balanced meter centered at 120.00 BPM ([119..121] snaps to center pair) */
    float bi = 0.0f;
    if (next < 11900 || next > 12100) {
        bi = (float)(next - (int32_t)SP1_UI_BPM_CENTER_X100) * SP1_UI_INV_6000;
    }
    set_meter_bipolar(ui, bi, SP1_UI_PARAM_METER_ROCKER_MS);
}

static void adjust_swing_clamped(sp1_ui_mapper_t *ui, sp1_sequencer_t *seq, float delta) {
    sp1_seq_set_swing(seq, sp1_seq_get_swing(seq) + delta);
    const float sw = sp1_seq_get_swing(seq);
    const float norm = (sw - SP1_SEQ_SWING_MIN) * 6.25f; /* 1.0f / 0.16f = 6.25f */
    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_ROCKER_MS);
}

static void adjust_volume_clamped(sp1_ui_mapper_t *ui, int32_t delta) {
    int32_t next = (int32_t)ui->dsp.master_volume + delta;
    if (next < (int32_t)SP1_UI_VOLUME_MIN) {
        next = (int32_t)SP1_UI_VOLUME_MIN;
    } else if (next > (int32_t)SP1_UI_VOLUME_MAX) {
        next = (int32_t)SP1_UI_VOLUME_MAX;
    }
    ui->dsp.master_volume = (uint8_t)next;
    ui->volume_hud_ms = SP1_UI_VOLUME_HUD_MS;
}

static sp1_track_speed_t map_norm_to_track_speed(float norm) {
    if (norm < 0.25f) {
        return SP1_TRACK_SPEED_DIV4;
    }
    if (norm < 0.50f) {
        return SP1_TRACK_SPEED_DIV2;
    }
    if (norm < 0.75f) {
        return SP1_TRACK_SPEED_1X;
    }
    return SP1_TRACK_SPEED_2X;
}

/* ── Standard 4-LED Widget Implementations ───────────────────────────────── */

void sp1_ui_led_render_unbalanced(float norm_0_to_1, uint8_t out_leds[SP1_NUM_TRACK_LEDS]) {
    if (out_leds == NULL) {
        return;
    }
    const float c = clamp01(norm_0_to_1);
    uint32_t rem = (uint32_t)(c * 1020.0f + 0.5f);
    if (rem > 1020U) {
        rem = 1020U;
    }
    for (uint8_t i = 0U; i < SP1_NUM_TRACK_LEDS; ++i) {
        if (rem >= 255U) {
            out_leds[i] = 255U;
            rem -= 255U;
        } else {
            out_leds[i] = (uint8_t)rem;
            rem = 0U;
        }
    }
}

void sp1_ui_led_render_middle_balanced(float bipolar_neg1_to_pos1,
                                       uint8_t out_leds[SP1_NUM_TRACK_LEDS]) {
    if (out_leds == NULL) {
        return;
    }
    out_leds[0] = 0U;
    out_leds[1] = 0U;
    out_leds[2] = 0U;
    out_leds[3] = 0U;

    if (bipolar_neg1_to_pos1 > -0.001f && bipolar_neg1_to_pos1 < 0.001f) {
        out_leds[1] = SP1_UI_LED_NUDGE_CENTER;
        out_leds[2] = SP1_UI_LED_NUDGE_CENTER;
        return;
    }

    if (bipolar_neg1_to_pos1 < 0.0f) {
        float mag = -bipolar_neg1_to_pos1;
        if (mag > 1.0f) {
            mag = 1.0f;
        }
        uint32_t scaled = (uint32_t)(mag * 510.0f + 0.5f);
        if (scaled < 48U) {
            scaled = 48U;
        }
        out_leds[1] = (scaled >= 255U) ? 255U : (uint8_t)scaled;
        out_leds[0] = (scaled > 255U) ? (uint8_t)(scaled - 255U) : 0U;
    } else {
        float mag = bipolar_neg1_to_pos1;
        if (mag > 1.0f) {
            mag = 1.0f;
        }
        uint32_t scaled = (uint32_t)(mag * 510.0f + 0.5f);
        if (scaled < 48U) {
            scaled = 48U;
        }
        out_leds[2] = (scaled >= 255U) ? 255U : (uint8_t)scaled;
        out_leds[3] = (scaled > 255U) ? (uint8_t)(scaled - 255U) : 0U;
    }
}

void sp1_ui_led_render_base2(uint32_t value, uint8_t out_leds[SP1_NUM_TRACK_LEDS]) {
    if (out_leds == NULL) {
        return;
    }
    /* Mod-16 4-bit binary (MSB..LSB): 0, 16, 32 -> 0000; 1, 17 -> 0001; ...; 15, 31 -> 1111 */
    const uint32_t bits = value & 0x0FU;
    out_leds[0] = ((bits & 8U) != 0U) ? 220U : 0U;
    out_leds[1] = ((bits & 4U) != 0U) ? 220U : 0U;
    out_leds[2] = ((bits & 2U) != 0U) ? 220U : 0U;
    out_leds[3] = ((bits & 1U) != 0U) ? 220U : 0U;
}

void sp1_ui_led_render_discrete(uint8_t slot_idx,
                                uint8_t num_slots,
                                uint8_t out_leds[SP1_NUM_TRACK_LEDS]) {
    if (out_leds == NULL) {
        return;
    }
    out_leds[0] = 0U;
    out_leds[1] = 0U;
    out_leds[2] = 0U;
    out_leds[3] = 0U;

    if (num_slots == 6U) {
        /* 6 Dub Delay Tempo Divisions: 1/32, 1/16, dot-1/16, 1/8, dot-1/8, 1/4 */
        switch (slot_idx) {
        case 0U: out_leds[0] = 255U; break;
        case 1U: out_leds[0] = 180U; out_leds[1] = 180U; break;
        case 2U: out_leds[1] = 255U; break;
        case 3U: out_leds[1] = 180U; out_leds[2] = 180U; break;
        case 4U: out_leds[2] = 255U; break;
        default: out_leds[3] = 255U; break;
        }
        return;
    }

    out_leds[slot_idx & 3U] = 255U;
}

/* ── Public UI Mapper Lifecycle & Helpers ────────────────────────────────── */

void sp1_ui_mapper_init(sp1_ui_mapper_t *ui) {
    if (ui == NULL) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->mode = SP1_UI_MODE_PATTERN;
    ui->prev_held_track = -1;
    ui->focused_track = 0U;
    ui->initialized = false;
    ui->battery_level = 1.0f;
    ui->battery_toast_ms = SP1_UI_BATTERY_TOAST_MS;
    ui->siren_lfo_vis = 0.5f;

    /* Default DSP Master FX, Volume, Kit 0 Voices, Channel Strips & Dub Siren */
    ui->dsp.dj_filter_pos = 0.50f;
    ui->dsp.dj_resonance_q = 1.85f;
    ui->dsp.crush_amount = 0.0f;
    ui->dsp.dub_delay_time = 0.40f;
    ui->dsp.dub_delay_wet = 0.22f;
    ui->dsp.dub_delay_feedback = 0.45f;
    ui->dsp.dub_delay_free_dragging = false;
    ui->dsp.dub_delay_div = 2U; /* Dotted 1/16 */
    ui->dsp.stutter_div = 0U;
    ui->dsp.master_volume = SP1_UI_VOLUME_DEFAULT;

    apply_kit_to_dsp(&ui->dsp, 0U);
    ui->dsp.kit_change_seq = 0U;

    static const float k_init_sends[SP1_SEQ_NUM_TRACKS] = { 0.0f, 0.55f, 0.0f, 0.60f };
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        ui->dsp.track_level[t] = 0.87f;
        ui->dsp.track_pan[t] = 0.50f;
        ui->dsp.track_delay_send[t] = k_init_sends[t];
        ui->dsp.track_tone[t] = 0.50f;
    }

    ui->dsp.siren_gate = false;
    ui->dsp.siren_pitch = 0.42f;
    ui->dsp.siren_lfo_rate = 0.35f;
    ui->dsp.siren_lfo_shape = 0.78f;
    ui->dsp.siren_delay_send = 0.65f;
}

float sp1_ui_fader_norm_to_nudge_ms(float norm) {
    const float c = clamp01(norm);
    if (c >= SP1_UI_NUDGE_DEADZONE_LOW && c <= SP1_UI_NUDGE_DEADZONE_HIGH) {
        return 0.0f;
    }
    if (c < SP1_UI_NUDGE_DEADZONE_LOW) {
        const float t = (SP1_UI_NUDGE_DEADZONE_LOW - c) * SP1_UI_NUDGE_INV_SPAN;
        return -t * SP1_SEQ_NUDGE_MAX_MS;
    }
    const float t = (c - SP1_UI_NUDGE_DEADZONE_HIGH) * SP1_UI_NUDGE_INV_SPAN;
    return t * SP1_SEQ_NUDGE_MAX_MS;
}

float sp1_ui_fader_norm_to_bipolar(float norm) {
    const float c = clamp01(norm);
    if (c >= SP1_UI_BIPOLAR_DEADZONE_LOW && c <= SP1_UI_BIPOLAR_DEADZONE_HIGH) {
        return 0.0f;
    }
    if (c < SP1_UI_BIPOLAR_DEADZONE_LOW) {
        return -((SP1_UI_BIPOLAR_DEADZONE_LOW - c) * SP1_UI_BIPOLAR_INV_SPAN);
    }
    return (c - SP1_UI_BIPOLAR_DEADZONE_HIGH) * SP1_UI_BIPOLAR_INV_SPAN;
}

void sp1_ui_mapper_set_mode(sp1_ui_mapper_t *ui,
                            sp1_sequencer_t *seq,
                            const sp1_controls_snapshot_t *snap,
                            sp1_ui_mode_t mode) {
    if (ui == NULL) {
        return;
    }
    const sp1_ui_mode_t next =
        ((unsigned)mode < (unsigned)SP1_UI_MODE_COUNT) ? mode : SP1_UI_MODE_PATTERN;

    ui->mode = next;
    ui->mode_toast_ms = SP1_UI_MODE_TOAST_TOTAL_MS;
    ui->meter_ms = 0U;
    ui->rocker_hold_ms = 0U;
    ui->rocker_repeat_ms = 0U;

    if (next != SP1_UI_MODE_PERF) {
        ui->dsp.stutter_div = 0U;
        if (seq != NULL) {
            sp1_seq_set_tempo_bend(seq, 0);
        }
    }
    latch_faders(ui, snap);
}

sp1_ui_mode_t sp1_ui_mapper_get_mode(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->mode : SP1_UI_MODE_PATTERN;
}

void sp1_ui_mapper_set_fx_mode(sp1_ui_mapper_t *ui,
                               const sp1_controls_snapshot_t *snap,
                               bool enabled) {
    sp1_ui_mapper_set_mode(ui, NULL, snap, enabled ? SP1_UI_MODE_PERF : SP1_UI_MODE_PATTERN);
}

bool sp1_ui_mapper_is_fx_mode(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) && (ui->mode == SP1_UI_MODE_PERF);
}

void sp1_ui_mapper_set_power_state(sp1_ui_mapper_t *ui,
                                   float battery_0_to_1,
                                   bool is_charging,
                                   bool usb_present) {
    if (ui == NULL) {
        return;
    }
    ui->battery_level = clamp01(battery_0_to_1);
    ui->battery_charging = is_charging;

    if (!ui->power_state_seeded) {
        ui->usb_present = usb_present;
        ui->power_state_seeded = true;
    } else if (ui->usb_present != usb_present) {
        ui->usb_present = usb_present;
        ui->battery_toast_ms = SP1_UI_BATTERY_TOAST_MS;
    }
}

void sp1_ui_mapper_set_siren_visual(sp1_ui_mapper_t *ui, float lfo_0_to_1) {
    if (ui != NULL) {
        ui->siren_lfo_vis = clamp01(lfo_0_to_1);
    }
}

const sp1_ui_dsp_params_t *sp1_ui_mapper_get_dsp_params(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? &ui->dsp : NULL;
}

uint8_t sp1_ui_mapper_get_master_volume(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->dsp.master_volume : SP1_UI_VOLUME_DEFAULT;
}

uint8_t sp1_ui_mapper_focused_track(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->focused_track : 0U;
}

/* ── Main 4-Mode Gesture Processor ───────────────────────────────────────── */

void sp1_ui_mapper_update(sp1_ui_mapper_t *ui,
                          sp1_sequencer_t *seq,
                          const sp1_controls_snapshot_t *snap) {
    sp1_ui_mapper_update_ex(ui, seq, snap, 20U);
}

void sp1_ui_mapper_update_ex(sp1_ui_mapper_t *ui,
                             sp1_sequencer_t *seq,
                             const sp1_controls_snapshot_t *snap,
                             uint32_t dt_ms) {
    if (ui == NULL || seq == NULL || snap == NULL) {
        return;
    }

    const uint16_t down = snap->buttons_down;
    const uint16_t pressed = snap->buttons_pressed;
    const uint16_t released = snap->buttons_released;
    const bool play_held = (down & SP1_BTN_BIT(SP1_BTN_PLAY)) != 0U;
    const bool fn_held = (down & SP1_BTN_BIT(SP1_BTN_FN)) != 0U;

    ui->btn_consumed_mask &= (uint16_t)(~pressed);

    /* Enforce AIN0 resistor-ladder safety whenever Play is held */
    if (play_held) {
        ui->btn_consumed_mask |= down;
    }

    if (!ui->initialized) {
        ui->initialized = true;
        ui->prev_held_track = (!play_held && !fn_held) ? resolve_held_track(down) : -1;
    }

    /* 1. Standalone Master Volume Buttons (Vol- / Vol+):
     *    100% independent of modes and modifiers; never forms combinations. */
    if (!play_held) {
        const uint16_t vol_mask =
            (uint16_t)(SP1_BTN_BIT(SP1_BTN_VOL_UP) | SP1_BTN_BIT(SP1_BTN_VOL_DOWN));
        if (((pressed | released) & vol_mask) != 0U) {
            /* Guard against transient RWD/FWD voltage crossings on shared AIN1 ladder */
            ui->btn_consumed_mask |=
                (uint16_t)(SP1_BTN_BIT(SP1_BTN_RWD) | SP1_BTN_BIT(SP1_BTN_FWD));
        }
        if ((pressed & SP1_BTN_BIT(SP1_BTN_VOL_UP)) != 0U) {
            adjust_volume_clamped(ui, (int32_t)SP1_UI_VOLUME_STEP);
        }
        if ((pressed & SP1_BTN_BIT(SP1_BTN_VOL_DOWN)) != 0U) {
            adjust_volume_clamped(ui, -(int32_t)SP1_UI_VOLUME_STEP);
        }
    }

    /* 2. Play Button: Transport Start / Stop Toggle */
    if ((pressed & SP1_BTN_BIT(SP1_BTN_PLAY)) != 0U && !fn_held) {
        if (sp1_seq_is_running(seq)) {
            sp1_seq_stop(seq);
            sp1_seq_set_tempo_bend(seq, 0);
            ui->siren_active = false;
            ui->dsp.siren_gate = false;
            ui->battery_toast_ms = SP1_UI_BATTERY_TOAST_MS;
        } else {
            sp1_seq_start(seq);
        }
    }

    /* 3. Function (••) Button:
     *    - Short press (< 200 ms, no fader moved): Cycle Mode 1 -> 2 -> 3 -> 4 -> 1
     *    - Hold while Playing (>= 200 ms or + F1–F4): Gate on live Dub Siren */
    if ((pressed & SP1_BTN_BIT(SP1_BTN_FN)) != 0U) {
        ui->fn_held_ms = 0U;
        ui->fn_fader_used = false;
        ui->fn_siren_latched = false;
        latch_faders(ui, snap);
    } else if (fn_held) {
        ui->fn_held_ms += dt_ms;
    }

    if (fn_held) {
        /* Suppress stray release actions on any track/rocker buttons pressed during Function */
        ui->btn_consumed_mask |= (uint16_t)(down & (uint16_t)(~SP1_BTN_BIT(SP1_BTN_FN)));

        if (sp1_seq_is_running(seq) &&
            (ui->fn_held_ms >= SP1_UI_FN_SIREN_HOLD_MS || ui->fn_fader_used)) {
            ui->fn_siren_latched = true;
            ui->siren_active = true;
            ui->dsp.siren_gate = true;
        }
    }

    if ((released & SP1_BTN_BIT(SP1_BTN_FN)) != 0U) {
        const bool is_short_tap =
            !play_held &&
            !ui->fn_siren_latched &&
            !ui->fn_fader_used &&
            (ui->fn_held_ms < SP1_UI_FN_SIREN_HOLD_MS);

        if (ui->siren_active || ui->dsp.siren_gate) {
            ui->siren_active = false;
            ui->dsp.siren_gate = false;
            latch_faders(ui, snap);
        }

        if (is_short_tap) {
            const sp1_ui_mode_t next_mode =
                (sp1_ui_mode_t)(((uint8_t)ui->mode + 1U) & 3U);
            sp1_ui_mapper_set_mode(ui, seq, snap, next_mode);
        } else {
            latch_faders(ui, snap);
        }

        ui->fn_held_ms = 0U;
        ui->fn_fader_used = false;
        ui->fn_siren_latched = false;
    }

    /* 4. Track Button Transitions & Per-Mode Button Actions (T1..T4) */
    const int8_t held_track = (!play_held && !fn_held) ? resolve_held_track(down) : -1;

    if (held_track != ui->prev_held_track) {
        /* Re-latch all 4 faders on T1..T4 press/release in Modes 1..3 so T_x + F1..F4
         * never clobbers unshifted parameters. In Mode 4 (Performance), keep faders live
         * so F1..F4 sweep FX smoothly over momentary T1..T4 stutters! */
        if (ui->mode != SP1_UI_MODE_PERF) {
            latch_faders(ui, snap);
        }
        ui->prev_held_track = held_track;
    }

    if (!play_held && !fn_held) {
        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            const uint16_t t_bit = SP1_BTN_BIT(k_track_buttons[t]);

            if (ui->mode == SP1_UI_MODE_SOUND) {
                /* Sound Edit Mode: immediate zero-latency finger-drum / audition on press */
                if ((pressed & t_bit) != 0U) {
                    sp1_seq_trigger_audition(seq, t, 110U);
                    ui->focused_track = t;
                }
            } else if (ui->mode == SP1_UI_MODE_PATTERN || ui->mode == SP1_UI_MODE_TRACK) {
                /* Pattern & Track Modes: toggle track mute on release when no fader moved */
                if ((released & t_bit) != 0U && (ui->btn_consumed_mask & t_bit) == 0U) {
                    sp1_seq_toggle_track_mute(seq, t);
                }
            }
        }
    }

    /* Performance Mode Momentary Stutter Punch-In (Hold T1..T4 = 1/8, 1/16, 1/32, 1/64) */
    if (ui->mode == SP1_UI_MODE_PERF && !play_held && !fn_held) {
        uint8_t mom_div = 0U;
        if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_4)) != 0U) {
            mom_div = 5U; /* 1/64 */
        } else if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_3)) != 0U) {
            mom_div = 4U; /* 1/32 */
        } else if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_2)) != 0U) {
            mom_div = 3U; /* 1/16 */
        } else if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_1)) != 0U) {
            mom_div = 2U; /* 1/8 */
        }
        ui->dsp.stutter_div = mom_div;
    } else {
        ui->dsp.stutter_div = 0U;
    }

    /* 5. Mode-Dependent RWD (<<) / FWD (>>) Rocker */
    if (!play_held && !fn_held) {
        const bool rwd_press =
            (pressed & SP1_BTN_BIT(SP1_BTN_RWD)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_RWD)) == 0U;
        const bool fwd_press =
            (pressed & SP1_BTN_BIT(SP1_BTN_FWD)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_FWD)) == 0U;
        const bool rwd_down =
            (down & SP1_BTN_BIT(SP1_BTN_RWD)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_RWD)) == 0U;
        const bool fwd_down =
            (down & SP1_BTN_BIT(SP1_BTN_FWD)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_FWD)) == 0U;

        switch (ui->mode) {
        case SP1_UI_MODE_PATTERN:
            sp1_seq_set_tempo_bend(seq, 0);
            if (rwd_press) {
                adjust_swing_clamped(ui, seq, -SP1_UI_SWING_STEP);
            }
            if (fwd_press) {
                adjust_swing_clamped(ui, seq, SP1_UI_SWING_STEP);
            }
            break;

        case SP1_UI_MODE_TRACK:
            sp1_seq_set_tempo_bend(seq, 0);
            if (rwd_press) {
                adjust_bpm_clamped(ui, seq, -(int32_t)SP1_UI_BPM_FINE_STEP_X100);
                ui->rocker_hold_ms = 0U;
                ui->rocker_repeat_ms = 0U;
            } else if (fwd_press) {
                adjust_bpm_clamped(ui, seq, (int32_t)SP1_UI_BPM_FINE_STEP_X100);
                ui->rocker_hold_ms = 0U;
                ui->rocker_repeat_ms = 0U;
            } else if (rwd_down || fwd_down) {
                ui->rocker_hold_ms += dt_ms;
                if (ui->rocker_hold_ms >= SP1_UI_ROCKER_HOLD_DELAY_MS) {
                    ui->rocker_repeat_ms += dt_ms;
                    while (ui->rocker_repeat_ms >= SP1_UI_ROCKER_REPEAT_MS) {
                        ui->rocker_repeat_ms -= SP1_UI_ROCKER_REPEAT_MS;
                        const int32_t step = rwd_down ? -(int32_t)SP1_UI_BPM_HOLD_STEP_X100
                                                      : (int32_t)SP1_UI_BPM_HOLD_STEP_X100;
                        adjust_bpm_clamped(ui, seq, step);
                    }
                }
            } else {
                ui->rocker_hold_ms = 0U;
                ui->rocker_repeat_ms = 0U;
            }
            break;

        case SP1_UI_MODE_SOUND:
            sp1_seq_set_tempo_bend(seq, 0);
            if (rwd_press || fwd_press) {
                const uint8_t cur_k = ui->dsp.active_kit;
                const uint8_t next_k =
                    rwd_press ? (uint8_t)((cur_k + SP1_UI_NUM_KITS - 1U) & 3U)
                              : (uint8_t)((cur_k + 1U) & 3U);
                apply_kit_to_dsp(&ui->dsp, next_k);
                latch_faders(ui, snap);
                set_meter_discrete(ui, next_k, SP1_UI_NUM_KITS, SP1_UI_PARAM_METER_ROCKER_MS);
            }
            break;

        case SP1_UI_MODE_PERF:
            if (rwd_down) {
                sp1_seq_set_tempo_bend(seq, -1);
            } else if (fwd_down) {
                sp1_seq_set_tempo_bend(seq, 1);
            } else {
                sp1_seq_set_tempo_bend(seq, 0);
            }
            break;

        default:
            break;
        }
    } else if (ui->mode != SP1_UI_MODE_PERF || play_held || fn_held) {
        sp1_seq_set_tempo_bend(seq, 0);
        ui->rocker_hold_ms = 0U;
        ui->rocker_repeat_ms = 0U;
    }

    /* 6. Faders F1..F4 Processing */
    bool f3_delay_moved = false;

    if (play_held) {
        latch_faders(ui, snap);
    } else {
        for (uint8_t f = 0U; f < SP1_NUM_FADERS; ++f) {
            if (!ui->fader_engaged[f]) {
                if (abs_diff_u16(snap->fader_raw[f], ui->mode_entry_raw[f]) <
                    SP1_UI_FADER_ENGAGE_DELTA) {
                    continue;
                }
                ui->fader_engaged[f] = true;
            } else if (snap->fader_raw[f] == ui->last_applied_raw[f]) {
                continue;
            }

            ui->last_applied_raw[f] = snap->fader_raw[f];
            const float norm = clamp01(snap->fader_norm[f]);

            /* A. Function Held while Playing -> Dub Siren Live Modulation (Func + F1..F4) */
            if (fn_held) {
                if (!sp1_seq_is_running(seq)) {
                    continue;
                }
                ui->fn_fader_used = true;
                ui->fn_siren_latched = true;
                ui->siren_active = true;
                ui->dsp.siren_gate = true;

                switch (f) {
                case 0U:
                    ui->dsp.siren_pitch = norm;
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                case 1U:
                    ui->dsp.siren_lfo_rate = norm;
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                case 2U: {
                    ui->dsp.siren_lfo_shape = norm;
                    float bi = 0.0f;
                    if (norm < SP1_UI_NUDGE_DEADZONE_LOW) {
                        bi = -((SP1_UI_NUDGE_DEADZONE_LOW - norm) * SP1_UI_NUDGE_INV_SPAN);
                    } else if (norm > SP1_UI_NUDGE_DEADZONE_HIGH) {
                        bi = (norm - SP1_UI_NUDGE_DEADZONE_HIGH) * SP1_UI_NUDGE_INV_SPAN;
                    }
                    set_meter_bipolar(ui, bi, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                }
                case 3U:
                    ui->dsp.siren_delay_send = norm;
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                default:
                    break;
                }
                continue;
            }

            /* Mark any held track button consumed so release never toggles mute */
            if (held_track >= 0) {
                ui->btn_consumed_mask |= (uint16_t)(down & SP1_BTN_TRACK_MASK);
            }

            /* B. Mode-Specific Fader Routing */
            switch (ui->mode) {
            case SP1_UI_MODE_PATTERN: {
                if (held_track >= 0) {
                    const uint8_t t = (uint8_t)held_track;
                    ui->focused_track = t;
                    const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, t);
                    const uint8_t cur_steps = (pat != NULL) ? pat->steps : 16U;

                    switch (f) {
                    case 0U: { /* T_x + F1: Track Clock Speed (0.25x, 0.5x, 1x, 2x) */
                        const sp1_track_speed_t spd = map_norm_to_track_speed(norm);
                        sp1_seq_set_track_speed(seq, t, spd);
                        set_meter_discrete(ui, (uint8_t)spd, 4U, SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    }
                    case 1U: { /* T_x + F2: Start-Beat Rotation (0..n-1) */
                        const uint8_t max_rot = (cur_steps > 1U) ? (uint8_t)(cur_steps - 1U) : 0U;
                        const uint8_t rot = (uint8_t)(norm * (float)max_rot + 0.5f);
                        sp1_seq_set_track_rotation(seq, t, rot);
                        set_meter_base2(ui, (uint32_t)rot, SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    }
                    case 2U: { /* T_x + F3: Dilla Micro-Timing Offset (-35..+35 ms) */
                        const float nudge_ms = sp1_ui_fader_norm_to_nudge_ms(norm);
                        sp1_seq_set_track_nudge_ms(seq, t, nudge_ms);
                        const float bi = nudge_ms * (1.0f / SP1_SEQ_NUDGE_MAX_MS);
                        set_meter_bipolar(ui, bi, SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    }
                    case 3U: { /* T_x + F4: Step Length (1..32, mod-16 Base-2 counter) */
                        const uint8_t steps =
                            (uint8_t)(SP1_EUCLID_MIN_STEPS +
                                      (uint8_t)(norm * (float)(SP1_EUCLID_MAX_STEPS - SP1_EUCLID_MIN_STEPS) +
                                                0.5f));
                        sp1_seq_set_track_steps(seq, t, steps);
                        set_meter_base2(ui, (uint32_t)steps, SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    }
                    default:
                        break;
                    }
                } else {
                    /* Unshifted F1–F4: Track 1–4 Euclidean Hits (0..n) */
                    ui->focused_track = f;
                    const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, f);
                    const uint8_t cur_steps = (pat != NULL) ? pat->steps : 16U;
                    const uint8_t pulses = (uint8_t)(norm * (float)cur_steps + 0.5f);
                    sp1_seq_set_track_pulses(seq, f, pulses);
                    set_meter_base2(ui, (uint32_t)pulses, SP1_UI_PARAM_METER_FADER_MS);
                }
                break;
            }

            case SP1_UI_MODE_TRACK: {
                if (held_track >= 0) {
                    const uint8_t t = (uint8_t)held_track;
                    ui->focused_track = t;
                    switch (f) {
                    case 0U: /* T_x + F1: Per-Track Tone Filter (LP <-> Flat <-> HP) */
                        ui->dsp.track_tone[t] = norm;
                        set_meter_bipolar(ui, sp1_ui_fader_norm_to_bipolar(norm),
                                          SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    case 1U: /* T_x + F2: Per-Track Stereo Pan */
                        ui->dsp.track_pan[t] = norm;
                        set_meter_bipolar(ui, sp1_ui_fader_norm_to_bipolar(norm),
                                          SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    case 2U: /* T_x + F3: Per-Track Dub Delay Send */
                        ui->dsp.track_delay_send[t] = norm;
                        set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    case 3U: /* T_x + F4: Per-Track Hit Probability */
                        sp1_seq_set_track_probability(seq, t, norm);
                        set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                        break;
                    default:
                        break;
                    }
                } else {
                    /* Unshifted F1–F4: Track 1–4 Mix Level */
                    ui->focused_track = f;
                    ui->dsp.track_level[f] = norm;
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                }
                break;
            }

            case SP1_UI_MODE_SOUND: {
                if (held_track >= 0) {
                    const uint8_t t = (uint8_t)held_track;
                    ui->focused_track = t;
                    ui->dsp.voice_params[t][f] = norm;
                    sync_legacy_voice_mirrors(&ui->dsp);
                    if (f == 0U) {
                        set_meter_bipolar(ui, sp1_ui_fader_norm_to_bipolar(norm),
                                          SP1_UI_PARAM_METER_FADER_MS);
                    } else {
                        set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    }
                } else {
                    /* Unshifted F1–F4: Voice 1–4 Signature Character Morph
                     * (preserves custom Pitch [0] and Filter/Sweep edits!) */
                    ui->focused_track = f;
                    switch (f) {
                    case 0U: /* Kick Decay + Drive */
                        ui->dsp.voice_params[0][1] = norm;
                        ui->dsp.voice_params[0][3] = 0.20f + (0.75f * norm);
                        break;
                    case 1U: /* Rim <-> Snare Morph + Wire Decay */
                        ui->dsp.voice_params[1][1] = norm;
                        ui->dsp.voice_params[1][2] = norm;
                        break;
                    case 2U: /* Closed <-> Open Hat Decay */
                        ui->dsp.voice_params[2][1] = norm;
                        break;
                    case 3U: /* FM Perc Index & Wavefold */
                        ui->dsp.voice_params[3][3] = norm;
                        break;
                    default:
                        break;
                    }
                    sync_legacy_voice_mirrors(&ui->dsp);
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                }
                break;
            }

            case SP1_UI_MODE_PERF: {
                /* F1–F4 drive Master FX whether or not T1–T4 stutter is held! */
                switch (f) {
                case 0U: /* F1: DJ Resonant Filter Isolator */
                    ui->dsp.dj_filter_pos = norm;
                    set_meter_bipolar(ui, sp1_ui_fader_norm_to_bipolar(norm),
                                      SP1_UI_PARAM_METER_FADER_MS);
                    break;
                case 1U: /* F2: Bitcrusher & ZOH Decimator */
                    ui->dsp.crush_amount = norm;
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                case 2U: { /* F3: Dub Delay Duration (Tape Pitch Glide -> Division Lock) */
                    ui->dsp.dub_delay_time = norm;
                    ui->dsp.dub_delay_free_dragging = true;
                    uint32_t div = (uint32_t)(norm * 5.0f + 0.5f);
                    if (div > 5U) {
                        div = 5U;
                    }
                    ui->dsp.dub_delay_div = (uint8_t)div;
                    ui->delay_drag_ms = SP1_UI_DELAY_DRAG_SETTLE_MS;
                    f3_delay_moved = true;
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                }
                case 3U: /* F4: Dub Ping-Pong Delay Wet & Feedback */
                    ui->dsp.dub_delay_wet = norm;
                    ui->dsp.dub_delay_feedback = 0.25f + (0.57f * norm);
                    set_meter_unbalanced(ui, norm, SP1_UI_PARAM_METER_FADER_MS);
                    break;
                default:
                    break;
                }
                break;
            }

            default:
                break;
            }
        }
    }

    /* Settle Performance F3 Tape Delay into magnetic tempo-division lock after movement stops */
    if (!f3_delay_moved && ui->delay_drag_ms > 0U) {
        if (dt_ms >= ui->delay_drag_ms) {
            ui->delay_drag_ms = 0U;
            ui->dsp.dub_delay_free_dragging = false;
            set_meter_discrete(ui, ui->dsp.dub_delay_div, 6U, SP1_UI_PARAM_METER_FADER_MS);
        } else {
            ui->delay_drag_ms -= dt_ms;
        }
    }

    ui->btn_consumed_mask &= (uint16_t)(~released);
}

/* ── Trigger Notification & Dual 4-LED Priority Renderer ─────────────────── */

void sp1_ui_mapper_on_triggers(sp1_ui_mapper_t *ui,
                               const sp1_seq_trigger_event_t *triggers,
                               uint32_t count) {
    if (ui == NULL || triggers == NULL) {
        return;
    }

    for (uint32_t i = 0U; i < count; ++i) {
        const uint8_t t = triggers[i].track;
        if (t < SP1_SEQ_NUM_TRACKS) {
            const uint8_t target =
                triggers[i].accent ? SP1_UI_LED_TRACK_HIT_ACCENT : SP1_UI_LED_TRACK_HIT_NORMAL;
            if (target > ui->track_flash[t]) {
                ui->track_flash[t] = target;
            }
        }
    }
}

static void dec_timer_u32(uint32_t *timer, uint32_t dt_ms) {
    if (*timer <= dt_ms) {
        *timer = 0U;
    } else {
        *timer -= dt_ms;
    }
}

static void render_side_leds(sp1_ui_mapper_t *ui,
                             const sp1_sequencer_t *seq,
                             uint8_t out_play[SP1_NUM_PLAY_LEDS]) {
    const uint8_t mode_idx = ((uint8_t)ui->mode < 4U) ? (uint8_t)ui->mode : 0U;

    /* Priority 1: Master Volume HUD (800 ms vertical bar on Vol-/Vol+) */
    if (ui->volume_hud_ms > 0U) {
        const float vol_norm = (float)ui->dsp.master_volume * SP1_UI_INV_127;
        sp1_ui_led_render_unbalanced(vol_norm, out_play);
        return;
    }

    /* Priority 2: Mode Change Toast (~1.2 s solid + 2x crisp blinks) */
    if (ui->mode_toast_ms > 0U) {
        const uint32_t elapsed = SP1_UI_MODE_TOAST_TOTAL_MS - ui->mode_toast_ms;
        bool lit = true;
        if (elapsed >= SP1_UI_MODE_TOAST_SOLID_MS) {
            const uint32_t blink_elapsed = elapsed - SP1_UI_MODE_TOAST_SOLID_MS;
            /* 4 phases of 120 ms: [0]=OFF, [1]=ON (blink 1), [2]=OFF, [3]=ON (blink 2) */
            uint32_t phase = 0U;
            uint32_t rem = blink_elapsed;
            while (rem >= SP1_UI_MODE_TOAST_BLINK_MS && phase < 3U) {
                rem -= SP1_UI_MODE_TOAST_BLINK_MS;
                phase++;
            }
            lit = ((phase & 1U) != 0U);
        }
        out_play[mode_idx] = lit ? 255U : 0U;
        return;
    }

    const sp1_clock_sync_t *clk = sp1_seq_clock_const(seq);
    const bool ext_mode = (sp1_clock_get_mode(clk) == SP1_CLOCK_MODE_EXTERNAL);

    /* Priority 3: Playback 4-Beat Quarter-Note Chaser */
    if (sp1_seq_is_running(seq)) {
        const uint32_t tick_idx = sp1_clock_get_tick_index(clk);
        const uint32_t cur_tick = (tick_idx > 0U) ? (tick_idx - 1U) : 0U;
        const uint32_t beat_total = div_by_24_u32(cur_tick);
        const uint32_t beat = beat_total & 3U;
        const uint32_t sub_beat = cur_tick - (beat_total * SP1_CLOCK_PPQN);

        const uint8_t floor_val = ext_mode ? 16U : 0U;
        for (uint8_t p = 0U; p < SP1_NUM_PLAY_LEDS; ++p) {
            out_play[p] = floor_val;
        }

        const uint32_t peak = (beat == 0U) ? 255U : 180U;
        const uint32_t drop = (beat == 0U) ? (sub_beat * 5U) : (sub_beat * 4U);
        const uint32_t val = (peak > drop) ? (peak - drop) : floor_val;
        out_play[beat] = (val > floor_val) ? (uint8_t)val : floor_val;
        return;
    }

    /* Priority 4: Stopped Battery Level Toast (3.0 s on boot, stop, or USB change) */
    if (ui->battery_toast_ms > 0U) {
        sp1_ui_led_render_unbalanced(ui->battery_level, out_play);
        if (out_play[0] < 32U) {
            out_play[0] = 32U; /* Always show at least dim LED 1 when awake */
        }
        if (ui->battery_charging) {
            /* Gentle 1-second triangle breathing pulse on the highest active battery LED */
            uint8_t top = 0U;
            for (uint8_t i = 0U; i < SP1_NUM_PLAY_LEDS; ++i) {
                if (out_play[i] > 0U) {
                    top = i;
                }
            }
            const uint32_t cyc = ui->anim_phase_ms & 1023U;
            const uint32_t tri = (cyc < 512U) ? (cyc >> 1U) : ((1023U - cyc) >> 1U);
            out_play[top] = (uint8_t)(32U + ((tri * 200U) >> 8U));
        }
        return;
    }

    /* Priority 5: Stopped Resting State — Dim Glow on Active Mode's Side LED */
    const uint8_t floor_val = ext_mode ? 12U : 0U;
    for (uint8_t p = 0U; p < SP1_NUM_PLAY_LEDS; ++p) {
        out_play[p] = floor_val;
    }
    out_play[mode_idx] = ext_mode ? 80U : SP1_UI_LED_SIDE_REST_MODE;
}

static void render_track_leds(const sp1_ui_mapper_t *ui,
                              const sp1_sequencer_t *seq,
                              uint8_t out_track[SP1_NUM_TRACK_LEDS]) {
    /* Priority 1: Temporary Parameter Meter Overlay */
    if (ui->meter_ms > 0U && ui->meter_style != SP1_LED_METER_NONE) {
        switch (ui->meter_style) {
        case SP1_LED_METER_UNBALANCED:
            sp1_ui_led_render_unbalanced(ui->meter_norm, out_track);
            return;
        case SP1_LED_METER_MIDDLE_BALANCED:
            sp1_ui_led_render_middle_balanced(ui->meter_bipolar, out_track);
            return;
        case SP1_LED_METER_BASE2:
            sp1_ui_led_render_base2(ui->meter_int, out_track);
            return;
        case SP1_LED_METER_DISCRETE:
            sp1_ui_led_render_discrete(ui->meter_slot, ui->meter_num_slots, out_track);
            return;
        case SP1_LED_METER_NONE:
        default:
            break;
        }
    }

    /* Priority 2: Live Dub Siren LFO Wave Animation (when no fader meter is active) */
    if (ui->siren_active) {
        const float pos = ui->siren_lfo_vis * 3.0f; /* [0.0f..3.0f] across Track LEDs 0..3 */
        for (uint8_t t = 0U; t < SP1_NUM_TRACK_LEDS; ++t) {
            float dist = pos - (float)t;
            if (dist < 0.0f) {
                dist = -dist;
            }
            if (dist < 1.15f) {
                const float br = (1.15f - dist) * 220.0f;
                out_track[t] = (br >= 255.0f) ? 255U : (uint8_t)br;
            } else {
                out_track[t] = 16U;
            }
        }
        return;
    }

    /* Priority 3 & 4: Live Drum Trigger Flashes + Momentary Stutter Pulse */
    for (uint8_t t = 0U; t < SP1_NUM_TRACK_LEDS; ++t) {
        if (sp1_seq_is_track_muted(seq, t)) {
            out_track[t] = SP1_UI_LED_TRACK_MUTE_DIM;
        } else {
            const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, t);
            const uint8_t base =
                (pat != NULL && pat->pulses > 0U) ? SP1_UI_LED_TRACK_ACTIVE_IDLE : 0U;
            out_track[t] = (ui->track_flash[t] > base) ? ui->track_flash[t] : base;
        }
    }

    if (ui->mode == SP1_UI_MODE_PERF && ui->dsp.stutter_div >= 2U && ui->dsp.stutter_div <= 5U) {
        const uint8_t st_trk = (uint8_t)(ui->dsp.stutter_div - 2U); /* 2..5 -> Track 0..3 */
        const uint32_t period_Shift = (uint32_t)(8U - ui->dsp.stutter_div); /* faster for 1/64 */
        const bool pulse_on = (((ui->anim_phase_ms >> period_Shift) & 1U) == 0U);
        out_track[st_trk] = pulse_on ? 255U : 64U;
    }
}

void sp1_ui_mapper_render_leds(sp1_ui_mapper_t *ui,
                               const sp1_sequencer_t *seq,
                               uint32_t dt_ms,
                               sp1_led_frame_t *out_frame) {
    if (ui == NULL || seq == NULL || out_frame == NULL) {
        return;
    }

    memset(out_frame, 0, sizeof(*out_frame));

    ui->anim_phase_ms += dt_ms;

    /* Advance per-track hit flash decay (~85 ms full decay from 255 -> 0) */
    const uint32_t decay = dt_ms * 3U;
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        if (decay >= (uint32_t)ui->track_flash[t]) {
            ui->track_flash[t] = 0U;
        } else {
            ui->track_flash[t] = (uint8_t)((uint32_t)ui->track_flash[t] - decay);
        }
    }

    render_side_leds(ui, seq, out_frame->play);
    render_track_leds(ui, seq, out_frame->track);

    /* Advance transient HUD / Toast / Meter countdowns AFTER rendering current frame
     * so newly triggered overlays are always visible on their first rendered frame. */
    dec_timer_u32(&ui->volume_hud_ms, dt_ms);
    dec_timer_u32(&ui->mode_toast_ms, dt_ms);
    if (!sp1_seq_is_running(seq)) {
        dec_timer_u32(&ui->battery_toast_ms, dt_ms);
    } else {
        ui->battery_toast_ms = 0U;
    }
    dec_timer_u32(&ui->meter_ms, dt_ms);
}

/* ── Function (P0.27) Power Hold Debounce State ──────────────────────────── */

void sp1_ui_power_hold_init(sp1_ui_power_hold_t *state, bool wait_for_initial_release) {
    if (state == NULL) {
        return;
    }
    state->hold_elapsed_ms = 0U;
    state->release_bounce_ms = 0U;
    state->holding = false;
    state->wait_for_release = wait_for_initial_release;
}

bool sp1_ui_power_hold_update(sp1_ui_power_hold_t *state,
                              bool fn_pressed,
                              uint32_t dt_ms,
                              uint32_t target_hold_ms,
                              uint32_t release_debounce_ms) {
    if (state == NULL) {
        return false;
    }

    if (state->wait_for_release) {
        if (fn_pressed) {
            state->release_bounce_ms = 0U;
        } else {
            state->release_bounce_ms += dt_ms;
            if (state->release_bounce_ms >= release_debounce_ms) {
                state->wait_for_release = false;
                state->holding = false;
                state->hold_elapsed_ms = 0U;
                state->release_bounce_ms = 0U;
            }
        }
        return false;
    }

    if (fn_pressed) {
        if (!state->holding) {
            state->holding = true;
            state->hold_elapsed_ms = 0U;
            state->release_bounce_ms = 0U;
        } else {
            if (state->release_bounce_ms > 0U) {
                state->hold_elapsed_ms += state->release_bounce_ms;
                state->release_bounce_ms = 0U;
            }
            state->hold_elapsed_ms += dt_ms;
        }
        return (state->hold_elapsed_ms >= target_hold_ms);
    }

    if (state->holding) {
        state->release_bounce_ms += dt_ms;
        if (state->release_bounce_ms >= release_debounce_ms) {
            state->holding = false;
            state->hold_elapsed_ms = 0U;
            state->release_bounce_ms = 0U;
        }
    }
    return false;
}
