#include "core/clock_sync.h"
#include "core/euclid.h"
#include "core/sequencer.h"
#include "core/ui_mapper.h"
#include "hal/sp1_hal.h"
#include "hal/sp1_hal_host.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Flash-Style 256-Entry Sine LUT + Guard Sample (1028 B) ──────────────── */

#define SP1_SIM_TWO_PI          6.28318530718f
#define SP1_SIM_PHASE_SCALE     89478.485333f  /* 2^32 / 48000.0f */
#define SP1_SIM_OUTPUT_GAIN     28000.0f       /* Headroom below int16_t full scale */

static float g_sine_lut[257];

static void init_sine_lut(void) {
    for (uint32_t i = 0U; i < 256U; ++i) {
        g_sine_lut[i] = sinf(SP1_SIM_TWO_PI * ((float)i * (1.0f / 256.0f)));
    }
    g_sine_lut[256] = g_sine_lut[0];
}

static inline float sine_lut_sample(uint32_t phase) {
    const uint32_t idx = phase >> 24U;
    const uint32_t frac = (phase >> 8U) & 0xFFFFU;
    const float t = (float)frac * (1.0f / 65536.0f);
    const float a = g_sine_lut[idx];
    const float b = g_sine_lut[idx + 1U];
    return a + (b - a) * t;
}

static inline uint32_t hz_to_phase_inc(float hz) {
    return (uint32_t)(hz * SP1_SIM_PHASE_SCALE);
}

static inline float fast_soft_clip(float x) {
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* ── Lightweight 4-Voice Placeholder Drum Pulse Synth (Milestone 2 Sim) ──── */

typedef struct {
    uint32_t phase;
    uint32_t phase2;
    uint32_t rng;
    float env;
    float pitch_env;
    float prev_noise;
    float gain;
} sp1_sim_voice_t;

static inline float next_noise(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static void sim_voices_init(sp1_sim_voice_t voices[SP1_SEQ_NUM_TRACKS]) {
    memset(voices, 0, sizeof(sp1_sim_voice_t) * SP1_SEQ_NUM_TRACKS);
    voices[0].rng = 0x13579BDFU;
    voices[1].rng = 0x2468ACE1U;
    voices[2].rng = 0xA5A55A5AU;
    voices[3].rng = 0xCAFEBABEU;
}

static void sim_voice_trigger(sp1_sim_voice_t voices[SP1_SEQ_NUM_TRACKS],
                              const sp1_seq_trigger_event_t *trig) {
    if (trig->track >= SP1_SEQ_NUM_TRACKS) {
        return;
    }
    sp1_sim_voice_t *v = &voices[trig->track];
    v->phase = 0U;
    v->phase2 = 0U;
    v->env = 1.0f;
    v->pitch_env = 1.0f;
    v->gain = (float)trig->velocity * (1.0f / 127.0f);
}

static void sim_voices_render_sample(sp1_sim_voice_t voices[SP1_SEQ_NUM_TRACKS],
                                     float *out_l,
                                     float *out_r) {
    float mix_l = 0.0f;
    float mix_r = 0.0f;

    /* Track 0: Kick (pitch-swept sine thump, ~110 ms decay) */
    if (voices[0].env > 1e-4f) {
        const float freq = 48.0f + (105.0f * voices[0].pitch_env);
        const float s = sine_lut_sample(voices[0].phase) * voices[0].env * voices[0].gain * 0.55f;
        voices[0].phase += hz_to_phase_inc(freq);
        voices[0].env *= 0.99935f;
        voices[0].pitch_env *= 0.9970f;
        mix_l += s;
        mix_r += s;
    } else {
        voices[0].env = 0.0f;
    }

    /* Track 1: Snare (185 Hz body + high-passed noise snap, ~80 ms decay) */
    if (voices[1].env > 1e-4f) {
        const float body = sine_lut_sample(voices[1].phase) * 0.45f;
        const float n = next_noise(&voices[1].rng);
        const float hp = n - voices[1].prev_noise;
        voices[1].prev_noise = n;
        const float s = (body + hp * 0.45f) * voices[1].env * voices[1].gain * 0.42f;
        voices[1].phase += hz_to_phase_inc(185.0f);
        voices[1].env *= 0.99910f;
        mix_l += s;
        mix_r += s;
    } else {
        voices[1].env = 0.0f;
    }

    /* Track 2: Hi-Hat (high-passed noise + metallic pulse, ~28 ms decay, pan right) */
    if (voices[2].env > 1e-4f) {
        const float n = next_noise(&voices[2].rng);
        const float hp = n - voices[2].prev_noise;
        voices[2].prev_noise = n;
        const float metal = sine_lut_sample(voices[2].phase) * sine_lut_sample(voices[2].phase2);
        const float s = (hp * 0.45f + metal * 0.35f) * voices[2].env * voices[2].gain * 0.28f;
        voices[2].phase += hz_to_phase_inc(6130.0f);
        voices[2].phase2 += hz_to_phase_inc(8420.0f);
        voices[2].env *= 0.99740f;
        mix_l += s * 0.78f;
        mix_r += s * 1.00f;
    } else {
        voices[2].env = 0.0f;
    }

    /* Track 3: Perc (tuned woodblock/tom ping, ~50 ms decay, pan left) */
    if (voices[3].env > 1e-4f) {
        const float freq = 460.0f + (180.0f * voices[3].pitch_env);
        const float s = sine_lut_sample(voices[3].phase) * voices[3].env * voices[3].gain * 0.32f;
        voices[3].phase += hz_to_phase_inc(freq);
        voices[3].env *= 0.99850f;
        voices[3].pitch_env *= 0.9960f;
        mix_l += s * 1.00f;
        mix_r += s * 0.78f;
    } else {
        voices[3].env = 0.0f;
    }

    *out_l = fast_soft_clip(mix_l);
    *out_r = fast_soft_clip(mix_r);
}

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
            "  --quiet                   Suppress MIDI CSV mirroring to stdout\n"
            "  --help                    Show this help text\n",
            prog);
}

