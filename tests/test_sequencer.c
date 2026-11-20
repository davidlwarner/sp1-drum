#include "test_harness.h"
#include "core/sequencer.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void init_host_hal(const char *csv_path) {
    const sp1_hal_host_config_t cfg = {
        .wav_path = NULL,
        .midi_csv_path = csv_path,
        .midi_log_stdout = false,
    };
    sp1_hal_host_configure(&cfg);
    TEST_ASSERT_EQ_INT(0, sp1_hal_init());
}

static void test_straight_grid_and_gm_drum_midi_notes(void) {
    const char *csv_path = "test_seq_gm_midi.csv";
    init_host_hal(csv_path);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 12000U); /* 120 BPM => 6000 frames / 16th step */
    sp1_seq_set_swing(&seq, 0.50f);

    /* All 4 tracks fire on every step of 4-step pattern (0, 1, 2, 3) */
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_set_track_pattern(&seq, t, 4U, 4U, 0U);
        sp1_seq_set_track_nudge_ms(&seq, t, 0.0f);
    }

    sp1_seq_start(&seq);

    uint64_t hits[SP1_SEQ_NUM_TRACKS][16];
    uint32_t hit_counts[SP1_SEQ_NUM_TRACKS] = { 0U, 0U, 0U, 0U };

    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];
    /* Render 1.0 s = 48,000 frames = 8 exact 16th steps (frames 0, 6000, ..., 42000) */
    for (uint64_t frame = 0ULL; frame < 48000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            const uint8_t t = trigs[i].track;
            TEST_ASSERT(t < SP1_SEQ_NUM_TRACKS);
            TEST_ASSERT(trigs[i].block_offset < SP1_AUDIO_BLOCK_FRAMES);
            TEST_ASSERT_EQ_U32((uint32_t)(trigs[i].sample_frame - frame), trigs[i].block_offset);
            if (hit_counts[t] < 16U) {
                hits[t][hit_counts[t]++] = trigs[i].sample_frame;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        TEST_ASSERT_EQ_U32(8U, hit_counts[t]);
        for (uint32_t s = 0U; s < 8U; ++s) {
            TEST_ASSERT_EQ_U32(s * 6000U, (uint32_t)hits[t][s]);
        }
    }

    /* Verify outbound GM drum MIDI Note-On (0x90) / Note-Off (0x80) for notes 36, 38, 42, 45 */
    sp1_hal_host_stats_t stats;
    sp1_hal_host_get_stats(&stats);
    TEST_ASSERT_EQ_U32(32U, stats.midi_tx_note_on_count);
    TEST_ASSERT_EQ_U32(32U, stats.midi_tx_note_off_count);
    TEST_ASSERT_EQ_U32(0U, stats.midi_tx_dropped);

    FILE *fp = fopen(csv_path, "rb");
    TEST_ASSERT(fp != NULL);
    uint32_t note_on_gm[4] = { 0U, 0U, 0U, 0U };
    uint32_t note_off_gm[4] = { 0U, 0U, 0U, 0U };
    char line[128];
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        unsigned ts = 0U, st = 0U, d1 = 0U, d2 = 0U;
        unsigned long long sf = 0ULL;
        char type[32];
        if (sscanf(line, "%u,%llu,0x%X,%u,%u,%31s", &ts, &sf, &st, &d1, &d2, type) == 6) {
            const uint8_t gm[4] = { 36U, 38U, 42U, 45U };
            for (uint32_t k = 0U; k < 4U; ++k) {
                if (d1 == gm[k]) {
                    if (st == SP1_MIDI_STATUS_NOTE_ON) {
                        note_on_gm[k]++;
                    } else if (st == SP1_MIDI_STATUS_NOTE_OFF) {
                        note_off_gm[k]++;
                    }
                }
            }
        }
    }
    fclose(fp);
    (void)remove(csv_path);

    for (uint32_t k = 0U; k < 4U; ++k) {
        TEST_ASSERT_EQ_U32(8U, note_on_gm[k]);
        TEST_ASSERT_EQ_U32(8U, note_off_gm[k]);
    }
}

