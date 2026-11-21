#include "core/clock_sync.h"
#include "core/euclid.h"
#include "core/sequencer.h"
#include "core/ui_mapper.h"
#include "dsp/drum_voices.h"
#include "dsp/master_fx.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --wav <path>              Output 48 kHz 16-bit stereo WAV (default: build-host/sim_out.wav)\n"
            "  --midi-csv <path>         Output timestamped MIDI CSV log (default: disabled)\n"
            "  --duration-ms <ms>        Render duration in milliseconds (default: 4000)\n"
            "  --bpm <bpm>               Master tempo in BPM [40..240] (default: 120)\n"
            "  --swing <pct>             Global swing percent [50..66] (default: 58)\n"
            "  --dilla-snare-ms <ms>     Snare Dilla nudge in ms [-35..35] (default: 18)\n"
            "  --dilla-hat-ms <ms>       Hi-Hat Dilla nudge in ms [-35..35] (default: -12)\n"
            "  --snare-morph <0.0..1.0>  Track 2 morph: 0.0=Rimshot .. 1.0=Snare (default: 0.65)\n"
            "  --hat-decay <0.0..1.0>    Track 3 decay: 0.0=Closed .. 1.0=Open (default: 0.25)\n"
            "  --dj-filter <0.0..1.0>    Master DJ SVF: <0.45=LP, 0.50=Bypass, >0.55=HP (default: 0.50)\n"
            "  --stutter-div <0..5>      Beat-Repeat: 0=Off, 1=1/4, 2=1/8, 3=1/16, 4=1/32, 5=1/64\n"
            "  --crush <0.0..1.0>        Bitcrusher / ZOH decimator amount (default: 0.0)\n"
            "  --dub-delay <0.0..1.0>    Dub Ping-Pong Delay send level (default: 0.22)\n"
            "  --fx-showcase             Automate Rimshot->Snare morph, DJ SVF, Stutter & Crush\n"
            "  --quiet                   Suppress MIDI CSV mirroring to stdout\n"
            "  --help                    Show this help text\n",
            prog);
}

static void apply_showcase_automation(sp1_drum_synth_t *synth,
                                      sp1_master_fx_t *fx,
                                      uint64_t frame,
                                      uint64_t total_frames) {
    if (total_frames == 0ULL) {
        return;
    }
    const float progress = (float)frame / (float)total_frames;

    /* Quarter 1 (0.00..0.25): Tight woody 808/909 Rimshot morphing toward snappy Snare */
    if (progress < 0.25f) {
        const float t = progress * 4.0f;
        sp1_drum_voices_set_snare_morph(synth, 0.10f + (0.85f * t));
        sp1_drum_voices_set_hat_decay(synth, 0.15f + (0.45f * t));
        sp1_master_fx_set_dj_filter(fx, 0.50f, 1.85f);
        sp1_master_fx_set_stutter(fx, SP1_STUTTER_OFF);
        sp1_master_fx_set_bitcrusher(fx, 0.0f);
    } else if (progress < 0.50f) {
        /* Quarter 2 (0.25..0.50): DJ Resonant TPT SVF Low-Pass -> High-Pass sweep */
        const float t = (progress - 0.25f) * 4.0f;
        const float dj_pos = (t < 0.5f) ? (0.50f - (0.72f * t)) : (0.14f + (1.40f * (t - 0.5f)));
        sp1_drum_voices_set_snare_morph(synth, 0.85f);
        sp1_master_fx_set_dj_filter(fx, dj_pos, 2.60f);
        sp1_master_fx_set_stutter(fx, SP1_STUTTER_OFF);
        sp1_master_fx_set_bitcrusher(fx, 0.0f);
    } else if (progress < 0.75f) {
        /* Quarter 3 (0.50..0.75): Tempo-synced Beat-Repeat Stutter (1/16 -> 1/32 ratchet) */
        const float t = (progress - 0.50f) * 4.0f;
        sp1_master_fx_set_dj_filter(fx, 0.50f, 1.85f);
        if (t < 0.45f) {
            sp1_master_fx_set_stutter(fx, SP1_STUTTER_DIV_1_16);
        } else if (t < 0.80f) {
            sp1_master_fx_set_stutter(fx, SP1_STUTTER_DIV_1_32);
        } else {
            sp1_master_fx_set_stutter(fx, SP1_STUTTER_OFF);
        }
    } else {
        /* Quarter 4 (0.75..1.00): Bitcrusher / ZOH Decimator + Dub Ping-Pong Delay */
        const float t = (progress - 0.75f) * 4.0f;
        sp1_master_fx_set_stutter(fx, SP1_STUTTER_OFF);
        sp1_master_fx_set_dj_filter(fx, 0.50f, 1.85f);
        sp1_master_fx_set_bitcrusher(fx, (t < 0.65f) ? (0.38f * t) : 0.0f);
        sp1_master_fx_set_dub_delay(fx, 0.35f, 0.55f);
    }
}

