#include "core/sequencer.h"

#include <stddef.h>
#include <string.h>

static const uint8_t k_default_gm_notes[SP1_SEQ_NUM_TRACKS] = {
    SP1_SEQ_GM_NOTE_KICK,
    SP1_SEQ_GM_NOTE_SNARE,
    SP1_SEQ_GM_NOTE_HAT,
    SP1_SEQ_GM_NOTE_PERC,
};

/* 48 frames per millisecond at 48,000 Hz (reciprocal 1/48 for ms inspection). */
#define SP1_SEQ_FRAMES_PER_MS       48.0f
#define SP1_SEQ_INV_FRAMES_PER_MS   (1.0f / 48.0f)

static void send_track_note_off(sp1_sequencer_t *seq,
                                sp1_seq_track_t *trk,
                                uint64_t frame) {
    if (!trk->midi_note_active) {
        return;
    }
    const sp1_midi_msg_t off = sp1_midi_msg_note_off(
        seq->midi_channel, trk->active_midi_note, 0U, sp1_clock_frame_to_us(frame));
    (void)sp1_hal_midi_send(&off);
    trk->midi_note_active = false;
}

static void release_all_active_notes(sp1_sequencer_t *seq, uint64_t frame) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        send_track_note_off(seq, &seq->tracks[t], frame);
    }
}

static void clear_all_lookahead_rings(sp1_sequencer_t *seq) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        seq->tracks[t].ring_count = 0U;
    }
}

static uint64_t compute_step_fire_frame(const sp1_sequencer_t *seq,
                                        uint8_t track_idx,
                                        uint32_t global_step,
                                        uint64_t nominal_grid_frame,
                                        uint32_t step_frames,
                                        uint64_t min_frame) {
    int64_t swing_frames = 0;
    /* Global swing delays 1-based even 16th steps (0-based odd indices: 1, 3, 5, ...). */
    if ((global_step & 1U) != 0U && seq->swing_offset_scale > 0.0f) {
        swing_frames = (int64_t)((float)step_frames * seq->swing_offset_scale + 0.5f);
    }

    const int64_t nudge = (int64_t)seq->tracks[track_idx].nudge_frames;
    int64_t signed_fire = (int64_t)nominal_grid_frame + swing_frames + nudge;
    if (signed_fire < (int64_t)min_frame) {
        signed_fire = (int64_t)min_frame;
    }
    return (uint64_t)signed_fire;
}

static void ring_insert_sorted(sp1_seq_track_t *trk, const sp1_seq_lookahead_event_t *evt) {
    if (trk->ring_count >= SP1_SEQ_LOOKAHEAD_CAP) {
        trk->dropped_events++;
        return;
    }

    uint8_t pos = trk->ring_count;
    while (pos > 0U && trk->ring[pos - 1U].fire_frame > evt->fire_frame) {
        trk->ring[pos] = trk->ring[pos - 1U];
        pos--;
    }
    trk->ring[pos] = *evt;
    trk->ring_count++;
}

static void ring_pop_front(sp1_seq_track_t *trk, sp1_seq_lookahead_event_t *out_evt) {
    if (trk->ring_count == 0U) {
        return;
    }
    *out_evt = trk->ring[0];
    trk->ring_count--;
    for (uint8_t i = 0U; i < trk->ring_count; ++i) {
        trk->ring[i] = trk->ring[i + 1U];
    }
}

static void ring_resort_small(sp1_seq_track_t *trk) {
    /* Insertion sort on <= 8 entries after a tempo-jump re-anchor. */
    for (uint8_t i = 1U; i < trk->ring_count; ++i) {
        const sp1_seq_lookahead_event_t key = trk->ring[i];
        uint8_t j = i;
        while (j > 0U && trk->ring[j - 1U].fire_frame > key.fire_frame) {
            trk->ring[j] = trk->ring[j - 1U];
            j--;
        }
        trk->ring[j] = key;
    }
}

static void evaluate_and_enqueue_step(sp1_sequencer_t *seq,
                                      uint32_t global_step,
                                      uint64_t nominal_grid_frame,
                                      uint32_t step_frames,
                                      uint64_t min_frame) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_track_t *trk = &seq->tracks[t];
        const uint8_t n = trk->pattern.steps;
        uint8_t local_step = trk->next_eval_step;
        if (local_step >= n) {
            local_step = 0U;
        }

        if (sp1_euclid_step_active(trk->pattern.mask, local_step)) {
            sp1_seq_lookahead_event_t evt;
            evt.fire_frame = compute_step_fire_frame(
                seq, t, global_step, nominal_grid_frame, step_frames, min_frame);
            evt.global_step = global_step;
            evt.track_step = local_step;
            evt.accent = (local_step == 0U) ? 1U : 0U;
            evt.velocity = (local_step == 0U) ? SP1_SEQ_VEL_ACCENT : SP1_SEQ_VEL_NORMAL;
            evt.reserved = 0U;

            ring_insert_sorted(trk, &evt);
        }

        trk->next_eval_step = sp1_euclid_next_step(local_step, n);
    }
}

