#include "core/ui_mapper.h"

#include <stddef.h>
#include <string.h>

#define SP1_UI_NUDGE_INV_SPAN   (1.0f / SP1_UI_NUDGE_DEADZONE_LOW)

static const sp1_button_id_t k_track_buttons[SP1_SEQ_NUM_TRACKS] = {
    SP1_BTN_TRACK_1,
    SP1_BTN_TRACK_2,
    SP1_BTN_TRACK_3,
    SP1_BTN_TRACK_4,
};

static int32_t abs_diff_u16(uint16_t a, uint16_t b) {
    return (a >= b) ? (int32_t)(a - b) : (int32_t)(b - a);
}

static sp1_ui_modifier_t resolve_hardware_safe_modifier(uint16_t buttons_down) {
    /* Mask strictly to SP1_BTN_VALID_SHIFT_MASK (excludes SP1_BTN_PLAY on AIN0). */
    const uint16_t shift_bits = (uint16_t)(buttons_down & SP1_BTN_VALID_SHIFT_MASK);

    if ((shift_bits & SP1_BTN_BIT(SP1_BTN_FN)) != 0U) {
        return SP1_UI_MOD_FN;
    }
    if ((shift_bits & SP1_BTN_BIT(SP1_BTN_RWD)) != 0U) {
        return SP1_UI_MOD_RWD;
    }
    if ((shift_bits & SP1_BTN_BIT(SP1_BTN_FWD)) != 0U) {
        return SP1_UI_MOD_FWD;
    }
    return SP1_UI_MOD_NONE;
}

static void latch_faders_for_mode(sp1_ui_mapper_t *ui, const sp1_controls_snapshot_t *snap) {
    for (uint8_t i = 0U; i < SP1_NUM_FADERS; ++i) {
        ui->mode_entry_raw[i] = snap->fader_raw[i];
        ui->fader_engaged[i] = false;
    }
}

static void adjust_bpm_clamped(sp1_sequencer_t *seq, int32_t delta_x100) {
    const int32_t cur = (int32_t)sp1_seq_get_bpm_x100(seq);
    int32_t next = cur + delta_x100;
    if (next < (int32_t)SP1_UI_BPM_MIN_X100) {
        next = (int32_t)SP1_UI_BPM_MIN_X100;
    } else if (next > (int32_t)SP1_UI_BPM_MAX_X100) {
        next = (int32_t)SP1_UI_BPM_MAX_X100;
    }
    sp1_seq_set_bpm_x100(seq, (uint32_t)next);
}

static void adjust_swing_clamped(sp1_sequencer_t *seq, float delta) {
    sp1_seq_set_swing(seq, sp1_seq_get_swing(seq) + delta);
}

static void adjust_volume_clamped(sp1_ui_mapper_t *ui, int32_t delta) {
    int32_t next = (int32_t)ui->dsp.master_volume + delta;
    if (next < (int32_t)SP1_UI_VOLUME_MIN) {
        next = (int32_t)SP1_UI_VOLUME_MIN;
    } else if (next > (int32_t)SP1_UI_VOLUME_MAX) {
        next = (int32_t)SP1_UI_VOLUME_MAX;
    }
    ui->dsp.master_volume = (uint8_t)next;
}

static uint32_t div_by_24_u32(uint32_t v) {
    /* Exact floor(v / 24) without hardware division: 2^36 / 24 = 0xAAAAAAAB. */
    return (uint32_t)(((uint64_t)v * 0xAAAAAAABULL) >> 36U);
}

static void render_bar_4led(uint32_t scaled_0_to_1020, uint8_t out_play[SP1_NUM_PLAY_LEDS]) {
    uint32_t rem = (scaled_0_to_1020 > 1020U) ? 1020U : scaled_0_to_1020;
    for (uint8_t i = 0U; i < SP1_NUM_PLAY_LEDS; ++i) {
        if (rem >= 255U) {
            out_play[i] = 255U;
            rem -= 255U;
        } else {
            out_play[i] = (uint8_t)rem;
            rem = 0U;
        }
    }
}

