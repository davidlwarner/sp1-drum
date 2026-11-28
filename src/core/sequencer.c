#include "core/sequencer.h"

#include <stddef.h>
#include <string.h>

static const uint8_t k_default_gm_notes[SP1_SEQ_NUM_TRACKS] = {
    SP1_SEQ_GM_NOTE_KICK,
    SP1_SEQ_GM_NOTE_SNARE,
    SP1_SEQ_GM_NOTE_HAT,
    SP1_SEQ_GM_NOTE_PERC,
};

/* Base grid runs on 32nd-note slots (3 ticks @ 24 PPQN):
 *  - SP1_TRACK_SPEED_DIV4 (0.25x): every 8 slots (24 ticks = 1/4 note), mask = 7
 *  - SP1_TRACK_SPEED_DIV2 (0.50x): every 4 slots (12 ticks = 1/8 note), mask = 3
 *  - SP1_TRACK_SPEED_1X   (1.00x): every 2 slots ( 6 ticks = 1/16 note), mask = 1
 *  - SP1_TRACK_SPEED_2X   (2.00x): every 1 slot  ( 3 ticks = 1/32 note), mask = 0 */
#define SP1_SEQ_TICKS_PER_SLOT32    3U
static const uint32_t k_speed_slot_mask[4] = { 7U, 3U, 1U, 0U };

/* 48 frames per millisecond at 48,000 Hz (reciprocal 1/48 for ms inspection). */
#define SP1_SEQ_FRAMES_PER_MS       48.0f
#define SP1_SEQ_INV_FRAMES_PER_MS   (1.0f / 48.0f)

static uint8_t seq_next_prob_u8(sp1_sequencer_t *seq) {
    uint32_t x = seq->prob_rng;
    if (x == 0U) {
        x = 0x6D2B79F5U;
    }
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    seq->prob_rng = x;
    return (uint8_t)(x >> 24U);
}

static uint32_t get_slot32_frames(const sp1_clock_sync_t *clk) {
    if (clk == NULL) {
        return 3000U;
    }
    const uint64_t slot_q32 = clk->frames_per_tick_q32 * (uint64_t)SP1_SEQ_TICKS_PER_SLOT32;
    return (uint32_t)((slot_q32 + 0x80000000ULL) >> 32U);
}

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

static void clear_last_fired_slots(sp1_sequencer_t *seq) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        seq->tracks[t].last_fired_slot_plus1 = 0U;
    }
}

static void clamp_track_step_indices(sp1_seq_track_t *trk) {
    const uint8_t n = trk->pattern.steps;
    if (trk->next_eval_step >= n) {
        trk->next_eval_step = 0U;
    }
    if (trk->playhead_step >= n) {
        trk->playhead_step = 0U;
    }
    if (trk->committed_next_step >= n) {
        trk->committed_next_step = 0U;
    }
    for (uint8_t i = 0U; i < trk->ring_count; ++i) {
        if (trk->ring[i].track_step >= n) {
            trk->ring[i].track_step = 0U;
            trk->ring[i].accent = 1U;
            trk->ring[i].velocity = SP1_SEQ_VEL_ACCENT;
        }
    }
}

