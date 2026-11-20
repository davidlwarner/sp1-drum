#include "core/clock_sync.h"

#include <stddef.h>
#include <string.h>

/* 60 s/min * 100 centi-BPM * 48,000 frames/s = 288,000,000 */
#define SP1_CLOCK_NUM_FRAMES_PER_MIN_X100  288000000ULL

/* 288,000,000 / 24 PPQN = 12,000,000 */
#define SP1_CLOCK_BPM_Q32_NUMERATOR        (12000000ULL << 32U)

/* round(48,000 / 1,000,000 * 2^32) = 206,158,430 (0x0C49BA5E):
 * converts microsecond deltas to Q32.32 48 kHz frames without division. */
#define SP1_CLOCK_US_TO_FRAMES_Q32         206158430ULL

/* Clamped Q32.32 frames-per-tick bounds matching [40.00 .. 240.00] BPM */
#define SP1_CLOCK_MIN_TICK_Q32             (500ULL << 32U)   /* 240 BPM */
#define SP1_CLOCK_MAX_TICK_Q32             (3000ULL << 32U)  /*  40 BPM */

/* Plausible inter-tick delta bounds for external MIDI clock PLL (~25..416 BPM) */
#define SP1_CLOCK_EXT_DT_US_MIN            6000U
#define SP1_CLOCK_EXT_DT_US_MAX            100000U
#define SP1_CLOCK_EXT_DF_MIN               288ULL
#define SP1_CLOCK_EXT_DF_MAX               4800ULL

static uint32_t clamp_bpm_x100(uint32_t bpm_x100) {
    if (bpm_x100 < SP1_CLOCK_BPM_MIN_X100) {
        return SP1_CLOCK_BPM_MIN_X100;
    }
    if (bpm_x100 > SP1_CLOCK_BPM_MAX_X100) {
        return SP1_CLOCK_BPM_MAX_X100;
    }
    return bpm_x100;
}

static uint64_t bpm_x100_to_tick_q32(uint32_t bpm_x100) {
    const uint32_t clamped = clamp_bpm_x100(bpm_x100);
    const uint64_t den = (uint64_t)clamped * (uint64_t)SP1_CLOCK_PPQN;
    /* Ceiling division eliminates 1-ULP early tick drift across exact rational periods. */
    return ((SP1_CLOCK_NUM_FRAMES_PER_MIN_X100 << 32U) + den - 1ULL) / den;
}

static uint32_t tick_q32_to_bpm_x100(uint64_t tick_q32) {
    if (tick_q32 == 0ULL) {
        return SP1_CLOCK_BPM_DEFAULT_X100;
    }
    const uint64_t bpm = (SP1_CLOCK_BPM_Q32_NUMERATOR + (tick_q32 >> 1U)) / tick_q32;
    if (bpm < (uint64_t)SP1_CLOCK_BPM_MIN_X100) {
        return SP1_CLOCK_BPM_MIN_X100;
    }
    if (bpm > (uint64_t)SP1_CLOCK_BPM_MAX_X100) {
        return SP1_CLOCK_BPM_MAX_X100;
    }
    return (uint32_t)bpm;
}

static uint64_t clamp_tick_q32(uint64_t tick_q32) {
    if (tick_q32 < SP1_CLOCK_MIN_TICK_Q32) {
        return SP1_CLOCK_MIN_TICK_Q32;
    }
    if (tick_q32 > SP1_CLOCK_MAX_TICK_Q32) {
        return SP1_CLOCK_MAX_TICK_Q32;
    }
    return tick_q32;
}

static void push_event(sp1_clock_event_t *out_events,
                       uint32_t max_events,
                       uint32_t *count,
                       sp1_clock_event_type_t type,
                       uint64_t sample_frame,
                       uint32_t tick_index,
                       uint8_t midi_tx_status) {
    if (out_events == NULL || count == NULL || *count >= max_events) {
        return;
    }
    sp1_clock_event_t *evt = &out_events[*count];
    evt->type = type;
    evt->sample_frame = sample_frame;
    evt->tick_index = tick_index;
    evt->midi_tx_status = midi_tx_status;
    (*count)++;
}

static void send_realtime_midi(uint8_t status, uint64_t sample_frame) {
    const sp1_midi_msg_t msg =
        sp1_midi_msg_realtime(status, sp1_clock_frame_to_us(sample_frame));
    (void)sp1_hal_midi_send(&msg);
}