int main(int argc, char **argv) {
    const char *wav_path = "build-host/sim_out.wav";
    const char *midi_csv_path = NULL;
    uint32_t duration_ms = 4000U;
    double bpm = 120.0;
    double swing_pct = 58.0;
    double dilla_snare_ms = 18.0;
    double dilla_hat_ms = -12.0;
    double snare_morph = 0.65;
    double hat_decay = 0.25;
    double dj_filter = 0.50;
    long stutter_div = 0L;
    double crush = 0.0;
    double dub_delay = 0.22;
    bool fx_showcase = false;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--wav") == 0 && (i + 1) < argc) {
            wav_path = argv[++i];
        } else if (strcmp(argv[i], "--midi-csv") == 0 && (i + 1) < argc) {
            midi_csv_path = argv[++i];
        } else if (strcmp(argv[i], "--duration-ms") == 0 && (i + 1) < argc) {
            const long val = strtol(argv[++i], NULL, 10);
            if (val <= 0L) {
                fprintf(stderr, "Error: --duration-ms must be positive\n");
                return 1;
            }
            duration_ms = (uint32_t)val;
        } else if (strcmp(argv[i], "--bpm") == 0 && (i + 1) < argc) {
            bpm = strtod(argv[++i], NULL);
            if (bpm < 40.0 || bpm > 240.0) {
                fprintf(stderr, "Error: --bpm must be between 40 and 240\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--swing") == 0 && (i + 1) < argc) {
            swing_pct = strtod(argv[++i], NULL);
            if (swing_pct < 50.0 || swing_pct > 66.0) {
                fprintf(stderr, "Error: --swing must be between 50 and 66\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dilla-snare-ms") == 0 && (i + 1) < argc) {
            dilla_snare_ms = strtod(argv[++i], NULL);
            if (dilla_snare_ms < -35.0 || dilla_snare_ms > 35.0) {
                fprintf(stderr, "Error: --dilla-snare-ms must be between -35 and 35\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dilla-hat-ms") == 0 && (i + 1) < argc) {
            dilla_hat_ms = strtod(argv[++i], NULL);
            if (dilla_hat_ms < -35.0 || dilla_hat_ms > 35.0) {
                fprintf(stderr, "Error: --dilla-hat-ms must be between -35 and 35\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--snare-morph") == 0 && (i + 1) < argc) {
            snare_morph = strtod(argv[++i], NULL);
            if (snare_morph < 0.0 || snare_morph > 1.0) {
                fprintf(stderr, "Error: --snare-morph must be between 0.0 and 1.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--hat-decay") == 0 && (i + 1) < argc) {
            hat_decay = strtod(argv[++i], NULL);
            if (hat_decay < 0.0 || hat_decay > 1.0) {
                fprintf(stderr, "Error: --hat-decay must be between 0.0 and 1.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dj-filter") == 0 && (i + 1) < argc) {
            dj_filter = strtod(argv[++i], NULL);
            if (dj_filter < 0.0 || dj_filter > 1.0) {
                fprintf(stderr, "Error: --dj-filter must be between 0.0 and 1.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--stutter-div") == 0 && (i + 1) < argc) {
            stutter_div = strtol(argv[++i], NULL, 10);
            if (stutter_div < 0L || stutter_div > 5L) {
                fprintf(stderr, "Error: --stutter-div must be between 0 and 5\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--crush") == 0 && (i + 1) < argc) {
            crush = strtod(argv[++i], NULL);
            if (crush < 0.0 || crush > 1.0) {
                fprintf(stderr, "Error: --crush must be between 0.0 and 1.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--dub-delay") == 0 && (i + 1) < argc) {
            dub_delay = strtod(argv[++i], NULL);
            if (dub_delay < 0.0 || dub_delay > 1.0) {
                fprintf(stderr, "Error: --dub-delay must be between 0.0 and 1.0\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--fx-showcase") == 0) {
            fx_showcase = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    const sp1_hal_host_config_t host_cfg = {
        .wav_path = wav_path,
        .midi_csv_path = midi_csv_path,
        .midi_log_stdout = !quiet,
    };
    sp1_hal_host_configure(&host_cfg);

    if (sp1_hal_init() != 0) {
        fprintf(stderr, "Error: sp1_hal_init() failed (check output path: %s)\n", wav_path);
        return 1;
    }

    /* Static allocation of full core + 4-voice drum synth + stereo master FX bus */
    static sp1_sequencer_t seq;
    static sp1_ui_mapper_t ui;
    static sp1_drum_synth_t synth;
    static sp1_master_fx_t fx;

    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    sp1_drum_voices_init(&synth);
    sp1_master_fx_init(&fx);

    sp1_seq_set_bpm(&seq, (float)bpm);
    sp1_seq_set_swing(&seq, (float)(swing_pct * 0.01));

    sp1_drum_voices_set_snare_morph(&synth, (float)snare_morph);
    sp1_drum_voices_set_hat_decay(&synth, (float)hat_decay);
    sp1_master_fx_set_dj_filter(&fx, (float)dj_filter, 1.85f);
    sp1_master_fx_set_stutter(&fx, (sp1_stutter_div_t)stutter_div);
    sp1_master_fx_set_bitcrusher(&fx, (float)crush);
    sp1_master_fx_set_dub_delay(&fx, (float)dub_delay, 0.48f);

    /* Track 0 (Kick, GM 36): E(5,16, rot=0), 0 ms straight anchor */
    sp1_seq_set_track_pattern(&seq, 0U, 5U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);

    /* Track 1 (Snare / Rimshot, GM 38): E(3,16, rot=4) + Dilla micro-timing */
    sp1_seq_set_track_pattern(&seq, 1U, 3U, 16U, 4U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, (float)dilla_snare_ms);

    /* Track 2 (Hi-Hat, GM 42): E(11,16, rot=0) + rushed Dilla pocket */
    sp1_seq_set_track_pattern(&seq, 2U, 11U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, (float)dilla_hat_ms);

    /* Track 3 (2-Op FM / Wavefold Perc, GM 45): E(5,12, rot=1) polymeter, +8 ms pocket */
    sp1_seq_set_track_pattern(&seq, 3U, 5U, 12U, 1U);
    sp1_seq_set_track_nudge_ms(&seq, 3U, 8.0f);

    sp1_seq_start(&seq);

    const uint64_t total_frames =
        ((uint64_t)duration_ms * (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ) / 1000ULL;

    int16_t block_pcm[SP1_AUDIO_BLOCK_SAMPLES];
    float main_l[SP1_AUDIO_BLOCK_FRAMES];
    float main_r[SP1_AUDIO_BLOCK_FRAMES];
    float send_l[SP1_AUDIO_BLOCK_FRAMES];
    float send_r[SP1_AUDIO_BLOCK_FRAMES];
    sp1_seq_trigger_event_t triggers[SP1_SEQ_MAX_BLOCK_TRIGGERS];
    sp1_controls_snapshot_t snap;
    sp1_led_frame_t leds;

    uint64_t frame = 0ULL;
    while (frame < total_frames) {
        const uint64_t rem = total_frames - frame;
        const uint32_t block_frames =
            (rem > (uint64_t)SP1_AUDIO_BLOCK_FRAMES) ? SP1_AUDIO_BLOCK_FRAMES : (uint32_t)rem;

        sp1_hal_controls_poll(&snap);
        sp1_ui_mapper_update(&ui, &seq, &snap);

        if (fx_showcase) {
            apply_showcase_automation(&synth, &fx, frame, total_frames);
        }

        const uint32_t trig_count = sp1_seq_process_block(
            &seq, frame, block_frames, triggers, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        sp1_ui_mapper_on_triggers(&ui, triggers, trig_count);

        sp1_drum_voices_render_block(&synth,
                                     triggers,
                                     trig_count,
                                     main_l,
                                     main_r,
                                     send_l,
                                     send_r,
                                     block_frames);

        const uint32_t step_16th_frames =
            sp1_clock_get_step_frames(sp1_seq_clock_const(&seq));
        sp1_master_fx_process_block(&fx,
                                    step_16th_frames,
                                    main_l,
                                    main_r,
                                    send_l,
                                    send_r,
                                    block_pcm,
                                    block_frames);

        sp1_ui_mapper_render_leds(&ui, &seq, 2U, &leds);
        sp1_hal_leds_set_frame(&leds);

        if (sp1_hal_audio_submit_block(block_pcm, block_frames) != 0) {
            fprintf(stderr, "Error: sp1_hal_audio_submit_block() failed\n");
            sp1_hal_shutdown();
            return 1;
        }
        sp1_hal_service_background();
        frame += (uint64_t)block_frames;
    }

    sp1_seq_stop(&seq);
    sp1_hal_shutdown();

    sp1_hal_host_stats_t stats;
    sp1_hal_host_get_stats(&stats);

    if (!quiet) {
        fprintf(stderr,
                "sp1_sim: wrote %llu stereo frames to %s | MIDI TX: total=%u start=%u clock=%u note_on=%u note_off=%u stop=%u dropped=%u\n",
                (unsigned long long)stats.total_frames_written,
                wav_path,
                stats.midi_tx_count,
                stats.midi_tx_start_count,
                stats.midi_tx_clock_count,
                stats.midi_tx_note_on_count,
                stats.midi_tx_note_off_count,
                stats.midi_tx_stop_count,
                stats.midi_tx_dropped);
    }

    return 0;
}