static uint64_t compute_step_fire_frame(const sp1_sequencer_t *seq,
                                        uint8_t track_idx,
                                        uint32_t slot32,
                                        uint64_t nominal_slot_frame,
                                        uint32_t step_16th_frames,
                                        uint64_t min_frame) {
    int64_t swing_frames = 0;
    /* Within each 8th-note group of four 32nd-slots (pos 0, 1, 2, 3):
     *  - pos 2 (off-beat 16th note): full global swing offset
     *  - pos 1 & pos 3 (32nd subdivisions): half swing offset to stay monotonic */
    if (seq->swing_offset_scale > 0.0f) {
        const uint32_t pos = slot32 & 3U;
        if (pos == 2U) {
            swing_frames = (int64_t)((float)step_16th_frames * seq->swing_offset_scale + 0.5f);
        } else if (pos == 1U || pos == 3U) {
            swing_frames =
                (int64_t)((float)step_16th_frames * (0.5f * seq->swing_offset_scale) + 0.5f);
        }
    }

    const int64_t nudge = (int64_t)seq->tracks[track_idx].nudge_frames;
    int64_t signed_fire = (int64_t)nominal_slot_frame + swing_frames + nudge;
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

static void evaluate_and_enqueue_slot(sp1_sequencer_t *seq,
                                      uint32_t slot32,
                                      uint64_t nominal_slot_frame,
                                      uint32_t step_16th_frames,
                                      uint64_t min_frame,
                                      bool skip_already_fired,
                                      bool is_committed_slot) {
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_track_t *trk = &seq->tracks[t];
        const uint8_t spd =
            (trk->speed <= (uint8_t)SP1_TRACK_SPEED_2X) ? trk->speed : (uint8_t)SP1_TRACK_SPEED_1X;
        if ((slot32 & k_speed_slot_mask[spd]) != 0U) {
            continue;
        }

        const uint8_t n = trk->pattern.steps;
        uint8_t local_step = trk->next_eval_step;
        if (local_step >= n) {
            local_step = 0U;
        }

        const bool already_fired =
            skip_already_fired && (trk->last_fired_slot_plus1 >= (slot32 + 1U));
        if (!already_fired && sp1_euclid_step_active(trk->pattern.mask, local_step)) {
            bool pass_prob = true;
            if (trk->probability_u8 == 0U) {
                pass_prob = false;
            } else if (trk->probability_u8 < 255U) {
                pass_prob = (seq_next_prob_u8(seq) < trk->probability_u8);
            }

            if (pass_prob) {
                sp1_seq_lookahead_event_t evt;
                evt.fire_frame = compute_step_fire_frame(
                    seq, t, slot32, nominal_slot_frame, step_16th_frames, min_frame);
                evt.global_step = slot32;
                evt.track_step = local_step;
                evt.accent = (local_step == 0U) ? 1U : 0U;
                evt.velocity = (local_step == 0U) ? SP1_SEQ_VEL_ACCENT : SP1_SEQ_VEL_NORMAL;
                evt.reserved = 0U;

                ring_insert_sorted(trk, &evt);
            }
        }

        trk->next_eval_step = sp1_euclid_next_step(local_step, n);
        if (is_committed_slot) {
            trk->committed_next_step = trk->next_eval_step;
        }
    }
}

static void reanchor_all_pending_events(sp1_sequencer_t *seq,
                                        uint64_t next_slot_frame,
                                        uint64_t min_frame) {
    if (!seq->has_scheduled_steps || !seq->clock.running || seq->clock.pending_start) {
        return;
    }

    const uint32_t step_16th_frames = sp1_clock_get_step_frames(&seq->clock);
    const uint32_t slot32_frames = get_slot32_frames(&seq->clock);

    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_track_t *trk = &seq->tracks[t];
        bool modified = false;

        for (uint8_t i = 0U; i < trk->ring_count; ++i) {
            if (trk->ring[i].fire_frame < min_frame) {
                continue;
            }
            const uint32_t ev_slot = trk->ring[i].global_step;
            if (ev_slot == seq->cur_grid_step) {
                trk->ring[i].fire_frame = compute_step_fire_frame(
                    seq, t, ev_slot, seq->last_grid_frame, step_16th_frames, min_frame);
                modified = true;
            } else if (ev_slot > seq->cur_grid_step) {
                const uint32_t ahead = ev_slot - (seq->cur_grid_step + 1U);
                const uint64_t nominal =
                    next_slot_frame + ((uint64_t)ahead * (uint64_t)slot32_frames);
                trk->ring[i].fire_frame = compute_step_fire_frame(
                    seq, t, ev_slot, nominal, step_16th_frames, min_frame);
                modified = true;
            }
        }

        if (modified) {
            ring_resort_small(trk);
        }
    }
}