uint32_t sp1_clock_frame_to_us(uint64_t sample_frame) {
    /* Exact floor(sample_frame * 125 / 6) mod 2^32 without hardware division for all 64-bit
     * frames: floor(x / 6) = (x * 0xAAAAAAAB) >> 34, and 2^32 = 6 * 0x2AAAAAAA + 4. */
    static const uint8_t k_rem_us[6] = { 0U, 20U, 41U, 62U, 83U, 104U };

    const uint32_t lo = (uint32_t)sample_frame;
    const uint32_t q_lo = (uint32_t)(((uint64_t)lo * 0xAAAAAAABULL) >> 34U);
    const uint32_t r_lo = lo - (q_lo * 6U);

    const uint32_t hi = (uint32_t)(sample_frame >> 32U);
    if (hi == 0U) {
        return (q_lo * 125U) + (uint32_t)k_rem_us[r_lo];
    }

    const uint32_t q_hi = (uint32_t)(((uint64_t)hi * 0xAAAAAAABULL) >> 34U);
    const uint32_t r_hi = hi - (q_hi * 6U);
    const uint32_t rem_total = (r_hi * 4U) + r_lo;
    const uint32_t q_rem = (uint32_t)(((uint64_t)rem_total * 0xAAAAAAABULL) >> 34U);
    const uint32_t r_final = rem_total - (q_rem * 6U);
    const uint32_t q_mod32 = (r_hi * 0x2AAAAAAAU) + q_lo + q_rem;

    return (q_mod32 * 125U) + (uint32_t)k_rem_us[r_final];
}

void sp1_clock_init(sp1_clock_sync_t *clk) {
    if (clk == NULL) {
        return;
    }
    memset(clk, 0, sizeof(*clk));
    clk->mode = SP1_CLOCK_MODE_INTERNAL;
    clk->bpm_x100 = SP1_CLOCK_BPM_DEFAULT_X100;
    clk->frames_per_tick_q32 = bpm_x100_to_tick_q32(clk->bpm_x100);
    clk->ext_frames_per_tick_q32 = clk->frames_per_tick_q32;
}

void sp1_clock_set_midi_passthrough(sp1_clock_sync_t *clk,
                                    sp1_clock_midi_rx_cb_t cb,
                                    void *user_ctx) {
    if (clk == NULL) {
        return;
    }
    clk->passthrough_cb = cb;
    clk->passthrough_ctx = user_ctx;
}

void sp1_clock_set_bpm_x100(sp1_clock_sync_t *clk, uint32_t bpm_x100) {
    if (clk == NULL) {
        return;
    }

    const uint32_t clamped = clamp_bpm_x100(bpm_x100);
    clk->bpm_x100 = clamped;

    const uint64_t new_tick_q32 = bpm_x100_to_tick_q32(clamped);
    if (clk->mode == SP1_CLOCK_MODE_INTERNAL || !clk->ext_bpm_locked) {
        clk->frames_per_tick_q32 = new_tick_q32;
    }

    if (clk->mode == SP1_CLOCK_MODE_INTERNAL) {
        /* If tempo increased while running, pull next_tick_frame within one new period
         * so fast tempo ramps respond immediately without stalling or double-firing. */
        if (clk->running && !clk->pending_start) {
            const uint64_t new_period_int = new_tick_q32 >> 32U;
            const uint64_t max_next = clk->current_frame + new_period_int;
            if (clk->next_tick_frame > max_next) {
                clk->next_tick_frame = max_next;
                clk->next_tick_frac = (uint32_t)(new_tick_q32 & 0xFFFFFFFFULL);
            }
        }
    }
}

void sp1_clock_set_bpm(sp1_clock_sync_t *clk, float bpm) {
    if (clk == NULL) {
        return;
    }
    if (bpm < 40.0f) {
        bpm = 40.0f;
    } else if (bpm > 240.0f) {
        bpm = 240.0f;
    }
    sp1_clock_set_bpm_x100(clk, (uint32_t)(bpm * 100.0f + 0.5f));
}

uint32_t sp1_clock_get_bpm_x100(const sp1_clock_sync_t *clk) {
    if (clk == NULL) {
        return SP1_CLOCK_BPM_DEFAULT_X100;
    }
    if (clk->mode == SP1_CLOCK_MODE_EXTERNAL && clk->ext_bpm_locked) {
        return tick_q32_to_bpm_x100(clk->ext_frames_per_tick_q32);
    }
    if (clk->bpm_x100 == 0U) {
        return tick_q32_to_bpm_x100(clk->frames_per_tick_q32);
    }
    return clk->bpm_x100;
}

uint32_t sp1_clock_get_step_frames(const sp1_clock_sync_t *clk) {
    if (clk == NULL) {
        return 6000U;
    }
    const uint64_t step_q32 = clk->frames_per_tick_q32 * (uint64_t)SP1_CLOCK_TICKS_PER_16TH;
    return (uint32_t)((step_q32 + 0x80000000ULL) >> 32U);
}

sp1_clock_mode_t sp1_clock_get_mode(const sp1_clock_sync_t *clk) {
    return (clk != NULL) ? clk->mode : SP1_CLOCK_MODE_INTERNAL;
}