static void test_dilla_negative_and_positive_nudge_stacked_with_swing(void) {
    init_host_hal(NULL);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 12000U); /* step_frames = 6000 */
    sp1_seq_set_swing(&seq, 0.66f);     /* +1920 frames on 1-based even 16ths (odd indices 1,3,5) */

    /* Track 0: 0 ms anchor; Track 1: -20 ms (-960); Track 2: +20 ms (+960); Track 3: muted */
    sp1_seq_set_track_pattern(&seq, 0U, 16U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);

    sp1_seq_set_track_pattern(&seq, 1U, 16U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, -20.0f);
    TEST_ASSERT_EQ_INT(-960, sp1_seq_get_track_nudge_frames(&seq, 1U));

    sp1_seq_set_track_pattern(&seq, 2U, 16U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, 20.0f);
    TEST_ASSERT_EQ_INT(960, sp1_seq_get_track_nudge_frames(&seq, 2U));

    sp1_seq_set_track_pattern(&seq, 3U, 0U, 16U, 0U);

    sp1_seq_start(&seq);

    uint64_t hits[3][16];
    uint32_t counts[3] = { 0U, 0U, 0U };
    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];

    /* Render 1.25 s (60,000 frames -> covers steps 0..8 cleanly including +20ms late hits) */
    for (uint64_t frame = 0ULL; frame < 60000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            if (trigs[i].track < 3U && counts[trigs[i].track] < 16U) {
                hits[trigs[i].track][counts[trigs[i].track]++] = trigs[i].sample_frame;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    /* Verify exact sample-frame timestamps for steps 0..5:
     * Nominal grid: 0, 6000, 12000, 18000, 24000, 30000
     * Swing (66%):  0, +1920,    0, +1920,     0, +1920 */
    const uint64_t exp_t0[6] = { 0ULL, 7920ULL, 12000ULL, 19920ULL, 24000ULL, 31920ULL };
    const uint64_t exp_t1[6] = { 0ULL, 6960ULL, 11040ULL, 18960ULL, 23040ULL, 30960ULL };
    const uint64_t exp_t2[6] = { 960ULL, 8880ULL, 12960ULL, 20880ULL, 24960ULL, 32880ULL };

    TEST_ASSERT(counts[0] >= 6U);
    TEST_ASSERT(counts[1] >= 6U);
    TEST_ASSERT(counts[2] >= 6U);

    for (uint32_t s = 0U; s < 6U; ++s) {
        TEST_ASSERT_EQ_U32((uint32_t)exp_t0[s], (uint32_t)hits[0][s]);
        TEST_ASSERT_EQ_U32((uint32_t)exp_t1[s], (uint32_t)hits[1][s]);
        TEST_ASSERT_EQ_U32((uint32_t)exp_t2[s], (uint32_t)hits[2][s]);
    }
}

static void test_negative_nudge_step0_clamp_and_bar_wrap_early_fire(void) {
    /* Verify that a negative Dilla nudge (-20 ms = -960 frames) on local Step 0:
     *  - Clamps safely to tick_frame (frame 0) on initial Start (global Step 0)
     *  - Fires genuinely early (-960 frames before the grid) on subsequent bar wraps:
     *    Step 16 (96,000 - 960 = 95,040) and Step 32 (192,000 - 960 = 191,040)! */
    init_host_hal(NULL);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 12000U); /* 16 steps = 96,000 frames = 2.0 s/bar */
    sp1_seq_set_swing(&seq, 0.50f);

    /* Track 0: E(1, 16, rot=0) -> fires ONLY on local Step 0 of each 16-step bar */
    sp1_seq_set_track_pattern(&seq, 0U, 1U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, -20.0f);

    /* Silence tracks 1..3 */
    for (uint8_t t = 1U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_set_track_pattern(&seq, t, 0U, 16U, 0U);
    }

    sp1_seq_start(&seq);

    uint64_t downbeats[4];
    uint8_t step_indices[4];
    uint32_t count = 0U;
    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];

    /* Render 4.1 s (196,800 frames) covering Bar 1 (0), Bar 2 (96,000), Bar 3 (192,000) */
    for (uint64_t frame = 0ULL; frame < 196800ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            TEST_ASSERT_EQ_INT(0, trigs[i].track);
            if (count < 4U) {
                downbeats[count] = trigs[i].sample_frame;
                step_indices[count] = trigs[i].step_index;
                count++;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    TEST_ASSERT_EQ_U32(3U, count);
    TEST_ASSERT_EQ_INT(0, step_indices[0]);
    TEST_ASSERT_EQ_INT(0, step_indices[1]);
    TEST_ASSERT_EQ_INT(0, step_indices[2]);

    /* Initial Start: clamped safely at frame 0 (no underflow) */
    TEST_ASSERT_EQ_U32(0U, (uint32_t)downbeats[0]);
    /* Bar 2 wrap (Step 16): genuinely early by -960 frames (-20 ms) */
    TEST_ASSERT_EQ_U32(96000U - 960U, (uint32_t)downbeats[1]);
    /* Bar 3 wrap (Step 32): genuinely early by -960 frames (-20 ms) */
    TEST_ASSERT_EQ_U32(192000U - 960U, (uint32_t)downbeats[2]);
}

static void test_tempo_changes_zero_drops_zero_doubles_and_note_cleanup(void) {
    init_host_hal(NULL);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 6000U); /* Start slow at 60 BPM (12,000 frames/step) */
    sp1_seq_set_swing(&seq, 0.62f);

    /* All 4 tracks hit every step of a 16-step pattern with diverse Dilla nudges:
     *   Track 0:   0 ms
     *   Track 1: -20 ms (-960 frames)
     *   Track 2: +20 ms (+960 frames)
     *   Track 3: -35 ms (-1680 frames, extreme rush!) */
    const float nudges[4] = { 0.0f, -20.0f, 20.0f, -35.0f };
    for (uint8_t t = 0U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        sp1_seq_set_track_pattern(&seq, t, 16U, 16U, 0U);
        sp1_seq_set_track_nudge_ms(&seq, t, nudges[t]);
    }

    sp1_seq_start(&seq);

    uint32_t hit_count[4] = { 0U, 0U, 0U, 0U };
    uint8_t expected_step[4] = { 0U, 0U, 0U, 0U };
    uint64_t last_fire_frame[4] = { 0ULL, 0ULL, 0ULL, 0ULL };
    bool has_fired[4] = { false, false, false, false };

    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];

    /* Render 3.0 s (144,000 frames) with abrupt tempo jumps mid-step:
     *   0.00 s:  60 BPM
     *   0.35 s: 180 BPM (3x abrupt speedup mid-step!)
     *   1.20 s:  80 BPM (abrupt slowdown!)
     *   2.00 s: 150 BPM (abrupt jump!) */
    for (uint64_t frame = 0ULL; frame < 144000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        if (frame == 16800ULL) {
            sp1_seq_set_bpm_x100(&seq, 18000U);
        } else if (frame == 57600ULL) {
            sp1_seq_set_bpm_x100(&seq, 8000U);
        } else if (frame == 96000ULL) {
            sp1_seq_set_bpm_x100(&seq, 15000U);
        }

        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            const uint8_t t = trigs[i].track;
            /* Confirm strict step-by-step continuity (zero dropped, zero double-fired steps!) */
            TEST_ASSERT_EQ_INT(expected_step[t], trigs[i].step_index);
            expected_step[t] = sp1_euclid_next_step(expected_step[t], 16U);

            /* Confirm strictly non-decreasing chronological fire timestamps per track */
            if (has_fired[t]) {
                TEST_ASSERT(trigs[i].sample_frame > last_fire_frame[t]);
            }
            last_fire_frame[t] = trigs[i].sample_frame;
            has_fired[t] = true;
            hit_count[t]++;
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    /* Every track must have stayed in lockstep within +/-1 lookahead step at the end boundary */
    TEST_ASSERT(hit_count[0] >= 20U);
    for (uint8_t t = 1U; t < SP1_SEQ_NUM_TRACKS; ++t) {
        const int32_t diff = (int32_t)hit_count[t] - (int32_t)hit_count[0];
        TEST_ASSERT(diff >= -1 && diff <= 1);
        TEST_ASSERT_EQ_U32(0U, seq.tracks[t].dropped_events);
    }

    /* Zero stuck MIDI notes after Stop: Note-On count == Note-Off count! */
    sp1_hal_host_stats_t stats;
    sp1_hal_host_get_stats(&stats);
    TEST_ASSERT_EQ_U32(stats.midi_tx_note_on_count, stats.midi_tx_note_off_count);
    TEST_ASSERT_EQ_U32(0U, stats.midi_tx_dropped);
}