static void reanchor_lookahead_from_control(sp1_sequencer_t *seq) {
    if (!seq->has_scheduled_steps || !seq->clock.running || seq->clock.pending_start) {
        return;
    }

    uint64_t next_slot_frame;
    if (seq->clock.mode == SP1_CLOCK_MODE_INTERNAL) {
        const uint32_t after_ticks =
            (seq->sub_tick == 0U) ? 0U : (uint32_t)(SP1_SEQ_TICKS_PER_SLOT32 - seq->sub_tick);
        const uint64_t total_q32 =
            (uint64_t)seq->clock.next_tick_frac +
            (seq->clock.frames_per_tick_q32 * (uint64_t)after_ticks);
        next_slot_frame = seq->clock.next_tick_frame + (total_q32 >> 32U);
    } else {
        const uint32_t rem_ticks =
            (seq->sub_tick == 0U) ? 1U : (uint32_t)(SP1_SEQ_TICKS_PER_SLOT32 + 1U - seq->sub_tick);
        const uint32_t rem_frames =
            (uint32_t)(((seq->clock.frames_per_tick_q32 * (uint64_t)rem_ticks) + 0x80000000ULL) >> 32U);
        next_slot_frame = seq->clock.prev_ext_tick_frame + (uint64_t)rem_frames;
    }

    reanchor_all_pending_events(seq, next_slot_frame, seq->clock.current_frame);
}

static uint32_t compute_midi_gate_frames(const sp1_sequencer_t *seq) {
    const uint32_t step_frames = sp1_clock_get_step_frames(&seq->clock);
    uint32_t gate_frames = step_frames >> 1U;
    if (gate_frames > SP1_SEQ_MIDI_GATE_MAX_FRAMES) {
        gate_frames = SP1_SEQ_MIDI_GATE_MAX_FRAMES;
    }
    if (gate_frames < 48U) {
        gate_frames = 48U;
    }
    return gate_frames;
}

static void drain_pending_auditions(sp1_sequencer_t *seq,
                                    uint64_t block_start_frame,
                                    sp1_seq_trigger_event_t *out_triggers,
                                    uint32_t max_triggers,
                                    uint32_t *out_count) {
    const uint32_t gate_frames = compute_midi_gate_frames(seq);

    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_track_t *trk = &seq->tracks[t];
        const uint8_t vel = trk->pending_audition_vel;
        if (vel == 0U) {
            continue;
        }
        trk->pending_audition_vel = 0U;

        if (trk->midi_note_active) {
            send_track_note_off(seq, trk, block_start_frame);
        }
        const sp1_midi_msg_t on = sp1_midi_msg_note_on(
            seq->midi_channel,
            trk->midi_note,
            vel,
            sp1_clock_frame_to_us(block_start_frame));
        (void)sp1_hal_midi_send(&on);

        trk->midi_note_active = true;
        trk->active_midi_note = trk->midi_note;
        trk->midi_note_off_frame = block_start_frame + (uint64_t)gate_frames;

        if (out_triggers != NULL && out_count != NULL && *out_count < max_triggers) {
            sp1_seq_trigger_event_t *dst = &out_triggers[*out_count];
            dst->sample_frame = block_start_frame;
            dst->block_offset = 0U;
            dst->track = t;
            dst->step_index = trk->playhead_step;
            dst->velocity = vel;
            dst->accent = (vel >= SP1_SEQ_VEL_ACCENT);
            (*out_count)++;
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
    const uint32_t gate_frames = compute_midi_gate_frames(seq);

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

        sp1_seq_lookahead_event_t sched = { 0 };
        ring_pop_front(trk, &sched);
        if ((sched.global_step + 1U) > trk->last_fired_slot_plus1) {
            trk->last_fired_slot_plus1 = sched.global_step + 1U;
        }

        if (!trk->muted) {
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
    clear_last_fired_slots(seq);

    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        seq->tracks[t].next_eval_step = 0U;
        seq->tracks[t].playhead_step = 0U;
        seq->tracks[t].committed_next_step = 0U;
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
        /* Rewind next_eval_step to committed_next_step so a subsequent Continue (0xFB)
         * resumes seamlessly without skipping lookahead steps flushed on Stop. */
        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            sp1_seq_track_t *trk = &seq->tracks[t];
            trk->next_eval_step = trk->committed_next_step;
        }
        seq->has_scheduled_steps = false;
    }
    seq->armed_for_start = false;
}