bool sp1_clock_is_running(const sp1_clock_sync_t *clk) {
    return (clk != NULL) ? clk->running : false;
}

uint32_t sp1_clock_get_tick_index(const sp1_clock_sync_t *clk) {
    return (clk != NULL) ? clk->tick_index : 0U;
}

void sp1_clock_start(sp1_clock_sync_t *clk) {
    if (clk == NULL) {
        return;
    }
    clk->running = true;
    clk->pending_start = true;
}

void sp1_clock_stop(sp1_clock_sync_t *clk) {
    if (clk == NULL) {
        return;
    }
    const bool was_running = clk->running;
    const bool was_pending_start = clk->pending_start;
    clk->running = false;
    clk->pending_start = false;

    if (was_running && !was_pending_start && clk->mode == SP1_CLOCK_MODE_INTERNAL) {
        send_realtime_midi(SP1_MIDI_STATUS_STOP, clk->current_frame);
    }
}

static void enter_external_mode(sp1_clock_sync_t *clk,
                                uint64_t frame,
                                sp1_clock_event_t *out_events,
                                uint32_t max_events,
                                uint32_t *evt_count) {
    if (clk->mode != SP1_CLOCK_MODE_EXTERNAL) {
        clk->mode = SP1_CLOCK_MODE_EXTERNAL;
        clk->pending_start = false;
        clk->ext_bpm_locked = false;
        clk->ext_has_prev_tick = false;
        clk->ext_frames_per_tick_q32 = clk->frames_per_tick_q32;
        push_event(out_events, max_events, evt_count,
                   SP1_CLOCK_EVT_MODE_CHANGE, frame, clk->tick_index, 0U);
    }
    clk->last_ext_rx_frame = frame;
}

static void handle_external_clock_tick(sp1_clock_sync_t *clk,
                                       const sp1_midi_msg_t *msg,
                                       uint64_t frame,
                                       sp1_clock_event_t *out_events,
                                       uint32_t max_events,
                                       uint32_t *evt_count) {
    enter_external_mode(clk, frame, out_events, max_events, evt_count);

    if (clk->ext_has_prev_tick) {
        uint64_t meas_q32 = 0ULL;
        const uint32_t dt_us = (uint32_t)(msg->timestamp_us - clk->prev_ext_rx_ts_us);

        if (dt_us >= SP1_CLOCK_EXT_DT_US_MIN && dt_us <= SP1_CLOCK_EXT_DT_US_MAX) {
            meas_q32 = (uint64_t)dt_us * SP1_CLOCK_US_TO_FRAMES_Q32;
        } else if (dt_us == 0U && frame > clk->prev_ext_tick_frame) {
            const uint64_t df = frame - clk->prev_ext_tick_frame;
            if (df >= SP1_CLOCK_EXT_DF_MIN && df <= SP1_CLOCK_EXT_DF_MAX) {
                meas_q32 = df << 32U;
            }
        }

        if (meas_q32 != 0ULL) {
            meas_q32 = clamp_tick_q32(meas_q32);
            if (!clk->ext_bpm_locked) {
                clk->ext_frames_per_tick_q32 = meas_q32;
                clk->ext_bpm_locked = true;
            } else {
                /* 1-pole shift-based PLL smoothing (alpha = 1/4, zero division). */
                const int64_t err =
                    (int64_t)meas_q32 - (int64_t)clk->ext_frames_per_tick_q32;
                clk->ext_frames_per_tick_q32 =
                    clamp_tick_q32((uint64_t)((int64_t)clk->ext_frames_per_tick_q32 + (err >> 2)));
            }
            clk->frames_per_tick_q32 = clk->ext_frames_per_tick_q32;
        }
    }

    clk->prev_ext_rx_ts_us = msg->timestamp_us;
    clk->prev_ext_tick_frame = frame;
    clk->ext_has_prev_tick = true;

    if (clk->running) {
        push_event(out_events, max_events, evt_count,
                   SP1_CLOCK_EVT_TICK, frame, clk->tick_index, 0U);
        clk->tick_index++;
    }
}