int main(int argc, char **argv) {
    const char *wav_path = "build-host/sim_out.wav";
    const char *midi_csv_path = NULL;
    uint32_t duration_ms = 4000U;
    double bpm = 120.0;
    double swing_pct = 58.0;
    double dilla_snare_ms = 18.0;
    double dilla_hat_ms = -12.0;
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

    init_sine_lut();

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

    /* Configure 4-track Euclidean polymetric groove with global swing + Dilla pocket. */
    static sp1_sequencer_t seq;
    static sp1_ui_mapper_t ui;
    static sp1_sim_voice_t voices[SP1_SEQ_NUM_TRACKS];

    sp1_seq_init(&seq);
    sp1_ui_mapper_init(&ui);
    sim_voices_init(voices);

    sp1_seq_set_bpm(&seq, (float)bpm);
    sp1_seq_set_swing(&seq, (float)(swing_pct * 0.01));

    /* Track 0 (Kick, GM 36): E(5,16, rot=0), 0 ms straight anchor */
    sp1_seq_set_track_pattern(&seq, 0U, 5U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 0U, 0.0f);

    /* Track 1 (Snare, GM 38): E(3,16, rot=4) hits steps 4 (straight beat 2), 9 (swung), 14 */
    sp1_seq_set_track_pattern(&seq, 1U, 3U, 16U, 4U);
    sp1_seq_set_track_nudge_ms(&seq, 1U, (float)dilla_snare_ms);

    /* Track 2 (Hi-Hat, GM 42): E(11,16, rot=0), rushed Dilla pocket */
    sp1_seq_set_track_pattern(&seq, 2U, 11U, 16U, 0U);
    sp1_seq_set_track_nudge_ms(&seq, 2U, (float)dilla_hat_ms);

    /* Track 3 (Perc, GM 45): E(5,12, rot=1) polymeter, +8 ms pocket */
    sp1_seq_set_track_pattern(&seq, 3U, 5U, 12U, 1U);
    sp1_seq_set_track_nudge_ms(&seq, 3U, 8.0f);

    sp1_seq_start(&seq);

    const uint64_t total_frames =
        ((uint64_t)duration_ms * (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ) / 1000ULL;

    /* Static block scratch buffer: 96 stereo frames = 384 B */
    int16_t block_pcm[SP1_AUDIO_BLOCK_SAMPLES];
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

        const uint32_t trig_count = sp1_seq_process_block(
            &seq, frame, block_frames, triggers, SP1_SEQ_MAX_BLOCK_TRIGGERS);
        sp1_ui_mapper_on_triggers(&ui, triggers, trig_count);

        uint32_t next_trig = 0U;
        for (uint32_t i = 0U; i < block_frames; ++i) {
            while (next_trig < trig_count && triggers[next_trig].block_offset <= i) {
                sim_voice_trigger(voices, &triggers[next_trig]);
                next_trig++;
            }

            float l = 0.0f;
            float r = 0.0f;
            sim_voices_render_sample(voices, &l, &r);

            block_pcm[i * 2U] = (int16_t)(l * SP1_SIM_OUTPUT_GAIN);
            block_pcm[(i * 2U) + 1U] = (int16_t)(r * SP1_SIM_OUTPUT_GAIN);
        }

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