static uint8_t map_fader_norm_to_stutter_div(float norm) {
    if (norm < 0.15f) {
        return 0U; /* SP1_STUTTER_OFF */
    }
    if (norm < 0.32f) {
        return 1U; /* SP1_STUTTER_DIV_1_4 */
    }
    if (norm < 0.50f) {
        return 2U; /* SP1_STUTTER_DIV_1_8 */
    }
    if (norm < 0.68f) {
        return 3U; /* SP1_STUTTER_DIV_1_16 */
    }
    if (norm < 0.84f) {
        return 4U; /* SP1_STUTTER_DIV_1_32 */
    }
    return 5U;     /* SP1_STUTTER_DIV_1_64 */
}

void sp1_ui_mapper_init(sp1_ui_mapper_t *ui) {
    if (ui == NULL) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->active_mod = SP1_UI_MOD_NONE;
    ui->focused_track = 0U;
    ui->initialized = false;
    ui->fx_mode_active = false;
    ui->fader_stutter_div = 0U;
    ui->btn_consumed_mask = 0U;

    /* Default DSP voice, master volume & performance FX targets */
    ui->dsp.dj_filter_pos = 0.50f;
    ui->dsp.dj_resonance_q = 1.85f;
    ui->dsp.crush_amount = 0.0f;
    ui->dsp.dub_delay_wet = 0.22f;
    ui->dsp.dub_delay_feedback = 0.45f;
    ui->dsp.kick_decay = 0.45f;
    ui->dsp.kick_drive = 0.35f;
    ui->dsp.snare_morph = 0.65f;
    ui->dsp.hat_decay = 0.20f;
    ui->dsp.perc_timbre = 0.45f;
    ui->dsp.stutter_div = 0U;
    ui->dsp.master_volume = SP1_UI_VOLUME_DEFAULT;
}

void sp1_ui_mapper_set_fx_mode(sp1_ui_mapper_t *ui,
                               const sp1_controls_snapshot_t *snap,
                               bool enabled) {
    if (ui == NULL) {
        return;
    }
    if (ui->fx_mode_active != enabled) {
        ui->fx_mode_active = enabled;
        if (!enabled) {
            ui->fader_stutter_div = 0U;
            ui->dsp.stutter_div = 0U;
        }
        if (snap != NULL) {
            latch_faders_for_mode(ui, snap);
        } else {
            for (uint8_t i = 0U; i < SP1_NUM_FADERS; ++i) {
                ui->fader_engaged[i] = false;
            }
        }
    }
}

bool sp1_ui_mapper_is_fx_mode(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->fx_mode_active : false;
}

const sp1_ui_dsp_params_t *sp1_ui_mapper_get_dsp_params(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? &ui->dsp : NULL;
}

uint8_t sp1_ui_mapper_get_master_volume(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->dsp.master_volume : SP1_UI_VOLUME_DEFAULT;
}

float sp1_ui_fader_norm_to_nudge_ms(float norm) {
    if (norm < 0.0f) {
        norm = 0.0f;
    } else if (norm > 1.0f) {
        norm = 1.0f;
    }

    if (norm >= SP1_UI_NUDGE_DEADZONE_LOW && norm <= SP1_UI_NUDGE_DEADZONE_HIGH) {
        return 0.0f;
    }

    if (norm < SP1_UI_NUDGE_DEADZONE_LOW) {
        const float t = (SP1_UI_NUDGE_DEADZONE_LOW - norm) * SP1_UI_NUDGE_INV_SPAN;
        return -t * SP1_SEQ_NUDGE_MAX_MS;
    }

    const float t = (norm - SP1_UI_NUDGE_DEADZONE_HIGH) * SP1_UI_NUDGE_INV_SPAN;
    return t * SP1_SEQ_NUDGE_MAX_MS;
}