static void handle_clock_mode_change(sp1_sequencer_t *seq) {
    if (seq->clock.mode == SP1_CLOCK_MODE_INTERNAL && seq->has_scheduled_steps) {
        /* After a 500 ms external clock timeout, pre-queued lookahead slots have already
         * drained during the timeout window. Align cur_grid_step to odd so the immediate
         * internal fallback tick (++cur_grid_step) lands on the next even 16th-note boundary. */
        clear_last_fired_slots(seq);
        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            seq->tracks[t].committed_next_step = seq->tracks[t].next_eval_step;
        }
        seq->cur_grid_step = (seq->scheduled_up_to_step | 1U);
        seq->has_scheduled_steps = false;
        seq->sub_tick = 0U;
    }
}

static void handle_clock_tick(sp1_sequencer_t *seq, const sp1_clock_event_t *evt) {
    const uint64_t tick_frame = evt->sample_frame;
    const uint32_t step_16th_frames = sp1_clock_get_step_frames(&seq->clock);
    const uint32_t slot32_frames = get_slot32_frames(&seq->clock);

    if (seq->armed_for_start || (evt->tick_index == 0U && !seq->has_scheduled_steps)) {
        /* Tick 0 (beat 1 downbeat): evaluate Slot 0 (committed) + 2-slot (1 full 16th) lookahead. */
        seq->armed_for_start = false;
        seq->sub_tick = 0U;
        seq->cur_grid_step = 0U;
        seq->last_grid_frame = tick_frame;
        clear_last_fired_slots(seq);

        for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
            seq->tracks[t].playhead_step = 0U;
            seq->tracks[t].next_eval_step = 0U;
            seq->tracks[t].committed_next_step = 0U;
        }

        evaluate_and_enqueue_slot(
            seq, 0U, tick_frame, step_16th_frames, tick_frame, false, true);
        evaluate_and_enqueue_slot(
            seq, 1U, tick_frame + (uint64_t)slot32_frames, step_16th_frames, tick_frame, false, false);
        evaluate_and_enqueue_slot(
            seq, 2U, tick_frame + (uint64_t)step_16th_frames, step_16th_frames, tick_frame, false, false);

        seq->scheduled_up_to_step = 2U;
        seq->has_scheduled_steps = true;
        seq->sub_tick = 1U;
        return;
    }

    if (seq->sub_tick == 0U) {
        /* Reached a new 32nd-note slot boundary (every 3 PPQN ticks). */
        if (seq->has_scheduled_steps) {
            seq->cur_grid_step++;
            for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
                sp1_seq_track_t *trk = &seq->tracks[t];
                const uint8_t spd =
                    (trk->speed <= (uint8_t)SP1_TRACK_SPEED_2X) ? trk->speed : (uint8_t)SP1_TRACK_SPEED_1X;
                if ((seq->cur_grid_step & k_speed_slot_mask[spd]) == 0U) {
                    trk->playhead_step =
                        sp1_euclid_next_step(trk->playhead_step, trk->pattern.steps);
                    trk->committed_next_step =
                        sp1_euclid_next_step(trk->playhead_step, trk->pattern.steps);
                }
            }
        } else {
            /* Resuming via MIDI Continue (0xFB) or timeout fallback on a slot boundary. */
            seq->cur_grid_step++;
            for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
                sp1_seq_track_t *trk = &seq->tracks[t];
                const uint8_t spd =
                    (trk->speed <= (uint8_t)SP1_TRACK_SPEED_2X) ? trk->speed : (uint8_t)SP1_TRACK_SPEED_1X;
                if ((seq->cur_grid_step & k_speed_slot_mask[spd]) == 0U) {
                    trk->playhead_step = trk->next_eval_step;
                }
            }
            evaluate_and_enqueue_slot(
                seq, seq->cur_grid_step, tick_frame, step_16th_frames, tick_frame, true, true);
            evaluate_and_enqueue_slot(
                seq,
                seq->cur_grid_step + 1U,
                tick_frame + (uint64_t)slot32_frames,
                step_16th_frames,
                tick_frame,
                true,
                false);
            seq->scheduled_up_to_step = seq->cur_grid_step + 1U;
            seq->has_scheduled_steps = true;
        }

        seq->last_grid_frame = tick_frame;

        /* Re-anchor any still-pending events across tempo/bend updates. */
        reanchor_all_pending_events(
            seq, tick_frame + (uint64_t)slot32_frames, tick_frame);

        /* 2-Slot (1 full 16th-note = 6 PPQN ticks) Lookahead: evaluate cur_grid_step + 2. */
        const uint32_t lookahead_slot = seq->cur_grid_step + 2U;
        if (lookahead_slot > seq->scheduled_up_to_step) {
            evaluate_and_enqueue_slot(seq,
                                      lookahead_slot,
                                      tick_frame + (uint64_t)step_16th_frames,
                                      step_16th_frames,
                                      tick_frame,
                                      false,
                                      false);
            seq->scheduled_up_to_step = lookahead_slot;
        }
    } else {
        const uint32_t rem_ticks = (uint32_t)(SP1_SEQ_TICKS_PER_SLOT32 - seq->sub_tick);
        const uint32_t rem_frames =
            (uint32_t)(((seq->clock.frames_per_tick_q32 * (uint64_t)rem_ticks) + 0x80000000ULL) >> 32U);
        const uint64_t next_slot_frame = tick_frame + (uint64_t)rem_frames;

        if (!seq->has_scheduled_steps) {
            /* Resuming via MIDI Continue (0xFB) mid-slot (sub_tick in 1..2). */
            evaluate_and_enqueue_slot(seq,
                                      seq->cur_grid_step + 1U,
                                      next_slot_frame,
                                      step_16th_frames,
                                      tick_frame,
                                      true,
                                      false);
            evaluate_and_enqueue_slot(seq,
                                      seq->cur_grid_step + 2U,
                                      next_slot_frame + (uint64_t)slot32_frames,
                                      step_16th_frames,
                                      tick_frame,
                                      true,
                                      false);
            seq->scheduled_up_to_step = seq->cur_grid_step + 2U;
            seq->has_scheduled_steps = true;
        } else if (seq->clock.mode == SP1_CLOCK_MODE_EXTERNAL) {
            reanchor_all_pending_events(seq, next_slot_frame, tick_frame);
        }
    }

    seq->sub_tick++;
    if (seq->sub_tick >= SP1_SEQ_TICKS_PER_SLOT32) {
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
    seq->prob_rng = 0x6D2B79F5U;
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
        seq->tracks[t].speed = (uint8_t)SP1_TRACK_SPEED_1X;
        seq->tracks[t].probability_u8 = 255U;
        seq->tracks[t].pending_audition_vel = 0U;
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
    reanchor_lookahead_from_control(seq);
}

