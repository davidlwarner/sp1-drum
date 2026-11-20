#ifndef SP1_HAL_MIDI_H
#define SP1_HAL_MIDI_H

/**
 * @file hal_midi.h
 * @brief Pure C11 lock-free SPSC MIDI queue & 24 PPQN Clock/Transport/Note/CC HAL.
 *
 * Real-time contract:
 *  - RX queue: producer = USB-C MIDI callback / host script; consumer = RT sequencer/audio core.
 *  - TX queue: producer = RT sequencer/audio core; consumer = background USB-C flush / host logger.
 *  - Power-of-2 ring capacity with wrap-safe uint32_t monotonic head/tail indices.
 *  - Zero blocking, zero heap allocation, zero logging inside push/pop.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_MIDI_PPQN               24U
#define SP1_MIDI_QUEUE_CAPACITY     64U    /* Must be a power of 2 */
#define SP1_MIDI_QUEUE_MASK         (SP1_MIDI_QUEUE_CAPACITY - 1U)

/* MIDI 1.0 status bytes */
#define SP1_MIDI_STATUS_NOTE_OFF    0x80U
#define SP1_MIDI_STATUS_NOTE_ON     0x90U
#define SP1_MIDI_STATUS_CC          0xB0U
#define SP1_MIDI_STATUS_CLOCK       0xF8U
#define SP1_MIDI_STATUS_START       0xFAU
#define SP1_MIDI_STATUS_CONTINUE    0xFBU
#define SP1_MIDI_STATUS_STOP        0xFCU

/* MIDI 2.0 UMP Message Types (bits 31..28 of word 0) */
#define SP1_UMP_MT_SYSTEM           0x1U   /* System Common & System Real-Time (32-bit) */
#define SP1_UMP_MT_MIDI1_CV         0x2U   /* MIDI 1.0 Channel Voice (32-bit) */

/* Compact timestamped MIDI message. Static RAM: 8 bytes per entry. */
typedef struct {
    uint32_t timestamp_us;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t length;    /* 1 for System Real-Time (0xF8/FA/FB/FC), 3 for Note/CC */
} sp1_midi_msg_t;

/* Lock-free SPSC ring buffer.
 * Static RAM: 64 * 8 + 12 = 524 bytes per queue (~1,048 bytes for RX + TX).
 * Indices are plain uint32_t for C++17 header compatibility; acquire/release atomics
 * are enforced in sp1_hal_common.c. */
typedef struct {
    sp1_midi_msg_t entries[SP1_MIDI_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
} sp1_midi_spsc_queue_t;

/* ── Portable Lock-Free SPSC Queue API (sp1_hal_common.c) ────────────────── */

void sp1_midi_queue_init(sp1_midi_spsc_queue_t *q);
bool sp1_midi_queue_push(sp1_midi_spsc_queue_t *q, const sp1_midi_msg_t *msg);
bool sp1_midi_queue_pop(sp1_midi_spsc_queue_t *q, sp1_midi_msg_t *out_msg);
uint32_t sp1_midi_queue_count(const sp1_midi_spsc_queue_t *q);
uint32_t sp1_midi_queue_dropped(const sp1_midi_spsc_queue_t *q);

/* ── Portable Message Constructors & UMP32 Helpers (sp1_hal_common.c) ────── */

sp1_midi_msg_t sp1_midi_msg_realtime(uint8_t status, uint32_t timestamp_us);
sp1_midi_msg_t sp1_midi_msg_note_on(uint8_t channel,
                                    uint8_t note,
                                    uint8_t velocity,
                                    uint32_t timestamp_us);
sp1_midi_msg_t sp1_midi_msg_note_off(uint8_t channel,
                                     uint8_t note,
                                     uint8_t velocity,
                                     uint32_t timestamp_us);
sp1_midi_msg_t sp1_midi_msg_cc(uint8_t channel,
                               uint8_t cc,
                               uint8_t value,
                               uint32_t timestamp_us);

/**
 * Encode a 1-byte System Real-Time or 3-byte MIDI 1.0 Channel Voice message into
 * a 32-bit Universal MIDI Packet (UMP word 0) on the given group (0..15).
 */
bool sp1_midi_encode_ump32(const sp1_midi_msg_t *msg,
                           uint8_t group,
                           uint32_t *out_word0);

/**
 * Decode a 32-bit UMP word 0 (MT=0x1 System Real-Time or MT=0x2 MIDI 1.0 Channel Voice)
 * into a portable sp1_midi_msg_t. Returns false for unsupported/ignored UMP types.
 */
bool sp1_midi_decode_ump32(uint32_t word0,
                           uint32_t timestamp_us,
                           sp1_midi_msg_t *out_msg);

/* ── Platform HAL MIDI I/O (sp1_hal_zephyr.c / sp1_hal_host.c) ───────────── */

int sp1_hal_midi_init(void);
bool sp1_hal_midi_is_ready(void);
void sp1_hal_midi_set_ready(bool ready);

/**
 * Enqueue a MIDI message for TX (non-blocking, RT-safe).
 */
bool sp1_hal_midi_send(const sp1_midi_msg_t *msg);

/**
 * Pop a received MIDI message from the RX queue (non-blocking, RT-safe).
 */
bool sp1_hal_midi_receive(sp1_midi_msg_t *out_msg);

/**
 * Drain queued TX messages to USB-C MIDI (Zephyr) or stdout/CSV log (Host).
 * Call from background/control context — never from hard-RT audio callbacks.
 */
void sp1_hal_midi_flush_tx(void);

/**
 * Thin delegation hook called by app/main.cpp on_midi_rx() for incoming UMP word 0.
 */
void sp1_hal_midi_on_rx_ump32(uint32_t word0);

#ifdef __cplusplus
}
#endif

#endif /* SP1_HAL_MIDI_H */