void sp1_ui_mapper_update(sp1_ui_mapper_t *ui,
                          sp1_sequencer_t *seq,
                          const sp1_controls_snapshot_t *snap) {
    if (ui == NULL || seq == NULL || snap == NULL) {
        return;
    }

    const uint16_t down = snap->buttons_down;
    const uint16_t pressed = snap->buttons_pressed;
    const uint16_t released = snap->buttons_released;
    const bool play_held = (down & SP1_BTN_BIT(SP1_BTN_PLAY)) != 0U;
    const bool fn_held = (down & SP1_BTN_BIT(SP1_BTN_FN)) != 0U;
    const bool any_valid_shift = (down & SP1_BTN_VALID_SHIFT_MASK) != 0U;

    const sp1_ui_modifier_t next_mod =
        play_held ? SP1_UI_MOD_NONE : resolve_hardware_safe_modifier(down);

    /* Freshly pressed buttons start unconsumed. If Play, Function, FX mode, or any shift
     * is active, mark participating held buttons consumed so release never fires a stray tap. */
    ui->btn_consumed_mask &= (uint16_t)(~pressed);
    if (play_held) {
        ui->btn_consumed_mask |= down;
    }
    if (fn_held) {
        ui->btn_consumed_mask |= (uint16_t)(down & (uint16_t)(~SP1_BTN_BIT(SP1_BTN_FN)));
    }
    if (any_valid_shift || ui->fx_mode_active) {
        ui->btn_consumed_mask |= (uint16_t)(down & SP1_BTN_TRACK_MASK);
    }

    if (!ui->initialized) {
        ui->initialized = true;
        ui->active_mod = next_mod;
        /* Keep initial mode_entry_raw at 0 so untouched faders preserve default preset,
         * while any fader already moved prior to the first poll engages immediately. */
    } else if (next_mod != ui->active_mod) {
        ui->active_mod = next_mod;
        latch_faders_for_mode(ui, snap);
    }

    /* Per-track anti-clobber fader latching when pressing or releasing T1..T4 in Normal
     * Pattern Mode so Hold T1..T4 + Fader 1..4 morphs voice timbre without jumping pulses. */
    if (!play_held && ui->active_mod == SP1_UI_MOD_NONE && !ui->fx_mode_active) {
        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            const uint16_t t_bit = SP1_BTN_BIT(k_track_buttons[t]);
            if (((pressed | released) & t_bit) != 0U) {
                ui->mode_entry_raw[t] = snap->fader_raw[t];
                ui->fader_engaged[t] = false;
            }
        }
    }

    /* 0. Hardware-Safe Performance FX Mode Toggle (never uses Play on AIN0):
     *    - Function (GPIO P0.27) + T1 + T4 (AIN0), OR
     *    - Vol- (AIN1) + T1 (AIN0) */
    if (!play_held) {
        const uint16_t t1_t4_mask =
            (uint16_t)(SP1_BTN_BIT(SP1_BTN_TRACK_1) | SP1_BTN_BIT(SP1_BTN_TRACK_4));
        const bool fn_t1_t4_chord =
            fn_held &&
            ((down & t1_t4_mask) == t1_t4_mask) &&
            ((pressed & t1_t4_mask) != 0U);
        const bool vol_down_t1_chord =
            sp1_controls_is_valid_shift_chord(down, SP1_BTN_VOL_DOWN, SP1_BTN_TRACK_1) &&
            ((pressed & SP1_BTN_BIT(SP1_BTN_TRACK_1)) != 0U);

        if (fn_t1_t4_chord || vol_down_t1_chord) {
            /* Suppress release taps (track mutes / volume step) for every button in the chord. */
            ui->btn_consumed_mask |= down;
            ui->btn_consumed_mask &= (uint16_t)(~released);
            sp1_ui_mapper_set_fx_mode(ui, snap, !ui->fx_mode_active);
            return;
        }
    }

    /* 1. Play button: transport toggle ONLY (never a modifier; ignored only if Function held). */
    if ((pressed & SP1_BTN_BIT(SP1_BTN_PLAY)) != 0U && !fn_held) {
        if (sp1_seq_is_running(seq)) {
            sp1_seq_stop(seq);
        } else {
            sp1_seq_start(seq);
        }
    }

    /* 2. Transport / Volume rockers (AIN1) + Function (P0.27) chords:
     *    - RWD / FWD (unshifted tap, no fader/chord used): Fine Master BPM (-2 / +2 BPM, 60..180)
     *    - Function + RWD / FWD:                           Coarse Master BPM (-10 / +10 BPM, 60..180)
     *    - Function + Vol- / Vol+:                         Global Swing (-2% / +2%, 50%..66%)
     *    - Vol- / Vol+ (unshifted tap):                    Master Output Volume (0..127, step +/-8)
     *      Note: Vol-/Vol+ share the single-press AIN1 resistor ladder with RWD/FWD, so
     *      releasing Vol+/Vol- must not be suppressed if the decaying ladder voltage
     *      briefly transits RWD/FWD (and any such transient RWD/FWD bit is marked consumed). */
    if (ui->active_mod == SP1_UI_MOD_FN) {
        if ((pressed & SP1_BTN_BIT(SP1_BTN_RWD)) != 0U) {
            adjust_bpm_clamped(seq, -(int32_t)SP1_UI_BPM_COARSE_STEP_X100);
            ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_RWD);
        }
        if ((pressed & SP1_BTN_BIT(SP1_BTN_FWD)) != 0U) {
            adjust_bpm_clamped(seq, (int32_t)SP1_UI_BPM_COARSE_STEP_X100);
            ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_FWD);
        }
        if ((pressed & SP1_BTN_BIT(SP1_BTN_VOL_DOWN)) != 0U) {
            adjust_swing_clamped(seq, -SP1_UI_SWING_STEP);
            ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_VOL_DOWN);
        }
        if ((pressed & SP1_BTN_BIT(SP1_BTN_VOL_UP)) != 0U) {
            adjust_swing_clamped(seq, SP1_UI_SWING_STEP);
            ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_VOL_UP);
        }
    } else if (!play_held && !fn_held) {
        const bool vol_up_pressed =
            (pressed & SP1_BTN_BIT(SP1_BTN_VOL_UP)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_VOL_UP)) == 0U;
        const bool vol_up_released =
            (released & SP1_BTN_BIT(SP1_BTN_VOL_UP)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_VOL_UP)) == 0U;
        const bool vol_down_released =
            (released & SP1_BTN_BIT(SP1_BTN_VOL_DOWN)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_VOL_DOWN)) == 0U;

        if (((pressed | released) &
             (SP1_BTN_BIT(SP1_BTN_VOL_UP) | SP1_BTN_BIT(SP1_BTN_VOL_DOWN))) != 0U) {
            /* Suppress any transient RWD/FWD sample on the shared AIN1 ladder during Vol+/Vol- rise/fall. */
            ui->btn_consumed_mask |= (uint16_t)(SP1_BTN_BIT(SP1_BTN_RWD) |
                                                SP1_BTN_BIT(SP1_BTN_FWD));
        }

        if (vol_up_pressed) {
            adjust_volume_clamped(ui, (int32_t)SP1_UI_VOLUME_STEP);
            ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_VOL_UP);
        } else if (vol_up_released) {
            adjust_volume_clamped(ui, (int32_t)SP1_UI_VOLUME_STEP);
        }

        if (vol_down_released) {
            adjust_volume_clamped(ui, -(int32_t)SP1_UI_VOLUME_STEP);
        }

        if ((released & SP1_BTN_BIT(SP1_BTN_RWD)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_RWD)) == 0U) {
            adjust_bpm_clamped(seq, -(int32_t)SP1_UI_BPM_FINE_STEP_X100);
        }
        if ((released & SP1_BTN_BIT(SP1_BTN_FWD)) != 0U &&
            (ui->btn_consumed_mask & SP1_BTN_BIT(SP1_BTN_FWD)) == 0U) {
            adjust_bpm_clamped(seq, (int32_t)SP1_UI_BPM_FINE_STEP_X100);
        }
    }

    /* 3. Track buttons T1..T4 (AIN0):
     *    - Suppressed whenever SP1_BTN_PLAY is asserted (AIN0 hardware ladder safety).
     *    - Valid shift chords (FN/RWD/FWD + T1..T4) validated via
     *      sp1_controls_is_valid_shift_chord() perform per-track quick resets and
     *      re-latch that track's fader so its parked position cannot overwrite the reset.
     *    - In Performance FX Mode (unshifted), holding T1..T4 punches in momentary
     *      tempo-synced Stutter divisions (1/8, 1/16, 1/32, 1/64).
     *    - In Normal Pattern Mode (unshifted), holding T1..T4 + moving Fader 1..4 morphs
     *      voice timbre; tapping T1..T4 without moving a fader toggles track mute on release. */
    if (!play_held) {
        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            const sp1_button_id_t trk_btn = k_track_buttons[t];
            const uint16_t t_bit = SP1_BTN_BIT(trk_btn);

            if ((pressed & t_bit) != 0U) {
                if (sp1_controls_is_valid_shift_chord(down, SP1_BTN_FN, trk_btn)) {
                    sp1_seq_set_track_nudge_ms(seq, t, 0.0f);
                    ui->focused_track = t;
                    ui->mode_entry_raw[t] = snap->fader_raw[t];
                    ui->fader_engaged[t] = false;
                    ui->btn_consumed_mask |= t_bit;
                } else if (sp1_controls_is_valid_shift_chord(down, SP1_BTN_RWD, trk_btn)) {
                    sp1_seq_set_track_rotation(seq, t, 0U);
                    ui->focused_track = t;
                    ui->mode_entry_raw[t] = snap->fader_raw[t];
                    ui->fader_engaged[t] = false;
                    ui->btn_consumed_mask |= (uint16_t)(t_bit | SP1_BTN_BIT(SP1_BTN_RWD));
                } else if (sp1_controls_is_valid_shift_chord(down, SP1_BTN_FWD, trk_btn)) {
                    sp1_seq_set_track_steps(seq, t, 16U);
                    ui->focused_track = t;
                    ui->mode_entry_raw[t] = snap->fader_raw[t];
                    ui->fader_engaged[t] = false;
                    ui->btn_consumed_mask |= (uint16_t)(t_bit | SP1_BTN_BIT(SP1_BTN_FWD));
                }
            }

            if ((released & t_bit) != 0U &&
                !any_valid_shift &&
                !ui->fx_mode_active &&
                (ui->btn_consumed_mask & t_bit) == 0U) {
                sp1_seq_toggle_track_mute(seq, t);
            }
        }
    }

    /* Update momentary vs. fader-latched Stutter division in Performance FX Mode */
    if (ui->fx_mode_active && !play_held && !any_valid_shift) {
        uint8_t mom_div = 0U;
        if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_4)) != 0U) {
            mom_div = 5U; /* 1/64 ratchet */
        } else if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_3)) != 0U) {
            mom_div = 4U; /* 1/32 ratchet */
        } else if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_2)) != 0U) {
            mom_div = 3U; /* 1/16 repeat */
        } else if ((down & SP1_BTN_BIT(SP1_BTN_TRACK_1)) != 0U) {
            mom_div = 2U; /* 1/8 repeat */
        }
        ui->dsp.stutter_div = (mom_div != 0U) ? mom_div : ui->fader_stutter_div;
    }

    /* 4. Faders 1..4 (SAADC AIN3/6/2/7):
     *    While Play is held, continuously re-latch fader positions so Play + Fader
     *    never applies edits during or after releasing Play. Once engaged, only apply
     *    when raw fader value actually changes. */
    if (play_held) {
        latch_faders_for_mode(ui, snap);
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

            float norm = snap->fader_norm[f];
            if (norm < 0.0f) {
                norm = 0.0f;
            } else if (norm > 1.0f) {
                norm = 1.0f;
            }

            const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, f);
            const uint8_t cur_steps = (pat != NULL) ? pat->steps : 16U;

            switch (ui->active_mod) {
            case SP1_UI_MOD_NONE: {
                const uint16_t trk_bit = SP1_BTN_BIT(k_track_buttons[f]);
                const bool track_morph_held =
                    !any_valid_shift && !ui->fx_mode_active && ((down & trk_bit) != 0U);

                if (track_morph_held) {
                    /* Hold T1..T4 + Fader 1..4: Per-Voice Timbre & Morph */
                    switch (f) {
                    case 0U:
                        ui->dsp.kick_decay = norm;
                        ui->dsp.kick_drive = 0.20f + (0.75f * norm);
                        break;
                    case 1U:
                        /* Track 2 continuous Rimshot (0.0) <-> Snare (1.0) morph */
                        ui->dsp.snare_morph = norm;
                        break;
                    case 2U:
                        /* Track 3 continuous Closed (0.0) <-> Open (1.0) Hi-Hat morph */
                        ui->dsp.hat_decay = norm;
                        break;
                    case 3U:
                        /* Track 4 2-Op FM index & West-Coast wavefold morph */
                        ui->dsp.perc_timbre = norm;
                        break;
                    default:
                        break;
                    }
                    ui->focused_track = f;
                    ui->btn_consumed_mask |= (uint16_t)(down & SP1_BTN_TRACK_MASK);
                } else if (ui->fx_mode_active) {
                    /* Latched Performance FX Mode: Faders 1..4 drive Master FX Bus */
                    switch (f) {
                    case 0U:
                        ui->dsp.dj_filter_pos = norm;
                        break;
                    case 1U:
                        ui->fader_stutter_div = map_fader_norm_to_stutter_div(norm);
                        ui->dsp.stutter_div = ui->fader_stutter_div;
                        break;
                    case 2U:
                        ui->dsp.crush_amount = norm;
                        break;
                    case 3U:
                        ui->dsp.dub_delay_wet = norm;
                        ui->dsp.dub_delay_feedback = 0.25f + (0.55f * norm);
                        break;
                    default:
                        break;
                    }
                } else {
                    const uint8_t pulses = (uint8_t)(norm * (float)cur_steps + 0.5f);
                    sp1_seq_set_track_pulses(seq, f, pulses);
                    if ((down & SP1_BTN_TRACK_MASK) != 0U) {
                        ui->btn_consumed_mask |= (uint16_t)(down & SP1_BTN_TRACK_MASK);
                    }
                }
                break;
            }
            case SP1_UI_MOD_FN: {
                const float nudge_ms = sp1_ui_fader_norm_to_nudge_ms(norm);
                sp1_seq_set_track_nudge_ms(seq, f, nudge_ms);
                ui->focused_track = f;
                break;
            }
            case SP1_UI_MOD_RWD: {
                const uint8_t max_rot = (cur_steps > 1U) ? (uint8_t)(cur_steps - 1U) : 0U;
                const uint8_t rot = (uint8_t)(norm * (float)max_rot + 0.5f);
                sp1_seq_set_track_rotation(seq, f, rot);
                ui->focused_track = f;
                ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_RWD);
                break;
            }
            case SP1_UI_MOD_FWD: {
                const uint8_t steps =
                    (uint8_t)(SP1_EUCLID_MIN_STEPS +
                              (uint8_t)(norm * (float)(SP1_EUCLID_MAX_STEPS - SP1_EUCLID_MIN_STEPS) +
                                        0.5f));
                sp1_seq_set_track_steps(seq, f, steps);
                ui->focused_track = f;
                ui->btn_consumed_mask |= SP1_BTN_BIT(SP1_BTN_FWD);
                break;
            }
            default:
                break;
            }
        }
    }

    /* Clear consumed flags for buttons that just finished releasing. */
    ui->btn_consumed_mask &= (uint16_t)(~released);
}

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