void sp1_seq_set_bpm(sp1_sequencer_t *seq, float bpm) {
    if (seq == NULL) {
        return;
    }
    sp1_clock_set_bpm(&seq->clock, bpm);
    reanchor_lookahead_from_control(seq);
}

uint32_t sp1_seq_get_bpm_x100(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? sp1_clock_get_bpm_x100(&seq->clock) : SP1_CLOCK_BPM_DEFAULT_X100;
}

void sp1_seq_set_tempo_bend(sp1_sequencer_t *seq, int8_t bend_dir) {
    if (seq == NULL) {
        return;
    }
    const int8_t prev = sp1_clock_get_tempo_bend(&seq->clock);
    sp1_clock_set_tempo_bend(&seq->clock, bend_dir);
    if (sp1_clock_get_tempo_bend(&seq->clock) != prev) {
        reanchor_lookahead_from_control(seq);
    }
}

int8_t sp1_seq_get_tempo_bend(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? sp1_clock_get_tempo_bend(&seq->clock) : 0;
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
    reanchor_lookahead_from_control(seq);
}

float sp1_seq_get_swing(const sp1_sequencer_t *seq) {
    return (seq != NULL) ? seq->swing_ratio : SP1_SEQ_SWING_DEFAULT;
}

void sp1_seq_set_midi_channel(sp1_sequencer_t *seq, uint8_t channel_0_to_15) {
    if (seq == NULL) {
        return;
    }
    const uint8_t next_ch = (uint8_t)(channel_0_to_15 & 0x0FU);
    if (next_ch != seq->midi_channel) {
        release_all_active_notes(seq, seq->clock.current_frame);
        seq->midi_channel = next_ch;
    }
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
    clamp_track_step_indices(&seq->tracks[track]);
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
    clamp_track_step_indices(&seq->tracks[track]);
}