uint32_t sp1_clock_process_block(sp1_clock_sync_t *clk,
                                 uint64_t block_start_frame,
                                 uint32_t num_frames,
                                 sp1_clock_event_t *out_events,
                                 uint32_t max_events) {
    if (clk == NULL || num_frames == 0U) {
        return 0U;
    }

    const uint64_t block_end_frame = block_start_frame + (uint64_t)num_frames;
    uint32_t evt_count = 0U;

    /* 1. Drain incoming MIDI RX SPSC queue (bounded to queue capacity for hard-RT safety). */
    sp1_midi_msg_t rx_msg;
    for (uint32_t i = 0U; i < SP1_MIDI_QUEUE_CAPACITY; ++i) {
        if (!sp1_hal_midi_receive(&rx_msg)) {
            break;
        }

        if (rx_msg.status == SP1_MIDI_STATUS_START) {
            enter_external_mode(clk, block_start_frame, out_events, max_events, &evt_count);
            clk->running = true;
            clk->tick_index = 0U;
            push_event(out_events, max_events, &evt_count,
                       SP1_CLOCK_EVT_START, block_start_frame, 0U, 0U);
        } else if (rx_msg.status == SP1_MIDI_STATUS_CONTINUE) {
            enter_external_mode(clk, block_start_frame, out_events, max_events, &evt_count);
            clk->running = true;
            push_event(out_events, max_events, &evt_count,
                       SP1_CLOCK_EVT_CONTINUE, block_start_frame, clk->tick_index, 0U);
        } else if (rx_msg.status == SP1_MIDI_STATUS_STOP) {
            enter_external_mode(clk, block_start_frame, out_events, max_events, &evt_count);
            clk->running = false;
            push_event(out_events, max_events, &evt_count,
                       SP1_CLOCK_EVT_STOP, block_start_frame, clk->tick_index, 0U);
        } else if (rx_msg.status == SP1_MIDI_STATUS_CLOCK) {
            handle_external_clock_tick(clk, &rx_msg, block_start_frame,
                                       out_events, max_events, &evt_count);
        } else if (clk->passthrough_cb != NULL) {
            clk->passthrough_cb(&rx_msg, clk->passthrough_ctx);
        }
    }

    /* 2. Check 500 ms (24,000 frames) external tick timeout -> clean fallback to INTERNAL. */
    if (clk->mode == SP1_CLOCK_MODE_EXTERNAL &&
        block_end_frame >= clk->last_ext_rx_frame &&
        (block_end_frame - clk->last_ext_rx_frame) >= SP1_CLOCK_EXT_TIMEOUT_FRAMES) {
        uint64_t fallback_frame = clk->last_ext_rx_frame + SP1_CLOCK_EXT_TIMEOUT_FRAMES;
        if (fallback_frame < block_start_frame) {
            fallback_frame = block_start_frame;
        }

        /* Adopt PLL-locked Q32.32 tick period directly without runtime division;
         * sp1_clock_get_bpm_x100() lazily derives centi-BPM when bpm_x100 == 0. */
        if (clk->ext_bpm_locked) {
            clk->frames_per_tick_q32 = clk->ext_frames_per_tick_q32;
            clk->bpm_x100 = 0U;
        }

        clk->mode = SP1_CLOCK_MODE_INTERNAL;
        clk->ext_bpm_locked = false;
        clk->ext_has_prev_tick = false;

        /* Re-anchor internal accumulator at fallback_frame so zero catch-up burst occurs. */
        clk->next_tick_frame = fallback_frame;
        clk->next_tick_frac = 0U;

        push_event(out_events, max_events, &evt_count,
                   SP1_CLOCK_EVT_MODE_CHANGE, fallback_frame, clk->tick_index, 0U);
    }

    /* 3. Internal Master 24 PPQN tick generation (O(ticks in block), zero divisions). */
    if (clk->mode == SP1_CLOCK_MODE_INTERNAL && clk->running) {
        if (clk->pending_start) {
            clk->pending_start = false;
            clk->tick_index = 0U;
            clk->next_tick_frame = block_start_frame;
            clk->next_tick_frac = 0U;

            if (!clk->defer_midi_tx) {
                send_realtime_midi(SP1_MIDI_STATUS_START, block_start_frame);
            }
            push_event(out_events, max_events, &evt_count,
                       SP1_CLOCK_EVT_START, block_start_frame, 0U, SP1_MIDI_STATUS_START);
        }

        const uint64_t step_int = clk->frames_per_tick_q32 >> 32U;
        const uint64_t step_frac = clk->frames_per_tick_q32 & 0xFFFFFFFFULL;

        while (clk->next_tick_frame < block_end_frame) {
            const uint64_t tick_frame = clk->next_tick_frame;
            if (!clk->defer_midi_tx) {
                send_realtime_midi(SP1_MIDI_STATUS_CLOCK, tick_frame);
            }
            push_event(out_events, max_events, &evt_count,
                       SP1_CLOCK_EVT_TICK, tick_frame, clk->tick_index, SP1_MIDI_STATUS_CLOCK);
            clk->tick_index++;

            const uint64_t frac_sum = (uint64_t)clk->next_tick_frac + step_frac;
            clk->next_tick_frame += step_int + (frac_sum >> 32U);
            clk->next_tick_frac = (uint32_t)(frac_sum & 0xFFFFFFFFULL);
        }
    }

    clk->current_frame = block_end_frame;
    return evt_count;
}