static void test_polymeter_and_phase_preserving_mute(void) {
    init_host_hal(NULL);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 12000U); /* 6000 frames/step */
    sp1_seq_set_swing(&seq, 0.50f);

    /* Track 0: 5-step polymeter E(1, 5, rot=0) -> fires at global steps 0, 5, 10, 15 */
    sp1_seq_set_track_pattern(&seq, 0U, 1U, 5U, 0U);
    /* Track 1: 4-step pattern E(4, 4, rot=0), muted during steps 1..2, unmuted for step 3+ */
    sp1_seq_set_track_pattern(&seq, 1U, 4U, 4U, 0U);
    sp1_seq_set_track_pattern(&seq, 2U, 0U, 16U, 0U);
    sp1_seq_set_track_pattern(&seq, 3U, 0U, 16U, 0U);

    sp1_seq_start(&seq);

    uint64_t poly_frames[8];
    uint32_t poly_count = 0U;
    uint8_t t1_steps[16];
    uint32_t t1_count = 0U;

    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];
    /* 16 steps = 96,000 frames */
    for (uint64_t frame = 0ULL; frame < 96000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        /* Mute Track 1 from frame 3072 (32 * 96) to 15072 (157 * 96) -> suppresses steps 1 & 2 */
        if (frame == 3072ULL) {
            sp1_seq_set_track_mute(&seq, 1U, true);
        } else if (frame == 15072ULL) {
            sp1_seq_set_track_mute(&seq, 1U, false);
        }

        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            if (trigs[i].track == 0U && poly_count < 8U) {
                poly_frames[poly_count++] = trigs[i].sample_frame;
            } else if (trigs[i].track == 1U && t1_count < 16U) {
                t1_steps[t1_count++] = trigs[i].step_index;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    /* 5-step polymeter fired cleanly at steps 0, 5, 10, 15 */
    TEST_ASSERT_EQ_U32(4U, poly_count);
    TEST_ASSERT_EQ_U32(0U, (uint32_t)poly_frames[0]);
    TEST_ASSERT_EQ_U32(30000U, (uint32_t)poly_frames[1]);
    TEST_ASSERT_EQ_U32(60000U, (uint32_t)poly_frames[2]);
    TEST_ASSERT_EQ_U32(90000U, (uint32_t)poly_frames[3]);

    /* Track 1 skipped steps 1 & 2 while muted, then resumed in-phase at local step 3 */
    TEST_ASSERT_EQ_U32(14U, t1_count);
    TEST_ASSERT_EQ_INT(0, t1_steps[0]);
    TEST_ASSERT_EQ_INT(3, t1_steps[1]);
    TEST_ASSERT_EQ_INT(0, t1_steps[2]);
}

static void test_mid_step_tempo_jump_180_to_60_and_60_to_180_reanchoring(void) {
    /* 1. Abrupt slowdown mid-step (180 BPM -> 60 BPM):
     *    At 180 BPM (tick = 666.67, step = 4000), Step 1 is initially predicted at frame 4000.
     *    Jumping to 60 BPM (tick = 2000, step = 12000) at frame 960 (after Tick 1 at 666,
     *    next_tick_frame = 1333, sub_tick = 2) pushes Step 1's actual grid tick to
     *    1333 + 4 * 2000 = 9333. Pending Step 1 lookahead hits must re-anchor to 9333
     *    rather than firing prematurely around frame 4000. */
    init_host_hal(NULL);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 18000U);
    sp1_seq_set_swing(&seq, 0.50f);

    sp1_seq_set_track_pattern(&seq, 0U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);
    sp1_seq_set_track_pattern(&seq, 1U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, -20.0f); /* -960 frames */
    sp1_seq_set_track_pattern(&seq, 2U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, 20.0f);  /* +960 frames */
    sp1_seq_set_track_pattern(&seq, 3U, 0U, 4U, 0U);

    sp1_seq_start(&seq);

    uint64_t hits_slow[3][4];
    uint32_t counts_slow[3] = { 0U, 0U, 0U };
    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];

    for (uint64_t frame = 0ULL; frame < 12000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        if (frame == 960ULL) {
            sp1_seq_set_bpm_x100(&seq, 6000U);
        }
        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            const uint8_t t = trigs[i].track;
            if (t < 3U && counts_slow[t] < 4U) {
                hits_slow[t][counts_slow[t]++] = trigs[i].sample_frame;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    for (uint8_t t = 0U; t < 3U; ++t) {
        TEST_ASSERT_EQ_U32(2U, counts_slow[t]);
    }
    /* Step 1 re-anchored cleanly to new 60 BPM grid boundary at frame 9333 */
    TEST_ASSERT_EQ_U32(9333U - 960U, (uint32_t)hits_slow[1][1]);
    TEST_ASSERT_EQ_U32(9333U, (uint32_t)hits_slow[0][1]);
    TEST_ASSERT_EQ_U32(9333U + 960U, (uint32_t)hits_slow[2][1]);

    /* 2. Abrupt speedup mid-step (60 BPM -> 180 BPM):
     *    At 60 BPM (step = 12000), Step 1 is initially predicted at frame 12000.
     *    Jumping to 180 BPM at frame 960 (sub_tick = 1) pulls Tick 1 to 960 + 666 = 1626
     *    and Step 1's grid boundary (Tick 6) to 4960. Track 1 (-20 ms = -960 frames)
     *    must re-anchor to 4960 - 960 = 4000 and fire genuinely early before frame 4960! */
    init_host_hal(NULL);
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 6000U);
    sp1_seq_set_swing(&seq, 0.50f);

    sp1_seq_set_track_pattern(&seq, 0U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);
    sp1_seq_set_track_pattern(&seq, 1U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, -20.0f); /* -960 frames */
    sp1_seq_set_track_pattern(&seq, 2U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, 20.0f);  /* +960 frames */
    sp1_seq_set_track_pattern(&seq, 3U, 0U, 4U, 0U);

    sp1_seq_start(&seq);

    uint64_t hits_fast[3][4];
    uint32_t counts_fast[3] = { 0U, 0U, 0U };

    for (uint64_t frame = 0ULL; frame < 6528ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        if (frame == 960ULL) {
            sp1_seq_set_bpm_x100(&seq, 18000U);
        }
        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            const uint8_t t = trigs[i].track;
            if (t < 3U && counts_fast[t] < 4U) {
                hits_fast[t][counts_fast[t]++] = trigs[i].sample_frame;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    TEST_ASSERT(counts_fast[0] >= 2U);
    TEST_ASSERT(counts_fast[1] >= 2U);
    TEST_ASSERT(counts_fast[2] >= 2U);
    TEST_ASSERT_EQ_U32(4960U - 960U, (uint32_t)hits_fast[1][1]);
    TEST_ASSERT_EQ_U32(4960U, (uint32_t)hits_fast[0][1]);
    TEST_ASSERT_EQ_U32(4960U + 960U, (uint32_t)hits_fast[2][1]);
}

static void test_external_stop_continue_with_negative_nudge_and_500ms_timeout_continuity(void) {
    init_host_hal(NULL);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 12000U); /* 1000 frames/tick, 6000 frames/16th step */
    sp1_seq_set_swing(&seq, 0.50f);

    /* Track 0: 0 ms anchor; Track 1: -35 ms (-1680 frames) rush on every step of E(4,4) */
    sp1_seq_set_track_pattern(&seq, 0U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);
    sp1_seq_set_track_pattern(&seq, 1U, 4U, 4U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, -35.0f);
    sp1_seq_set_track_pattern(&seq, 2U, 0U, 4U, 0U);
    sp1_seq_set_track_pattern(&seq, 3U, 0U, 4U, 0U);

    uint8_t expected_step[2] = { 0U, 0U };
    uint32_t hit_counts[2] = { 0U, 0U };
    uint64_t hit_frames[2][16];
    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];

    /* Render 45,024 frames at 96-frame blocks:
     *  - External 0xFA + Ticks 0..12 at ~1000-frame intervals (120 BPM):
     *    * Tick 0..4 at blocks covering 0, 1000, 2000, 3000, 4000
     *      (Track 1 fires Step 1 early at ~4320!)
     *    * Tick 5 + 0xFC (STOP) at block covering 5000 (AFTER Track 1 already fired Step 1!)
     *    * 0xFB (CONTINUE) + Tick 6 at block covering 6000
     *      (Track 0 fires Step 1; Track 1 MUST NOT double-fire Step 1!)
     *    * Tick 7 + 0xFC (STOP) at block covering 7000 (BEFORE Track 1 fires Step 2!)
     *    * 0xFB (CONTINUE) + Tick 8 at block covering 8000 (mid-step resume at sub_tick=2!)
     *    * Ticks 9..12 at blocks covering 9000, 10000, 11000, 12000
     *      (Track 1 fires Step 2 early before Tick 12!)
     *  - External clock stops after Tick 12 (~frame 11904); 500 ms (24,000-frame) timeout
     *    falls back to INTERNAL mode at ~frame 35904 without a 1-step silent gap! */
    uint32_t next_ext_tick = 0U;

    for (uint64_t frame = 0ULL; frame < 45024ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        const uint64_t block_end = frame + SP1_AUDIO_BLOCK_FRAMES;
        const uint64_t target_tick_frame = (uint64_t)next_ext_tick * 1000ULL;

        if (next_ext_tick <= 12U && target_tick_frame >= frame && target_tick_frame < block_end) {
            if (next_ext_tick == 0U) {
                const sp1_midi_msg_t start_m =
                    sp1_midi_msg_realtime(SP1_MIDI_STATUS_START, sp1_clock_frame_to_us(target_tick_frame));
                TEST_ASSERT(sp1_hal_host_inject_rx_midi(&start_m));
            } else if (next_ext_tick == 6U || next_ext_tick == 8U) {
                const sp1_midi_msg_t cont_m =
                    sp1_midi_msg_realtime(SP1_MIDI_STATUS_CONTINUE, sp1_clock_frame_to_us(target_tick_frame));
                TEST_ASSERT(sp1_hal_host_inject_rx_midi(&cont_m));
            }

            const sp1_midi_msg_t clk_m =
                sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, sp1_clock_frame_to_us(target_tick_frame));
            TEST_ASSERT(sp1_hal_host_inject_rx_midi(&clk_m));

            if (next_ext_tick == 5U || next_ext_tick == 7U) {
                const sp1_midi_msg_t stop_m =
                    sp1_midi_msg_realtime(SP1_MIDI_STATUS_STOP, sp1_clock_frame_to_us(target_tick_frame));
                TEST_ASSERT(sp1_hal_host_inject_rx_midi(&stop_m));
            }
            next_ext_tick++;
        }

        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            const uint8_t t = trigs[i].track;
            TEST_ASSERT(t < 2U);
            TEST_ASSERT_EQ_INT(expected_step[t], trigs[i].step_index);
            expected_step[t] = sp1_euclid_next_step(expected_step[t], 4U);
            if (hit_counts[t] < 16U) {
                hit_frames[t][hit_counts[t]++] = trigs[i].sample_frame;
            }
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    /* Both Track 0 (0 ms) and Track 1 (-35 ms) fired the exact same number of steps:
     *   Steps 0, 1, 2 (external), Step 3 (lookahead during timeout), Step 4 & 5 (internal takeover) */
    TEST_ASSERT_EQ_U32(6U, hit_counts[0]);
    TEST_ASSERT_EQ_U32(6U, hit_counts[1]);

    /* Step 2 (index 2) on Track 1 fired genuinely early (< Track 0's Step 2 at Tick 12)
     * after the mid-step 0xFC -> 0xFB Continue at Tick 8! */
    TEST_ASSERT(hit_frames[1][2] + 1500ULL <= hit_frames[0][2]);

    /* Step 4 (index 4) fired immediately upon the 500 ms timeout fallback (~frame 35904)
     * without a 1-step silent hole! */
    TEST_ASSERT(hit_frames[0][4] >= 35900ULL && hit_frames[0][4] <= 36000ULL);
}

