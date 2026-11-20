#include "hal/sp1_hal.h"

#include <stddef.h>
#include <string.h>

/* Compile-time sanity check: SPSC capacity must be a non-zero power of two. */
_Static_assert((SP1_MIDI_QUEUE_CAPACITY != 0U) &&
               ((SP1_MIDI_QUEUE_CAPACITY & (SP1_MIDI_QUEUE_CAPACITY - 1U)) == 0U),
               "SP1_MIDI_QUEUE_CAPACITY must be a power of 2");

/* ── Portable Fader Filtering & Control Snapshot Bus ─────────────────────── */

void sp1_fader_filter_init(sp1_fader_filter_t *filter) {
    if (filter == NULL) {
        return;
    }
    filter->raw_clamped = 0;
    filter->cc7 = 0U;
    filter->initialized = false;
}

bool sp1_fader_filter_update(sp1_fader_filter_t *filter,
                             int16_t raw_sample,
                             float *out_norm,
                             uint8_t *out_cc7) {
    if (filter == NULL) {
        return false;
    }

    int16_t clamped = raw_sample;
    if (clamped < SP1_FADER_RAW_MIN) {
        clamped = SP1_FADER_RAW_MIN;
    } else if (clamped > SP1_FADER_RAW_MAX) {
        clamped = SP1_FADER_RAW_MAX;
    }

    bool changed = false;
    if (!filter->initialized) {
        filter->raw_clamped = clamped;
        filter->initialized = true;
        changed = true;
    } else {
        int32_t diff = (int32_t)clamped - (int32_t)filter->raw_clamped;
        if (diff < 0) {
            diff = -diff;
        }
        if (diff >= SP1_FADER_DEADBAND) {
            filter->raw_clamped = clamped;
            changed = true;
        }
    }

    int32_t cc_val = (int32_t)filter->raw_clamped >> 5;
    if (cc_val < 0) {
        cc_val = 0;
    } else if (cc_val > 127) {
        cc_val = 127;
    }
    filter->cc7 = (uint8_t)cc_val;

    if (out_norm != NULL) {
        /* Multiply by precomputed reciprocal (avoid runtime division). */
        *out_norm = (float)filter->raw_clamped * (1.0f / (float)SP1_FADER_RAW_MAX);
    }
    if (out_cc7 != NULL) {
        *out_cc7 = filter->cc7;
    }

    return changed;
}

bool sp1_controls_is_valid_shift_chord(uint16_t buttons_down,
                                       sp1_button_id_t shift_btn,
                                       sp1_button_id_t track_btn) {
    if ((unsigned)shift_btn >= (unsigned)SP1_BTN_COUNT ||
        (unsigned)track_btn >= (unsigned)SP1_BTN_COUNT) {
        return false;
    }

    /* Enforce SP-1 AIN0 hardware constraint: Play can NEVER act as a T1..T4 shift. */
    if (shift_btn == SP1_BTN_PLAY) {
        return false;
    }

    const uint16_t shift_bit = SP1_BTN_BIT(shift_btn);
    const uint16_t track_bit = SP1_BTN_BIT(track_btn);

    if ((shift_bit & SP1_BTN_VALID_SHIFT_MASK) == 0U) {
        return false;
    }
    if ((track_bit & SP1_BTN_TRACK_MASK) == 0U) {
        return false;
    }

    return (buttons_down & shift_bit) != 0U && (buttons_down & track_bit) != 0U;
}

void sp1_controls_snapshot_init(sp1_controls_snapshot_t *snap) {
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
}

void sp1_controls_bus_init(sp1_controls_bus_t *bus) {
    if (bus == NULL) {
        return;
    }
    memset(bus, 0, sizeof(*bus));
    __atomic_store_n(&bus->active_idx, 0U, __ATOMIC_RELEASE);
}

void sp1_controls_bus_publish(sp1_controls_bus_t *bus,
                              const sp1_controls_snapshot_t *snap) {
    if (bus == NULL || snap == NULL) {
        return;
    }
    const uint32_t cur = __atomic_load_n(&bus->active_idx, __ATOMIC_RELAXED) & 1U;
    const uint32_t next = cur ^ 1U;
    bus->buffers[next] = *snap;
    __atomic_store_n(&bus->active_idx, next, __ATOMIC_RELEASE);
}

