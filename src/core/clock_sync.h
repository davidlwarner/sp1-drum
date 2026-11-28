#ifndef SP1_CORE_CLOCK_SYNC_H
#define SP1_CORE_CLOCK_SYNC_H

/**
 * @file clock_sync.h
 * @brief Pure C11 24 PPQN Master & Auto-Follower MIDI Clock Sync Engine.
 *
 * Real-time contract:
 *  - Zero Zephyr headers, zero heap allocation, zero blocking, zero runtime division
 *    inside sp1_clock_process_block().
 *  - Sample-accurate 48 kHz timebase using Q32.32 fixed-point tick intervals.
 *  - Internal master mode broadcasts 0xFA (Start), 0xF8 (Clock), 0xFC (Stop) via
 *    sp1_hal_midi_send().
 *  - Auto-follower mode locks onto incoming 0xF8/0xFA/0xFB/0xFC via sp1_hal_midi_receive()
 *    with 1-pole PLL smoothing and clean fallback to internal master after 500 ms timeout.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/hal_audio.h"
#include "hal/hal_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_CLOCK_PPQN                  SP1_MIDI_PPQN               /* 24 ticks/quarter */
#define SP1_CLOCK_TICKS_PER_16TH        6U                          /* 24 / 4 = 6 ticks/16th */
#define SP1_CLOCK_BPM_MIN_X100          4000U                       /* 40.00 BPM */
#define SP1_CLOCK_BPM_MAX_X100          24000U                      /* 240.00 BPM */
#define SP1_CLOCK_BPM_DEFAULT_X100      12000U                      /* 120.00 BPM */
#define SP1_CLOCK_EXT_TIMEOUT_MS        500U                        /* 500 ms follower timeout */
#define SP1_CLOCK_EXT_TIMEOUT_FRAMES    24000ULL                    /* 500 ms @ 48,000 Hz */
#define SP1_CLOCK_MAX_BLOCK_EVENTS      16U

typedef enum {
    SP1_CLOCK_MODE_INTERNAL = 0,
    SP1_CLOCK_MODE_EXTERNAL = 1
} sp1_clock_mode_t;

typedef enum {
    SP1_CLOCK_EVT_NONE = 0,
    SP1_CLOCK_EVT_START,
    SP1_CLOCK_EVT_CONTINUE,
    SP1_CLOCK_EVT_STOP,
    SP1_CLOCK_EVT_TICK,
    SP1_CLOCK_EVT_MODE_CHANGE
} sp1_clock_event_type_t;

typedef struct {
    sp1_clock_event_type_t type;
    uint64_t sample_frame;
    uint32_t tick_index;
    uint8_t midi_tx_status;           /* Non-zero (0xFA/0xF8) when internal master MIDI TX applies */
} sp1_clock_event_t;

/* Optional callback receiving non-clock MIDI RX messages (Notes/CCs) popped while
 * draining sp1_hal_midi_receive() so downstream modules never lose incoming MIDI. */
typedef void (*sp1_clock_midi_rx_cb_t)(const sp1_midi_msg_t *msg, void *user_ctx);

/* 24 PPQN Master & Auto-Follower Clock State.
 * Static RAM: 96 bytes on 64-bit host / ~88 bytes on Cortex-M4F. */
typedef struct {
    sp1_clock_mode_t mode;
    bool running;
    bool pending_start;
    bool ext_bpm_locked;
    bool ext_has_prev_tick;
    bool defer_midi_tx;               /* True when sp1_seq_process_block() interleaves MIDI TX */

    uint32_t bpm_x100;                /* Configured/active internal tempo [4000..24000], 0 if ext-adopted */
    uint32_t tick_index;              /* Monotonic 24 PPQN tick count since Start */
    uint32_t prev_ext_rx_ts_us;       /* Timestamp of previous incoming 0xF8 message */

    uint64_t current_frame;           /* End frame of most recently processed block */
    uint64_t frames_per_tick_q32;     /* Active Q32.32 frames per 24 PPQN tick */
    uint64_t next_tick_frame;         /* Integer 48 kHz frame of next internal tick */
    uint32_t next_tick_frac;          /* Fractional Q32 remainder of next internal tick */
    int8_t tempo_bend_dir;            /* Momentary DJ bend: -1 (-6%), 0 (nominal), +1 (+6%) */
    uint8_t reserved[3];

    uint64_t base_frames_per_tick_q32;/* Unbent internal Q32.32 frames per tick */
    uint64_t last_ext_rx_frame;       /* Sample frame of most recent external clock/transport msg */
    uint64_t prev_ext_tick_frame;     /* Sample frame of previous external 0xF8 tick */
    uint64_t ext_frames_per_tick_q32; /* PLL-smoothed external Q32.32 frames per tick */

    sp1_clock_midi_rx_cb_t passthrough_cb;
    void *passthrough_ctx;
} sp1_clock_sync_t;