static void test_strict_monotonic_midi_tx_ordering_and_live_channel_step_guards(void) {
    const char *csv_path = "test_seq_monotonic_midi.csv";
    init_host_hal(csv_path);

    sp1_sequencer_t seq;
    sp1_seq_init(&seq);
    sp1_seq_set_bpm_x100(&seq, 12000U);
    sp1_seq_set_swing(&seq, 0.58f);

    sp1_seq_set_track_pattern(&seq, 0U, 5U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);
    sp1_seq_set_track_pattern(&seq, 1U, 3U, 16U, 4U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, 18.0f);
    sp1_seq_set_track_pattern(&seq, 2U, 11U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, -12.0f);
    sp1_seq_set_track_pattern(&seq, 3U, 5U, 12U, 1U);
    sp1_seq_set_track_nudge_ms(&seq, 3U, 8.0f);

    sp1_seq_start(&seq);

    sp1_seq_trigger_event_t trigs[SP1_SEQ_MAX_BLOCK_TRIGGERS];
    /* Verify zero-frame call is a safe no-op */
    TEST_ASSERT_EQ_U32(0U, sp1_seq_process_block(&seq, 0ULL, 0U, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS));

    for (uint64_t frame = 0ULL; frame < 96000ULL; frame += SP1_AUDIO_BLOCK_FRAMES) {
        /* Mid-gate MIDI channel switch & live polymeter shrinkage */
        if (frame == 960ULL) {
            sp1_seq_set_midi_channel(&seq, 9U);
        } else if (frame == 48000ULL) {
            sp1_seq_set_track_steps(&seq, 2U, 4U);
        }
        const uint32_t n = sp1_seq_process_block(
            &seq, frame, SP1_AUDIO_BLOCK_FRAMES, trigs, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        for (uint32_t i = 0U; i < n; ++i) {
            TEST_ASSERT(trigs[i].block_offset < SP1_AUDIO_BLOCK_FRAMES);
            TEST_ASSERT(trigs[i].step_index < sp1_seq_get_track_pattern(&seq, trigs[i].track)->steps);
        }
        sp1_hal_service_background();
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    sp1_hal_host_stats_t stats;
    sp1_hal_host_get_stats(&stats);
    TEST_ASSERT_EQ_U32(stats.midi_tx_note_on_count, stats.midi_tx_note_off_count);
    TEST_ASSERT_EQ_U32(0U, stats.midi_tx_dropped);

    /* Confirm 100% monotonically non-decreasing timestamps and sample_frames in CSV */
    FILE *fp = fopen(csv_path, "rb");
    TEST_ASSERT(fp != NULL);
    char line[128];
    uint32_t prev_ts = 0U;
    uint64_t prev_sf = 0ULL;
    uint32_t rows = 0U;
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        unsigned ts = 0U, st = 0U, d1 = 0U, d2 = 0U;
        unsigned long long sf = 0ULL;
        char type[32];
        if (sscanf(line, "%u,%llu,0x%X,%u,%u,%31s", &ts, &sf, &st, &d1, &d2, type) == 6) {
            TEST_ASSERT((uint32_t)ts >= prev_ts);
            TEST_ASSERT((uint64_t)sf >= prev_sf);
            prev_ts = (uint32_t)ts;
            prev_sf = (uint64_t)sf;
            rows++;
        }
    }
    fclose(fp);
    (void)remove(csv_path);
    TEST_ASSERT(rows > 100U);
}

int main(void) {
    RUN_TEST(test_straight_grid_and_gm_drum_midi_notes);
    RUN_TEST(test_dilla_negative_and_positive_nudge_stacked_with_swing);
    RUN_TEST(test_negative_nudge_step0_clamp_and_bar_wrap_early_fire);
    RUN_TEST(test_tempo_changes_zero_drops_zero_doubles_and_note_cleanup);
    RUN_TEST(test_polymeter_and_phase_preserving_mute);
    RUN_TEST(test_mid_step_tempo_jump_180_to_60_and_60_to_180_reanchoring);
    RUN_TEST(test_external_stop_continue_with_negative_nudge_and_500ms_timeout_continuity);
    RUN_TEST(test_strict_monotonic_midi_tx_ordering_and_live_channel_step_guards);
    return sp1_test_summary();
}