static void reanchor_pending_step_events(sp1_sequencer_t *seq,
                                         uint32_t cur_grid_step,
                                         uint64_t tick_frame,
                                         uint32_t step_frames) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_track_t *trk = &seq->tracks[t];
        bool modified = false;

        for (uint8_t i = 0U; i < trk->ring_count; ++i) {
            /* Only re-anchor events still at/after tick_frame (positive-nudge hits or
             * stale predictions after a sudden tempo jump). Earlier negative-nudge hits
             * already fired prior to tick_frame via chronological block draining. */
            if (trk->ring[i].global_step == cur_grid_step &&
                trk->ring[i].fire_frame >= tick_frame) {
                trk->ring[i].fire_frame = compute_step_fire_frame(
                    seq, t, cur_grid_step, tick_frame, step_frames, tick_frame);
                modified = true;
            }
        }

        if (modified) {
            ring_resort_small(trk);
        }
    }
}

static void drain_events_before(sp1_sequencer_t *seq,
                                uint64_t block_start_frame,
                                uint64_t limit_frame,
                                uint32_t num_frames,
                                sp1_seq_trigger_event_t *out_triggers,
                                uint32_t max_triggers,
                                uint32_t *out_count) {
    const uint32_t step_frames = sp1_clock_get_step_frames(&seq->clock);
    uint32_t gate_frames = step_frames >> 1U;
    if (gate_frames > SP1_SEQ_MIDI_GATE_MAX_FRAMES) {
        gate_frames = SP1_SEQ_MIDI_GATE_MAX_FRAMES;
    }
    if (gate_frames < 48U) {
        gate_frames = 48U;
    }

    /* Repeatedly pick the earliest pending MIDI Note-Off or Trigger strictly < limit_frame
     * across all 4 tracks so all DSP triggers and outbound MIDI are strictly chronological. */
    for (;;) {
        uint64_t earliest_frame = limit_frame;
        int best_track = -1;
        bool best_is_note_off = false;

        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            const sp1_seq_track_t *trk = &seq->tracks[t];

            if (trk->midi_note_active && trk->midi_note_off_frame < earliest_frame) {
                earliest_frame = trk->midi_note_off_frame;
                best_track = (int)t;
                best_is_note_off = true;
            }

            if (trk->ring_count > 0U && trk->ring[0].fire_frame < earliest_frame) {
                earliest_frame = trk->ring[0].fire_frame;
                best_track = (int)t;
                best_is_note_off = false;
            }
        }

        if (best_track < 0) {
            break;
        }

        sp1_seq_track_t *trk = &seq->tracks[(uint8_t)best_track];
        if (best_is_note_off) {
            send_track_note_off(seq, trk, earliest_frame);
            continue;
        }

        sp1_seq_lookahead_event_t sched;
        ring_pop_front(trk, &sched);

        if (!trk->muted) {
            /* Cleanly release any still-held note on this drum track before retriggering. */
            if (trk->midi_note_active) {
                send_track_note_off(seq, trk, sched.fire_frame);
            }

            const sp1_midi_msg_t on = sp1_midi_msg_note_on(
                seq->midi_channel,
                trk->midi_note,
                sched.velocity,
                sp1_clock_frame_to_us(sched.fire_frame));
            (void)sp1_hal_midi_send(&on);

            trk->midi_note_active = true;
            trk->active_midi_note = trk->midi_note;
            trk->midi_note_off_frame = sched.fire_frame + (uint64_t)gate_frames;

            if (out_triggers != NULL && out_count != NULL && *out_count < max_triggers) {
                uint32_t offset = 0U;
                if (sched.fire_frame > block_start_frame) {
                    const uint64_t delta = sched.fire_frame - block_start_frame;
                    offset = (delta < (uint64_t)num_frames)
                                 ? (uint32_t)delta
                                 : (num_frames > 0U ? (num_frames - 1U) : 0U);
                }

                sp1_seq_trigger_event_t *dst = &out_triggers[*out_count];
                dst->sample_frame = sched.fire_frame;
                dst->block_offset = offset;
                dst->track = (uint8_t)best_track;
                dst->step_index = sched.track_step;
                dst->velocity = sched.velocity;
                dst->accent = (sched.accent != 0U);
                (*out_count)++;
            }
        }
    }
}