void sp1_seq_set_track_rotation(sp1_sequencer_t *seq, uint8_t track, uint8_t rotation) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_euclid_pattern_set_rotation(&seq->tracks[track].pattern, rotation);
}

void sp1_seq_set_track_speed(sp1_sequencer_t *seq, uint8_t track, sp1_track_speed_t speed) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    uint8_t clamped = (uint8_t)speed;
    if (clamped > (uint8_t)SP1_TRACK_SPEED_2X) {
        clamped = (uint8_t)SP1_TRACK_SPEED_2X;
    }
    sp1_seq_track_t *trk = &seq->tracks[track];
    if (trk->speed != clamped) {
        trk->speed = clamped;
        /* Flush stale pre-scheduled lookahead events at the previous clock speed so
         * upcoming slots evaluate cleanly at the new stride without overlap. */
        if (seq->has_scheduled_steps) {
            trk->ring_count = 0U;
            trk->next_eval_step = trk->committed_next_step;
        }
    }
}

sp1_track_speed_t sp1_seq_get_track_speed(const sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return SP1_TRACK_SPEED_1X;
    }
    return (sp1_track_speed_t)seq->tracks[track].speed;
}

void sp1_seq_set_track_probability(sp1_sequencer_t *seq, uint8_t track, float prob_0_to_1) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    if (prob_0_to_1 <= 0.0f) {
        seq->tracks[track].probability_u8 = 0U;
    } else if (prob_0_to_1 >= 0.996f) {
        seq->tracks[track].probability_u8 = 255U;
    } else {
        seq->tracks[track].probability_u8 = (uint8_t)(prob_0_to_1 * 255.0f + 0.5f);
    }
}

float sp1_seq_get_track_probability(const sp1_sequencer_t *seq, uint8_t track) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS) {
        return 1.0f;
    }
    return (float)seq->tracks[track].probability_u8 * (1.0f / 255.0f);
}

void sp1_seq_trigger_audition(sp1_sequencer_t *seq, uint8_t track, uint8_t velocity) {
    if (seq == NULL || track >= SP1_SEQ_NUM_TRACKS || velocity == 0U) {
        return;
    }
    seq->tracks[track].pending_audition_vel = (uint8_t)(velocity & 0x7FU);
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

    reanchor_lookahead_from_control(seq);
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
    if (seq == NULL || num_frames == 0U) {
        return 0U;
    }

    const uint64_t block_end_frame = block_start_frame + (uint64_t)num_frames;
    sp1_clock_event_t clock_events[SP1_CLOCK_MAX_BLOCK_EVENTS];

    uint32_t trig_count = 0U;
    drain_pending_auditions(seq, block_start_frame, out_triggers, max_triggers, &trig_count);

    seq->clock.defer_midi_tx = true;
    const uint32_t clk_count = sp1_clock_process_block(
        &seq->clock, block_start_frame, num_frames, clock_events, SP1_CLOCK_MAX_BLOCK_EVENTS);
    seq->clock.defer_midi_tx = false;

    /* Interleave scheduled trigger/note-off firing with clock events in exact chronological
     * frame order so early negative-nudge hits fire before the grid tick inside the same block. */
    for (uint32_t i = 0U; i < clk_count; ++i) {
        const sp1_clock_event_t *evt = &clock_events[i];
        const uint64_t evt_limit =
            (evt->sample_frame <= block_end_frame) ? evt->sample_frame : block_end_frame;

        drain_events_before(seq, block_start_frame, evt_limit, num_frames,
                            out_triggers, max_triggers, &trig_count);

        if (evt->midi_tx_status != 0U) {
            const sp1_midi_msg_t rt_msg =
                sp1_midi_msg_realtime(evt->midi_tx_status, sp1_clock_frame_to_us(evt->sample_frame));
            (void)sp1_hal_midi_send(&rt_msg);
        }

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
        case SP1_CLOCK_EVT_MODE_CHANGE:
            handle_clock_mode_change(seq);
            break;
        case SP1_CLOCK_EVT_CONTINUE:
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
