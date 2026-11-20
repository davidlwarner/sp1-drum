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
#define SP1_SIM_TEST_TONE_HZ    440.0f
#define SP1_SIM_TEST_TONE_AMP   16384.0f       /* -6 dBFS peak, zero clipping */

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

static uint32_t frame_to_us(uint64_t frame) {
    return (uint32_t)((frame * 1000000ULL) / (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ);
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --wav <path>          Output 48 kHz 16-bit stereo WAV (default: build-host/sim_out.wav)\n"
            "  --midi-csv <path>     Output timestamped MIDI CSV log (default: disabled)\n"
            "  --duration-ms <ms>    Render duration in milliseconds (default: 2000)\n"
            "  --bpm <bpm>           Tempo in BPM for 24 PPQN clock (default: 120)\n"
            "  --quiet               Suppress MIDI CSV mirroring to stdout\n"
            "  --help                Show this help text\n",
            prog);
}

int main(int argc, char **argv) {
    const char *wav_path = "build-host/sim_out.wav";
    const char *midi_csv_path = NULL;
    uint32_t duration_ms = 2000U;
    double bpm = 120.0;
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
            if (bpm < 20.0 || bpm > 300.0) {
                fprintf(stderr, "Error: --bpm must be between 20 and 300\n");
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

    const uint64_t total_frames =
        ((uint64_t)duration_ms * (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ) / 1000ULL;
    const uint32_t phase_inc = (uint32_t)(SP1_SIM_TEST_TONE_HZ * SP1_SIM_PHASE_SCALE);
    uint32_t phase = 0U;

    /* Exact integer 24 PPQN accumulator (zero floating-point drift):
     * fires whenever clock_acc >= 60 * 100 * 48000 = 288,000,000. */
    const uint64_t bpm_x100 = (uint64_t)(bpm * 100.0 + 0.5);
    const uint64_t clock_num = bpm_x100 * (uint64_t)SP1_MIDI_PPQN;
    const uint64_t clock_den = 60ULL * 100ULL * (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ;
    uint64_t clock_acc = clock_den; /* Fire tick 0 exactly at frame 0 */
    uint32_t tick_idx = 0U;

    /* Emit MIDI Start (0xFA) at frame 0 */
    const sp1_midi_msg_t start_msg = sp1_midi_msg_realtime(SP1_MIDI_STATUS_START, 0U);
    (void)sp1_hal_midi_send(&start_msg);

    /* Static block scratch buffer: 96 stereo frames = 384 B */
    int16_t block_pcm[SP1_AUDIO_BLOCK_SAMPLES];
    sp1_led_frame_t leds;
    memset(&leds, 0, sizeof(leds));

    uint64_t frame = 0ULL;
    while (frame < total_frames) {
        const uint64_t rem = total_frames - frame;
        const uint32_t block_frames =
            (rem > (uint64_t)SP1_AUDIO_BLOCK_FRAMES) ? SP1_AUDIO_BLOCK_FRAMES : (uint32_t)rem;

        for (uint32_t i = 0U; i < block_frames; ++i) {
            const uint64_t cur_frame = frame + (uint64_t)i;

            if (clock_acc >= clock_den) {
                clock_acc -= clock_den;
                const uint32_t ts_us = frame_to_us(cur_frame);

                const sp1_midi_msg_t clk = sp1_midi_msg_realtime(SP1_MIDI_STATUS_CLOCK, ts_us);
                (void)sp1_hal_midi_send(&clk);

                const uint32_t sub_tick = tick_idx % SP1_MIDI_PPQN;
                if (sub_tick == 0U) {
                    const uint32_t beat = (tick_idx / SP1_MIDI_PPQN) & 3U;
                    const sp1_midi_msg_t note_on =
                        sp1_midi_msg_note_on(0U, 60U, 110U, ts_us);
                    (void)sp1_hal_midi_send(&note_on);

                    memset(&leds, 0, sizeof(leds));
                    leds.play[beat] = 255U;
                    leds.track[0] = 220U;
                    sp1_hal_leds_set_frame(&leds);
                } else if (sub_tick == (SP1_MIDI_PPQN / 2U)) {
                    const sp1_midi_msg_t note_off =
                        sp1_midi_msg_note_off(0U, 60U, 0U, ts_us);
                    (void)sp1_hal_midi_send(&note_off);

                    leds.track[0] = 0U;
                    sp1_hal_leds_set_frame(&leds);
                }
                tick_idx++;
            }
            clock_acc += clock_num;

            const float s = sine_lut_sample(phase) * SP1_SIM_TEST_TONE_AMP;
            const int16_t pcm = (int16_t)s;
            block_pcm[i * 2U] = pcm;
            block_pcm[(i * 2U) + 1U] = pcm;
            phase += phase_inc;
        }

        if (sp1_hal_audio_submit_block(block_pcm, block_frames) != 0) {
            fprintf(stderr, "Error: sp1_hal_audio_submit_block() failed\n");
            sp1_hal_shutdown();
            return 1;
        }
        sp1_hal_service_background();
        frame += (uint64_t)block_frames;
    }

    /* Emit MIDI Stop (0xFC) at end of render */
    const sp1_midi_msg_t stop_msg =
        sp1_midi_msg_realtime(SP1_MIDI_STATUS_STOP, frame_to_us(total_frames));
    (void)sp1_hal_midi_send(&stop_msg);

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