static void handle_clock_start(sp1_sequencer_t *seq, uint64_t frame) {
    release_all_active_notes(seq, frame);
    clear_all_lookahead_rings(seq);

    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        seq->tracks[t].next_eval_step = 0U;
        seq->tracks[t].playhead_step = 0U;
    }

    seq->sub_tick = 0U;
    seq->cur_grid_step = 0U;
    seq->scheduled_up_to_step = 0U;
    seq->has_scheduled_steps = false;
    seq->armed_for_start = true;
    seq->last_grid_frame = frame;
}

static void handle_clock_stop(sp1_sequencer_t *seq, uint64_t frame) {
    release_all_active_notes(seq, frame);
    clear_all_lookahead_rings(seq);

    if (seq->has_scheduled_steps) {
        /* Rewind next_eval_step so a subsequent Continue (0xFB) resumes on the next step
         * without skipping steps that were pre-queued in the flushed lookahead rings. */
        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            sp1_seq_track_t *trk = &seq->tracks[t];
            trk->next_eval_step = sp1_euclid_next_step(trk->playhead_step, trk->pattern.steps);
        }
        seq->has_scheduled_steps = false;
    }
    seq->armed_for_start = false;
}

static void handle_clock_tick(sp1_sequencer_t *seq, const sp1_clock_event_t *evt) {
    const uint64_t tick_frame = evt->sample_frame;
    const uint32_t step_frames = sp1_clock_get_step_frames(&seq->clock);

    if (seq->armed_for_start || (evt->tick_index == 0U && !seq->has_scheduled_steps)) {
        /* Tick 0 (beat 1 downbeat): evaluate Step 0 (clamped >= tick_frame so negative
         * nudge at frame 0 never underflows) AND 1-step lookahead for Step 1. */
        seq->armed_for_start = false;
        seq->sub_tick = 0U;
        seq->cur_grid_step = 0U;
        seq->last_grid_frame = tick_frame;

        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            seq->tracks[t].playhead_step = 0U;
            seq->tracks[t].next_eval_step = 0U;
        }

        evaluate_and_enqueue_step(seq, 0U, tick_frame, step_frames, tick_frame);
        evaluate_and_enqueue_step(
            seq, 1U, tick_frame + (uint64_t)step_frames, step_frames, tick_frame);

        seq->scheduled_up_to_step = 1U;
        seq->has_scheduled_steps = true;
        seq->sub_tick = 1U;
        return;
    }

    if (seq->sub_tick == 0U) {
        /* Reached a new 16th-note grid boundary (every 6 PPQN ticks). */
        if (seq->has_scheduled_steps) {
            seq->cur_grid_step++;
            for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
                sp1_seq_track_t *trk = &seq->tracks[t];
                trk->playhead_step = sp1_euclid_next_step(trk->playhead_step, trk->pattern.steps);
            }
        } else {
            /* Resuming via MIDI Continue (0xFB): schedule current step + 1-step lookahead. */
            seq->cur_grid_step++;
            for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
                sp1_seq_track_t *trk = &seq->tracks[t];
                trk->playhead_step = trk->next_eval_step;
            }
            evaluate_and_enqueue_step(seq, seq->cur_grid_step, tick_frame, step_frames, tick_frame);
            seq->scheduled_up_to_step = seq->cur_grid_step;
            seq->has_scheduled_steps = true;
        }

        seq->last_grid_frame = tick_frame;

        /* Re-anchor any still-pending events for cur_grid_step across tempo changes. */
        reanchor_pending_step_events(seq, seq->cur_grid_step, tick_frame, step_frames);

        /* 1-Step Lookahead: evaluate cur_grid_step + 1 exactly once. */
        const uint32_t lookahead_step = seq->cur_grid_step + 1U;
        if (lookahead_step > seq->scheduled_up_to_step) {
            evaluate_and_enqueue_step(seq,
                                      lookahead_step,
                                      tick_frame + (uint64_t)step_frames,
                                      step_frames,
                                      tick_frame);
            seq->scheduled_up_to_step = lookahead_step;
        }
    }

    seq->sub_tick++;
    if (seq->sub_tick >= SP1_CLOCK_TICKS_PER_16TH) {
        seq->sub_tick = 0U;
    }
}