void sp1_controls_bus_read(const sp1_controls_bus_t *bus,
                           sp1_controls_snapshot_t *out_snap) {
    if (bus == NULL || out_snap == NULL) {
        return;
    }
    const uint32_t cur = __atomic_load_n(&bus->active_idx, __ATOMIC_ACQUIRE) & 1U;
    *out_snap = bus->buffers[cur];
}

/* ── Portable Audio Packing (int16_t -> 24-bit-in-32-bit I2S DMA words) ─── */

void sp1_audio_pack_s16_to_i2s24_in_32(const int16_t *src_s16,
                                       int32_t *dst_i2s32,
                                       uint32_t num_frames) {
    if (src_s16 == NULL || dst_i2s32 == NULL || num_frames == 0U) {
        return;
    }

    const uint32_t total_samples = num_frames * SP1_AUDIO_CHANNELS;
    for (uint32_t i = 0U; i < total_samples; ++i) {
        /* Multiply by 256 (2^8) to avoid C11 signed left-shift UB on negative values.
         * Places int16_t into bits [23:8] with sign extension in [31:24]. */
        dst_i2s32[i] = (int32_t)src_s16[i] * SP1_AUDIO_I2S24_SCALE;
    }
}

/* ── Portable Lock-Free SPSC MIDI Queue ──────────────────────────────────── */

void sp1_midi_queue_init(sp1_midi_spsc_queue_t *q) {
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
    __atomic_store_n(&q->head, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&q->tail, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&q->dropped, 0U, __ATOMIC_RELEASE);
}

bool sp1_midi_queue_push(sp1_midi_spsc_queue_t *q, const sp1_midi_msg_t *msg) {
    if (q == NULL || msg == NULL) {
        return false;
    }

    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);

    /* Unsigned subtraction handles uint32_t wrap-around seamlessly. */
    if ((uint32_t)(head - tail) >= SP1_MIDI_QUEUE_CAPACITY) {
        const uint32_t dropped = __atomic_load_n(&q->dropped, __ATOMIC_RELAXED);
        __atomic_store_n(&q->dropped, dropped + 1U, __ATOMIC_RELAXED);
        return false;
    }

    q->entries[head & SP1_MIDI_QUEUE_MASK] = *msg;
    __atomic_store_n(&q->head, head + 1U, __ATOMIC_RELEASE);
    return true;
}

bool sp1_midi_queue_pop(sp1_midi_spsc_queue_t *q, sp1_midi_msg_t *out_msg) {
    if (q == NULL || out_msg == NULL) {
        return false;
    }

    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);

    if (head == tail) {
        return false;
    }

    *out_msg = q->entries[tail & SP1_MIDI_QUEUE_MASK];
    __atomic_store_n(&q->tail, tail + 1U, __ATOMIC_RELEASE);
    return true;
}

uint32_t sp1_midi_queue_count(const sp1_midi_spsc_queue_t *q) {
    if (q == NULL) {
        return 0U;
    }
    const uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    const uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    return (uint32_t)(head - tail);
}

uint32_t sp1_midi_queue_dropped(const sp1_midi_spsc_queue_t *q) {
    if (q == NULL) {
        return 0U;
    }
    return __atomic_load_n(&q->dropped, __ATOMIC_RELAXED);
}

/* ── Portable MIDI Message Constructors & UMP32 Codec ────────────────────── */

sp1_midi_msg_t sp1_midi_msg_realtime(uint8_t status, uint32_t timestamp_us) {
    sp1_midi_msg_t msg;
    msg.timestamp_us = timestamp_us;
    msg.status = status;
    msg.data1 = 0U;
    msg.data2 = 0U;
    msg.length = 1U;
    return msg;
}

sp1_midi_msg_t sp1_midi_msg_note_on(uint8_t channel,
                                    uint8_t note,
                                    uint8_t velocity,
                                    uint32_t timestamp_us) {
    sp1_midi_msg_t msg;
    msg.timestamp_us = timestamp_us;
    msg.status = (uint8_t)(SP1_MIDI_STATUS_NOTE_ON | (channel & 0x0FU));
    msg.data1 = (uint8_t)(note & 0x7FU);
    msg.data2 = (uint8_t)(velocity & 0x7FU);
    msg.length = 3U;
    return msg;
}

