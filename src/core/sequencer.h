#ifndef SP1_CORE_SEQUENCER_H
#define SP1_CORE_SEQUENCER_H

/**
 * @file sequencer.h
 * @brief Pure C11 4-Track Euclidean Sequencer with 1-Step Lookahead, Global Swing,
 *        and Per-Track Dilla Micro-Timing (-35 ms .. +35 ms).
 *
 * Real-time contract:
 *  - Zero Zephyr headers, zero heap allocation, zero blocking, zero runtime division
 *    inside sp1_seq_process_block().
 *  - Evaluates upcoming steps one 16th-note ahead into static 8-entry per-track lookahead
 *    rings stamped in 48 kHz sample frames.
 *  - Emits sample-accurate sp1_seq_trigger_event_t (for Milestone 3 DSP / host simulator)
 *    and outbound USB-C MIDI Note-On/Note-Off (0x90/0x80 on GM notes 36, 38, 42, 45).
 */

#include <stdbool.h>
#include <stdint.h>

#include "core/clock_sync.h"
#include "core/euclid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_SEQ_NUM_TRACKS              4U
#define SP1_SEQ_LOOKAHEAD_CAP           8U       /* Static 8-event lookahead ring per track */
#define SP1_SEQ_MAX_BLOCK_TRIGGERS      32U

#define SP1_SEQ_NUDGE_MIN_MS            (-35.0f)
#define SP1_SEQ_NUDGE_MAX_MS            35.0f
#define SP1_SEQ_NUDGE_MAX_FRAMES        1680     /* 35 ms @ 48 kHz (48 frames/ms) */

#define SP1_SEQ_SWING_MIN               0.50f    /* 50% straight 16ths */
#define SP1_SEQ_SWING_MAX               0.66f    /* 66% triplet swing feel */
#define SP1_SEQ_SWING_DEFAULT           0.50f

/* Default GM drum MIDI notes emitted on 0x90/0x80 (Channel 0 by default) */
#define SP1_SEQ_GM_NOTE_KICK            36U
#define SP1_SEQ_GM_NOTE_SNARE           38U
#define SP1_SEQ_GM_NOTE_HAT             42U
#define SP1_SEQ_GM_NOTE_PERC            45U

#define SP1_SEQ_VEL_ACCENT              120U
#define SP1_SEQ_VEL_NORMAL              96U
#define SP1_SEQ_MIDI_GATE_MAX_FRAMES    2400U    /* 50 ms max MIDI drum note gate */

typedef enum {
    SP1_TRACK_SPEED_DIV4 = 0,   /* 0.25x: 1/4-note steps (24 PPQN ticks/step, up to 8 bars) */
    SP1_TRACK_SPEED_DIV2 = 1,   /* 0.50x: 1/8-note steps (12 PPQN ticks/step, up to 4 bars) */
    SP1_TRACK_SPEED_1X   = 2,   /* 1.00x: 1/16-note steps (6 PPQN ticks/step, default) */
    SP1_TRACK_SPEED_2X   = 3    /* 2.00x: 1/32-note steps (3 PPQN ticks/step, fast rolls) */
} sp1_track_speed_t;

/* Sample-accurate internal trigger event handed to Milestone 3 DSP / host simulator. */
typedef struct {
    uint64_t sample_frame;      /* Absolute 48 kHz sample frame */
    uint32_t block_offset;      /* Intra-block sample offset [0..num_frames-1] */
    uint8_t track;              /* Track index [0..3] */
    uint8_t step_index;         /* Track local Euclidean step index [0..n-1] */
    uint8_t velocity;           /* Trigger velocity [1..127] */
    bool accent;                /* True on track downbeat accent */
} sp1_seq_trigger_event_t;

/* Internal lookahead ring slot. Static RAM: 16 bytes/entry (128 B/track, 512 B total). */
typedef struct {
    uint64_t fire_frame;
    uint32_t global_step;
    uint8_t track_step;
    uint8_t velocity;
    uint8_t accent;
    uint8_t reserved;
} sp1_seq_lookahead_event_t;

/* Per-track sequencer state. Static RAM: 168 bytes/track (672 B for 4 tracks). */
typedef struct {
    sp1_euclid_pattern_t pattern;
    int16_t nudge_frames;
    uint8_t midi_note;
    bool muted;

    uint8_t next_eval_step;         /* Local step index for next lookahead evaluation */
    uint8_t playhead_step;          /* Most recently reached grid step index */
    uint8_t committed_next_step;    /* Next local step after cur_grid_step (for Stop/Continue) */
    uint8_t speed;                  /* sp1_track_speed_t [0..3], default SP1_TRACK_SPEED_1X */

    uint8_t probability_u8;         /* Step trigger probability [0..255], 255 = 100% */
    bool midi_note_active;
    uint8_t active_midi_note;
    uint8_t pending_audition_vel;   /* One-shot live audition trigger velocity (0 = none) */

    uint64_t midi_note_off_frame;

    sp1_seq_lookahead_event_t ring[SP1_SEQ_LOOKAHEAD_CAP];
    uint8_t ring_count;
    uint8_t pad[3];
    uint32_t last_fired_slot_plus1; /* 1 + highest global 32nd-slot already fired */
    uint32_t dropped_events;
} sp1_seq_track_t;

/* Complete 4-track Euclidean sequencer + embedded 24 PPQN clock.
 * Static RAM: ~808 bytes total (zero dynamic alloc). */