void sp1_seq_init(sp1_sequencer_t *seq) {
    if (seq == NULL) {
        return;
    }

    memset(seq, 0, sizeof(*seq));
    sp1_clock_init(&seq->clock);

    seq->midi_channel = 0U; /* 0x90 / 0x80 */
    sp1_seq_set_swing(seq, SP1_SEQ_SWING_DEFAULT);

    /* Sensible default 4-track Euclidean kit (overridable via API or UI faders). */
    sp1_euclid_pattern_set(&seq->tracks[0].pattern, 4U, 16U, 0U);  /* Kick: four-on-floor */
    sp1_euclid_pattern_set(&seq->tracks[1].pattern, 2U, 16U, 4U);  /* Snare: 2 & 4 backbeat */
    sp1_euclid_pattern_set(&seq->tracks[2].pattern, 8U, 16U, 0U);  /* Hat: 8th notes */
    sp1_euclid_pattern_set(&seq->tracks[3].pattern, 5U, 16U, 0U);  /* Perc: E(5,16) */

    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        seq->tracks[t].midi_note = k_default_gm_notes[t];
        seq->tracks[t].nudge_frames = 0;
        seq->tracks[t].muted = false;
    }
}

void sp1_seq_start(sp1_sequencer_t *seq) {
    if (seq == NULL) {
        return;
    }
    sp1_clock_start(&seq->clock);
}

void sp1_seq_stop(sp1_sequencer_t *seq) {
    if (seq == NULL) {
        return;
    }
    handle_clock_stop(seq, seq->clock.current_frame);
    sp1_clock_stop(&seq->clock);
}

bool sp1_seq_is_running(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? sp1_clock_is_running(&seq->clock) : false;
}

void sp1_seq_set_bpm_x100(sp1_sequencer_t *seq, uint32_t bpm_x100) {
    if (seq == NULL) {
        return;
    }
    sp1_clock_set_bpm_x100(&seq->clock, bpm_x100);
}

void sp1_seq_set_bpm(sp1_sequencer_t *seq, float bpm) {
    if (seq == NULL) {
        return;
    }
    sp1_clock_set_bpm(&seq->clock, bpm);
}

uint32_t sp1_seq_get_bpm_x100(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? sp1_clock_get_bpm_x100(&seq->clock) : SP1_CLOCK_BPM_DEFAULT_X100;
}

void sp1_seq_set_swing(sp1_sequencer_t *seq, float swing_ratio) {
    if (seq == NULL) {
        return;
    }
    if (swing_ratio < SP1_SEQ_SWING_MIN) {
        swing_ratio = SP1_SEQ_SWING_MIN;
    } else if (swing_ratio > SP1_SEQ_SWING_MAX) {
        swing_ratio = SP1_SEQ_SWING_MAX;
    }
    seq->swing_ratio = swing_ratio;
    seq->swing_offset_scale = 2.0f * (swing_ratio - SP1_SEQ_SWING_MIN);
}

float sp1_seq_get_swing(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? seq->swing_ratio : SP1_SEQ_SWING_DEFAULT;
}

void sp1_seq_set_midi_channel(sp1_sequencer_t *seq, uint8_t channel_0_to_15) {
    if (seq == NULL) {
        return;
    }
    seq->midi_channel = (uint8_t)(channel_0_to_15 & 0x0FU);
}

void sp1_seq_set_track_pattern(sp1_sequencer_t *seq,
                               uint8_t track,
                               uint8_t pulses,
                               uint8_t steps,
                               uint8_t rotation) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_euclid_pattern_set(&seq->tracks[track].pattern, pulses, steps, rotation);
    if (seq->tracks[track].next_eval_step >= seq->tracks[track].pattern.steps) {
        seq->tracks[track].next_eval_step = 0U;
    }
    if (seq->tracks[track].playhead_step >= seq->tracks[track].pattern.steps) {
        seq->tracks[track].playhead_step = 0U;
    }
}

void sp1_seq_set_track_pulses(sp1_sequencer_t *seq, uint8_t track, uint8_t pulses) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_euclid_pattern_set_pulses(&seq->tracks[track].pattern, pulses);
}

void sp1_seq_set_track_steps(sp1_sequencer_t *seq, uint8_t track, uint8_t steps) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_euclid_pattern_set_steps(&seq->tracks[track].pattern, steps);
    if (seq->tracks[track].next_eval_step >= seq->tracks[track].pattern.steps) {
        seq->tracks[track].next_eval_step = 0U;
    }
    if (seq->tracks[track].playhead_step >= seq->tracks[track].pattern.steps) {
        seq->tracks[track].playhead_step = 0U;
    }
}

void sp1_seq_set_track_rotation(sp1_sequencer_t *seq, uint8_t track, uint8_t rotation) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_euclid_pattern_set_rotation(&seq->tracks[track].pattern, rotation);
}