sp1_midi_msg_t sp1_midi_msg_note_off(uint8_t channel,
                                     uint8_t note,
                                     uint8_t velocity,
                                     uint32_t timestamp_us) {
    sp1_midi_msg_t msg;
    msg.timestamp_us = timestamp_us;
    msg.status = (uint8_t)(SP1_MIDI_STATUS_NOTE_OFF | (channel & 0x0FU));
    msg.data1 = (uint8_t)(note & 0x7FU);
    msg.data2 = (uint8_t)(velocity & 0x7FU);
    msg.length = 3U;
    return msg;
}

sp1_midi_msg_t sp1_midi_msg_cc(uint8_t channel,
                               uint8_t cc,
                               uint8_t value,
                               uint32_t timestamp_us) {
    sp1_midi_msg_t msg;
    msg.timestamp_us = timestamp_us;
    msg.status = (uint8_t)(SP1_MIDI_STATUS_CC | (channel & 0x0FU));
    msg.data1 = (uint8_t)(cc & 0x7FU);
    msg.data2 = (uint8_t)(value & 0x7FU);
    msg.length = 3U;
    return msg;
}

bool sp1_midi_encode_ump32(const sp1_midi_msg_t *msg,
                           uint8_t group,
                           uint32_t *out_word0) {
    if (msg == NULL || out_word0 == NULL) {
        return false;
    }

    const uint32_t g = (uint32_t)(group & 0x0FU) << 24U;

    if (msg->status >= 0xF8U) {
        /* UMP MT=0x1: System Common & System Real-Time Messages */
        *out_word0 = ((uint32_t)SP1_UMP_MT_SYSTEM << 28U) | g |
                     ((uint32_t)msg->status << 16U);
        return true;
    }

    const uint8_t high_nibble = (uint8_t)(msg->status & 0xF0U);
    if (high_nibble == SP1_MIDI_STATUS_NOTE_OFF ||
        high_nibble == SP1_MIDI_STATUS_NOTE_ON ||
        high_nibble == SP1_MIDI_STATUS_CC) {
        /* UMP MT=0x2: MIDI 1.0 Channel Voice Messages */
        *out_word0 = ((uint32_t)SP1_UMP_MT_MIDI1_CV << 28U) | g |
                     ((uint32_t)msg->status << 16U) |
                     ((uint32_t)(msg->data1 & 0x7FU) << 8U) |
                     ((uint32_t)(msg->data2 & 0x7FU));
        return true;
    }

    return false;
}

bool sp1_midi_decode_ump32(uint32_t word0,
                           uint32_t timestamp_us,
                           sp1_midi_msg_t *out_msg) {
    if (out_msg == NULL) {
        return false;
    }

    const uint8_t mt = (uint8_t)((word0 >> 28U) & 0x0FU);
    const uint8_t status = (uint8_t)((word0 >> 16U) & 0xFFU);

    if (mt == SP1_UMP_MT_SYSTEM) {
        if (status == SP1_MIDI_STATUS_CLOCK ||
            status == SP1_MIDI_STATUS_START ||
            status == SP1_MIDI_STATUS_CONTINUE ||
            status == SP1_MIDI_STATUS_STOP) {
            *out_msg = sp1_midi_msg_realtime(status, timestamp_us);
            return true;
        }
        return false;
    }

    if (mt == SP1_UMP_MT_MIDI1_CV) {
        const uint8_t high_nibble = (uint8_t)(status & 0xF0U);
        if (high_nibble == SP1_MIDI_STATUS_NOTE_OFF ||
            high_nibble == SP1_MIDI_STATUS_NOTE_ON ||
            high_nibble == SP1_MIDI_STATUS_CC) {
            out_msg->timestamp_us = timestamp_us;
            out_msg->status = status;
            out_msg->data1 = (uint8_t)((word0 >> 8U) & 0x7FU);
            out_msg->data2 = (uint8_t)(word0 & 0x7FU);
            out_msg->length = 3U;
            return true;
        }
        return false;
    }

    return false;
}