typedef struct {
    sp1_clock_sync_t clock;
    sp1_seq_track_t tracks[SP1_SEQ_NUM_TRACKS];

    float swing_ratio;          /* Global swing [0.50f..0.66f] */
    float swing_offset_scale;   /* Precomputed 2.0f * (swing_ratio - 0.50f) in [0.0f..0.32f] */

    uint8_t midi_channel;       /* Outbound MIDI channel [0..15], default 0 (0x90/0x80) */
    uint8_t sub_tick;           /* Tick offset within current 32nd-note slot [0..2] */
    bool has_scheduled_steps;
    bool armed_for_start;

    uint32_t cur_grid_step;     /* Monotonic 32nd-note slot index since Start */
    uint32_t scheduled_up_to_step;
    uint64_t last_grid_frame;
    uint32_t prob_rng;          /* Deterministic xorshift32 PRNG for < 100% probability */
} sp1_sequencer_t;

/**
 * Initialize the 4-track Euclidean sequencer with default musical patterns,
 * 120.00 BPM internal master clock, 50% straight swing, 0 ms nudge, and GM notes 36/38/42/45.
 */
void sp1_seq_init(sp1_sequencer_t *seq);

/**
 * Transport & global tempo/swing/MIDI controls.
 */
void sp1_seq_start(sp1_sequencer_t *seq);
void sp1_seq_stop(sp1_sequencer_t *seq);
bool sp1_seq_is_running(const sp1_sequencer_t *seq);

void sp1_seq_set_bpm_x100(sp1_sequencer_t *seq, uint32_t bpm_x100);
void sp1_seq_set_bpm(sp1_sequencer_t *seq, float bpm);
uint32_t sp1_seq_get_bpm_x100(const sp1_sequencer_t *seq);

void sp1_seq_set_tempo_bend(sp1_sequencer_t *seq, int8_t bend_dir);
int8_t sp1_seq_get_tempo_bend(const sp1_sequencer_t *seq);

void sp1_seq_set_swing(sp1_sequencer_t *seq, float swing_ratio);
float sp1_seq_get_swing(const sp1_sequencer_t *seq);

void sp1_seq_set_midi_channel(sp1_sequencer_t *seq, uint8_t channel_0_to_15);

/**
 * Per-track pattern, clock speed, probability, mute, audition, MIDI note, and Dilla micro-timing.
 */
void sp1_seq_set_track_pattern(sp1_sequencer_t *seq,
                               uint8_t track,
                               uint8_t pulses,
                               uint8_t steps,
                               uint8_t rotation);
void sp1_seq_set_track_pulses(sp1_sequencer_t *seq, uint8_t track, uint8_t pulses);
void sp1_seq_set_track_steps(sp1_sequencer_t *seq, uint8_t track, uint8_t steps);
void sp1_seq_set_track_rotation(sp1_sequencer_t *seq, uint8_t track, uint8_t rotation);

void sp1_seq_set_track_speed(sp1_sequencer_t *seq, uint8_t track, sp1_track_speed_t speed);
sp1_track_speed_t sp1_seq_get_track_speed(const sp1_sequencer_t *seq, uint8_t track);

void sp1_seq_set_track_probability(sp1_sequencer_t *seq, uint8_t track, float prob_0_to_1);
float sp1_seq_get_track_probability(const sp1_sequencer_t *seq, uint8_t track);

void sp1_seq_trigger_audition(sp1_sequencer_t *seq, uint8_t track, uint8_t velocity);

void sp1_seq_set_track_nudge_ms(sp1_sequencer_t *seq, uint8_t track, float nudge_ms);
int16_t sp1_seq_get_track_nudge_frames(const sp1_sequencer_t *seq, uint8_t track);
float sp1_seq_get_track_nudge_ms(const sp1_sequencer_t *seq, uint8_t track);

void sp1_seq_set_track_mute(sp1_sequencer_t *seq, uint8_t track, bool muted);
void sp1_seq_toggle_track_mute(sp1_sequencer_t *seq, uint8_t track);
bool sp1_seq_is_track_muted(const sp1_sequencer_t *seq, uint8_t track);

void sp1_seq_set_track_midi_note(sp1_sequencer_t *seq, uint8_t track, uint8_t note);
const sp1_euclid_pattern_t *sp1_seq_get_track_pattern(const sp1_sequencer_t *seq, uint8_t track);

/**
 * Access embedded 24 PPQN clock engine (for mode/tick inspection or passthrough registration).
 */
sp1_clock_sync_t *sp1_seq_clock(sp1_sequencer_t *seq);
const sp1_clock_sync_t *sp1_seq_clock_const(const sp1_sequencer_t *seq);

/**
 * Process one 48 kHz audio render block [block_start_frame, block_start_frame + num_frames).
 * Advances the 24 PPQN clock, updates lookahead rings with global swing + per-track
 * Dilla micro-timing, sends outbound MIDI Note-On/Note-Off, and outputs chronological
 * sample-accurate trigger events into out_triggers.
 *
 * Returns the number of trigger events written to out_triggers (up to max_triggers).
 */
uint32_t sp1_seq_process_block(sp1_sequencer_t *seq,
                               uint64_t block_start_frame,
                               uint32_t num_frames,
                               sp1_seq_trigger_event_t *out_triggers,
                               uint32_t max_triggers);

#ifdef __cplusplus
}
#endif

#endif /* SP1_CORE_SEQUENCER_H */