/**
 * Exact division-free conversion from 48 kHz sample frame to microseconds
 * (floor(frame * 125 / 6)) for frame <= 0xFFFFFFFF (~24.85 hours).
 */
uint32_t sp1_clock_frame_to_us(uint64_t sample_frame);

/**
 * Initialize clock engine in INTERNAL master mode at 120.00 BPM, stopped.
 */
void sp1_clock_init(sp1_clock_sync_t *clk);

/**
 * Register an optional passthrough callback for non-clock MIDI RX messages.
 */
void sp1_clock_set_midi_passthrough(sp1_clock_sync_t *clk,
                                    sp1_clock_midi_rx_cb_t cb,
                                    void *user_ctx);

/**
 * Set internal master tempo in centi-BPM (clamped to [4000..24000] = 40.00..240.00 BPM).
 * Safe to call at control rate; updates Q32.32 tick interval without double-firing.
 */
void sp1_clock_set_bpm_x100(sp1_clock_sync_t *clk, uint32_t bpm_x100);

/**
 * Convenience float BPM setter (clamped to [40.0f..240.0f]).
 */
void sp1_clock_set_bpm(sp1_clock_sync_t *clk, float bpm);

/**
 * Query effective tempo in centi-BPM (returns locked external tempo when following).
 * Control/UI inspection helper — not intended for per-sample loops.
 */
uint32_t sp1_clock_get_bpm_x100(const sp1_clock_sync_t *clk);

/**
 * Query active 16th-note step duration in 48 kHz sample frames (6 * frames_per_tick).
 * Division-free RT-safe helper used by the 1-step lookahead scheduler.
 */
uint32_t sp1_clock_get_step_frames(const sp1_clock_sync_t *clk);

/**
 * Set momentary DJ tempo bend (-1 = -6% drag, 0 = nominal, +1 = +6% push) in internal
 * clock mode without altering the stored nominal BPM.
 */
void sp1_clock_set_tempo_bend(sp1_clock_sync_t *clk, int8_t bend_dir);
int8_t sp1_clock_get_tempo_bend(const sp1_clock_sync_t *clk);

sp1_clock_mode_t sp1_clock_get_mode(const sp1_clock_sync_t *clk);
bool sp1_clock_is_running(const sp1_clock_sync_t *clk);
uint32_t sp1_clock_get_tick_index(const sp1_clock_sync_t *clk);

/**
 * Arm transport Start (emits 0xFA + tick 0 on the next processed audio block in Master mode).
 */
void sp1_clock_start(sp1_clock_sync_t *clk);

/**
 * Stop transport immediately (broadcasts 0xFC at current_frame in Master mode).
 */
void sp1_clock_stop(sp1_clock_sync_t *clk);

/**
 * Advance the 24 PPQN clock across one audio render block [block_start_frame, block_start_frame + num_frames).
 * Drains incoming MIDI via sp1_hal_midi_receive(), emits master MIDI via sp1_hal_midi_send(),
 * handles 500 ms external timeout fallback, and writes chronological clock events into out_events.
 *
 * Returns the number of events written to out_events (up to max_events).
 */
uint32_t sp1_clock_process_block(sp1_clock_sync_t *clk,
                                 uint64_t block_start_frame,
                                 uint32_t num_frames,
                                 sp1_clock_event_t *out_events,
                                 uint32_t max_events);

#ifdef __cplusplus
}
#endif

#endif /* SP1_CORE_CLOCK_SYNC_H */