void sp1_ui_mapper_render_leds(sp1_ui_mapper_t *ui,
                               const sp1_sequencer_t *seq,
                               uint32_t dt_ms,
                               sp1_led_frame_t *out_frame) {
    if (ui == NULL || seq == NULL || out_frame == NULL) {
        return;
    }

    memset(out_frame, 0, sizeof(*out_frame));

    /* Advance per-track hit flash decay (~85 ms full decay from 255 -> 0). */
    const uint32_t decay = dt_ms * 3U;
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        if (decay >= (uint32_t)ui->track_flash[t]) {
            ui->track_flash[t] = 0U;
        } else {
            ui->track_flash[t] = (uint8_t)((uint32_t)ui->track_flash[t] - decay);
        }
    }

    /* ── Shift Overlay Modes (FN = Dilla Nudge Polarity; RWD/FWD = Bar Meter) ── */
    if (ui->active_mod != SP1_UI_MOD_NONE) {
        const uint8_t focus =
            (ui->focused_track < SP1_SEQ_NUM_TRACKS) ? ui->focused_track : 0U;

        for (uint8_t t = 0U; t < SP1_NUM_TRACK_LEDS; ++t) {
            out_frame->track[t] =
                (t == focus) ? SP1_UI_LED_SHIFT_FOCUS : SP1_UI_LED_SHIFT_OTHER;
        }

        if (ui->active_mod == SP1_UI_MOD_FN) {
            const int16_t nudge = sp1_seq_get_track_nudge_frames(seq, focus);
            if (nudge == 0) {
                /* Exact 0 ms grid lock: symmetric center pair. */
                out_frame->play[1] = SP1_UI_LED_NUDGE_CENTER;
                out_frame->play[2] = SP1_UI_LED_NUDGE_CENTER;
            } else if (nudge < 0) {
                /* Rushed / early (< 0 ms): left LEDs [1] then [0] illuminate. */
                const uint32_t mag = (uint32_t)(-(int32_t)nudge);
                uint32_t scaled = (mag * 19900U) >> 16U;
                if (scaled < 48U) {
                    scaled = 48U;
                }
                if (scaled > 510U) {
                    scaled = 510U;
                }
                out_frame->play[1] = (scaled >= 255U) ? 255U : (uint8_t)scaled;
                out_frame->play[0] = (scaled > 255U) ? (uint8_t)(scaled - 255U) : 0U;
            } else {
                /* Dragged / late (> 0 ms): right LEDs [2] then [3] illuminate. */
                const uint32_t mag = (uint32_t)nudge;
                uint32_t scaled = (mag * 19900U) >> 16U;
                if (scaled < 48U) {
                    scaled = 48U;
                }
                if (scaled > 510U) {
                    scaled = 510U;
                }
                out_frame->play[2] = (scaled >= 255U) ? 255U : (uint8_t)scaled;
                out_frame->play[3] = (scaled > 255U) ? (uint8_t)(scaled - 255U) : 0U;
            }
        } else if (ui->active_mod == SP1_UI_MOD_FWD) {
            /* Polymeter length meter (1..32 steps across 4 LEDs, 32 * 32 - 4 ~= 1020). */
            const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, focus);
            const uint32_t steps = (pat != NULL) ? (uint32_t)pat->steps : 16U;
            render_bar_4led(steps * 32U, out_frame->play);
        } else {
            /* Rotation meter (0..n-1 scaled across 4 LEDs). */
            const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, focus);
            const uint32_t rot = (pat != NULL) ? (uint32_t)pat->rotation : 0U;
            render_bar_4led(rot * 32U, out_frame->play);
        }
        return;
    }

    /* ── Normal Mode: Track Pulse / Mute Dim + Play Beat Clock Indicator ───── */
    for (uint8_t t = 0U; t < SP1_NUM_TRACK_LEDS; ++t) {
        if (sp1_seq_is_track_muted(seq, t)) {
            out_frame->track[t] = SP1_UI_LED_TRACK_MUTE_DIM;
        } else {
            const sp1_euclid_pattern_t *pat = sp1_seq_get_track_pattern(seq, t);
            const uint8_t base =
                (pat != NULL && pat->pulses > 0U) ? SP1_UI_LED_TRACK_ACTIVE_IDLE : 0U;
            out_frame->track[t] =
                (ui->track_flash[t] > base) ? ui->track_flash[t] : base;
        }
    }

    const sp1_clock_sync_t *clk = sp1_seq_clock_const(seq);
    const bool ext_mode = (sp1_clock_get_mode(clk) == SP1_CLOCK_MODE_EXTERNAL);

    if (sp1_seq_is_running(seq)) {
        const uint32_t tick_idx = sp1_clock_get_tick_index(clk);
        const uint32_t cur_tick = (tick_idx > 0U) ? (tick_idx - 1U) : 0U;
        const uint32_t beat_total = div_by_24_u32(cur_tick);
        const uint32_t beat = beat_total & 3U;
        const uint32_t sub_beat = cur_tick - (beat_total * SP1_CLOCK_PPQN);

        const uint8_t floor_val = ext_mode ? 16U : 0U;
        for (uint8_t p = 0U; p < SP1_NUM_PLAY_LEDS; ++p) {
            out_frame->play[p] = floor_val;
        }

        const uint32_t peak = (beat == 0U) ? 255U : 180U;
        const uint32_t drop = (beat == 0U) ? (sub_beat * 5U) : (sub_beat * 4U);
        const uint32_t val = (peak > drop) ? (peak - drop) : floor_val;
        out_frame->play[beat] = (val > floor_val) ? (uint8_t)val : floor_val;
    } else {
        out_frame->play[0] = ext_mode ? 80U : 40U;
    }
}

sp1_ui_modifier_t sp1_ui_mapper_active_modifier(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->active_mod : SP1_UI_MOD_NONE;
}

uint8_t sp1_ui_mapper_focused_track(const sp1_ui_mapper_t *ui) {
    return (ui != NULL) ? ui->focused_track : 0U;
}

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
                /* Contact bounce recovered before release_debounce_ms expired:
                 * credit the elapsed time rather than resetting the hold counter. */
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