void sp1_seq_set_track_nudge_ms(sp1_sequencer_t *seq, uint8_t track, float nudge_ms) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    if (nudge_ms < SP1_SEQ_NUDGE_MIN_MS) {
        nudge_ms = SP1_SEQ_NUDGE_MIN_MS;
    } else if (nudge_ms > SP1_SEQ_NUDGE_MAX_MS) {
        nudge_ms = SP1_SEQ_NUDGE_MAX_MS;
    }

    const int32_t frames = (nudge_ms >= 0.0f)
                               ? (int32_t)(nudge_ms * SP1_SEQ_FRAMES_PER_MS + 0.5f)
                               : (int32_t)(nudge_ms * SP1_SEQ_FRAMES_PER_MS - 0.5f);
    if (frames < -SP1_SEQ_NUDGE_MAX_FRAMES) {
        seq->tracks[track].nudge_frames = -SP1_SEQ_NUDGE_MAX_FRAMES;
    } else if (frames > SP1_SEQ_NUDGE_MAX_FRAMES) {
        seq->tracks[track].nudge_frames = SP1_SEQ_NUDGE_MAX_FRAMES;
    } else {
        seq->tracks[track].nudge_frames = (int16_t)frames;
    }
}

int16_t sp1_seq_get_track_nudge_frames(const sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return 0;
    }
    return seq->tracks[track].nudge_frames;
}

float sp1_seq_get_track_nudge_ms(const sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return 0.0f;
    }
    return (float)seq->tracks[track].nudge_frames * SP1_SEQ_INV_FRAMES_PER_MS;
}

void sp1_seq_set_track_mute(sp1_sequencer_t *seq, uint8_t track, bool muted) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    seq->tracks[track].muted = muted;
}

void sp1_seq_toggle_track_mute(sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    seq->tracks[track].muted = !seq->tracks[track].muted;
}

bool sp1_seq_is_track_muted(const sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return false;
    }
    return seq->tracks[track].muted;
}

void sp1_seq_set_track_midi_note(sp1_sequencer_t *seq, uint8_t track, uint8_t note) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    seq->tracks[track].midi_note = (uint8_t)(note & 0x7FU);
}

const sp1_euclid_pattern_t *sp1_seq_get_track_pattern(const sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return NULL;
    }
    return &seq->tracks[track].pattern;
}

sp1_clock_sync_t *sp1_seq_clock(sp1_sequencer_t *seq) {
    return (seq != NULL) ? &seq->clock : NULL;
}

const sp1_clock_sync_t *sp1_seq_clock_const(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? &seq->clock : NULL;
}

uint32_t sp1_seq_process_block(sp1_sequencer_t *seq,
                               uint64_t block_start_frame,
                               uint32_t num_frames,
                               sp1_seq_trigger_event_t *out_triggers,
                               uint32_t max_triggers) {
    if (seq == NULL) {
        return 0U;
    }

    const uint64_t block_end_frame = block_start_frame + (uint64_t)num_frames;
    sp1_clock_event_t clock_events[SP1_CLOCK_MAX_BLOCK_EVENTS];
    const uint32_t clk_count = sp1_clock_process_block(
        &seq->clock, block_start_frame, num_frames, clock_events, SP1_CLOCK_MAX_BLOCK_EVENTS);

    uint32_t trig_count = 0U;

    /* Interleave scheduled trigger/note-off firing with clock events in exact chronological
     * frame order so early negative-nudge hits fire before the grid tick inside the same block. */
    for (uint32_t i = 0U; i < clk_count; ++i) {
        const sp1_clock_event_t *evt = &clock_events[i];
        const uint64_t evt_limit =
            (evt->sample_frame <= block_end_frame) ? evt->sample_frame : block_end_frame;

        drain_events_before(seq, block_start_frame, evt_limit, num_frames,
                            out_triggers, max_triggers, &trig_count);

        switch (evt->type) {
        case SP1_CLOCK_EVT_START:
            handle_clock_start(seq, evt->sample_frame);
            break;
        case SP1_CLOCK_EVT_STOP:
            handle_clock_stop(seq, evt->sample_frame);
            break;
        case SP1_CLOCK_EVT_TICK:
            handle_clock_tick(seq, evt);
            break;
        case SP1_CLOCK_EVT_CONTINUE:
        case SP1_CLOCK_EVT_MODE_CHANGE:
        case SP1_CLOCK_EVT_NONE:
        default:
            break;
        }
    }

    /* Drain all remaining scheduled hits & MIDI Note-Offs within [block_start, block_end). */
    drain_events_before(seq, block_start_frame, block_end_frame, num_frames,
                        out_triggers, max_triggers, &trig_count);

    return trig_count;
}
