#include "hal/sp1_hal_host.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── Static Host HAL State ───────────────────────────────────────────────── */

static sp1_hal_host_config_t g_host_cfg = {
    .wav_path = NULL,
    .midi_csv_path = NULL,
    .midi_log_stdout = false,
};

static FILE *g_wav_file = NULL;
static FILE *g_midi_csv_file = NULL;

/* Static RAM: 2 * 524 B = 1048 B for RX + TX MIDI SPSC queues */
static sp1_midi_spsc_queue_t g_midi_rx_q;
static sp1_midi_spsc_queue_t g_midi_tx_q;
static bool g_midi_ready = true;

/* Static RAM: 16 B fader filters + 40 B live snapshot + 84 B bus + 8 B LEDs = 148 B */
static sp1_fader_filter_t g_fader_filters[SP1_NUM_FADERS];
static sp1_controls_snapshot_t g_live_controls;
static sp1_controls_bus_t g_controls_bus;
static sp1_led_frame_t g_led_frame;

static sp1_audio_render_cb_t g_audio_cb = NULL;
static void *g_audio_ctx = NULL;
static bool g_audio_running = false;
static bool g_audio_muted = false;
static uint8_t g_audio_volume = 100U;

static sp1_hal_host_stats_t g_stats;

/* ── Little-Endian WAV Helpers ───────────────────────────────────────────── */

static void put_le16(uint8_t *dst, uint16_t val) {
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
}

static void put_le32(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static int write_wav_header(FILE *fp, uint32_t data_bytes) {
    uint8_t hdr[44];
    const uint16_t num_channels = (uint16_t)SP1_AUDIO_CHANNELS;
    const uint32_t sample_rate = SP1_AUDIO_SAMPLE_RATE_HZ;
    const uint16_t bits_per_sample = 16U;
    const uint16_t block_align = (uint16_t)(num_channels * (bits_per_sample / 8U));
    const uint32_t byte_rate = sample_rate * (uint32_t)block_align;
    const uint32_t riff_size = 36U + data_bytes;

    memcpy(&hdr[0], "RIFF", 4U);
    put_le32(&hdr[4], riff_size);
    memcpy(&hdr[8], "WAVE", 4U);
    memcpy(&hdr[12], "fmt ", 4U);
    put_le32(&hdr[16], 16U);             /* PCM fmt chunk size */
    put_le16(&hdr[20], 1U);              /* AudioFormat = 1 (PCM) */
    put_le16(&hdr[22], num_channels);
    put_le32(&hdr[24], sample_rate);
    put_le32(&hdr[28], byte_rate);
    put_le16(&hdr[32], block_align);
    put_le16(&hdr[34], bits_per_sample);
    memcpy(&hdr[36], "data", 4U);
    put_le32(&hdr[40], data_bytes);

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        return -1;
    }
    return (fwrite(hdr, 1U, sizeof(hdr), fp) == sizeof(hdr)) ? 0 : -1;
}

static const char *midi_status_name(uint8_t status) {
    if (status == SP1_MIDI_STATUS_CLOCK) {
        return "CLOCK";
    }
    if (status == SP1_MIDI_STATUS_START) {
        return "START";
    }
    if (status == SP1_MIDI_STATUS_CONTINUE) {
        return "CONTINUE";
    }
    if (status == SP1_MIDI_STATUS_STOP) {
        return "STOP";
    }
    switch (status & 0xF0U) {
    case SP1_MIDI_STATUS_NOTE_ON:
        return "NOTE_ON";
    case SP1_MIDI_STATUS_NOTE_OFF:
        return "NOTE_OFF";
    case SP1_MIDI_STATUS_CC:
        return "CC";
    default:
        return "OTHER";
    }
}

/* ── Host Configuration & Inspection ─────────────────────────────────────── */

void sp1_hal_host_configure(const sp1_hal_host_config_t *cfg) {
    if (cfg == NULL) {
        g_host_cfg.wav_path = NULL;
        g_host_cfg.midi_csv_path = NULL;
        g_host_cfg.midi_log_stdout = false;
        return;
    }
    g_host_cfg = *cfg;
}

bool sp1_hal_host_inject_rx_midi(const sp1_midi_msg_t *msg) {
    return sp1_midi_queue_push(&g_midi_rx_q, msg);
}

void sp1_hal_host_get_leds(sp1_led_frame_t *out_frame) {
    if (out_frame != NULL) {
        *out_frame = g_led_frame;
    }
}

void sp1_hal_host_get_stats(sp1_hal_host_stats_t *out_stats) {
    if (out_stats != NULL) {
        g_stats.midi_tx_dropped = sp1_midi_queue_dropped(&g_midi_tx_q);
        g_stats.midi_rx_dropped = sp1_midi_queue_dropped(&g_midi_rx_q);
        *out_stats = g_stats;
    }
}

/* ── HAL Controls & LEDs ─────────────────────────────────────────────────── */

int sp1_hal_controls_init(void) {
    for (uint32_t i = 0U; i < SP1_NUM_FADERS; ++i) {
        sp1_fader_filter_init(&g_fader_filters[i]);
    }
    sp1_controls_snapshot_init(&g_live_controls);
    sp1_controls_bus_init(&g_controls_bus);
    memset(&g_led_frame, 0, sizeof(g_led_frame));
    return 0;
}

void sp1_hal_controls_on_button_event(sp1_button_id_t btn, bool pressed) {
    if ((unsigned)btn >= (unsigned)SP1_BTN_COUNT) {
        return;
    }

    const uint16_t bit = SP1_BTN_BIT(btn);
    const bool was_down = (g_live_controls.buttons_down & bit) != 0U;

    if (pressed) {
        g_live_controls.buttons_down |= bit;
        if (!was_down) {
            g_live_controls.buttons_pressed |= bit;
        }
    } else {
        g_live_controls.buttons_down &= (uint16_t)(~bit);
        if (was_down) {
            g_live_controls.buttons_released |= bit;
        }
    }

    g_live_controls.seq++;
    sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
}

void sp1_hal_controls_on_fader_raw(uint8_t fader_idx, int16_t raw_sample) {
    if (fader_idx >= SP1_NUM_FADERS) {
        return;
    }

    float norm = 0.0f;
    uint8_t cc7 = 0U;
    if (sp1_fader_filter_update(&g_fader_filters[fader_idx], raw_sample, &norm, &cc7)) {
        g_live_controls.fader_raw[fader_idx] = (uint16_t)g_fader_filters[fader_idx].raw_clamped;
        g_live_controls.fader_norm[fader_idx] = norm;
        g_live_controls.fader_cc[fader_idx] = cc7;
        g_live_controls.seq++;
        sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
    }
}

void sp1_hal_controls_poll(sp1_controls_snapshot_t *out_snapshot) {
    if (out_snapshot != NULL) {
        sp1_controls_bus_read(&g_controls_bus, out_snapshot);
    }
    /* Clear one-shot press/release edge latches after consumption. */
    if (g_live_controls.buttons_pressed != 0U || g_live_controls.buttons_released != 0U) {
        g_live_controls.buttons_pressed = 0U;
        g_live_controls.buttons_released = 0U;
        sp1_controls_bus_publish(&g_controls_bus, &g_live_controls);
    }
}

void sp1_hal_leds_set_frame(const sp1_led_frame_t *frame) {
    if (frame == NULL) {
        return;
    }
    g_led_frame = *frame;
}

void sp1_hal_leds_all_off(void) {
    memset(&g_led_frame, 0, sizeof(g_led_frame));
}

/* ── HAL MIDI ────────────────────────────────────────────────────────────── */

int sp1_hal_midi_init(void) {
    sp1_midi_queue_init(&g_midi_rx_q);
    sp1_midi_queue_init(&g_midi_tx_q);
    g_midi_ready = true;

    if (g_midi_csv_file != NULL) {
        fclose(g_midi_csv_file);
        g_midi_csv_file = NULL;
    }

    if (g_host_cfg.midi_csv_path != NULL) {
        g_midi_csv_file = fopen(g_host_cfg.midi_csv_path, "wb");
        if (g_midi_csv_file == NULL) {
            return -1;
        }
        fprintf(g_midi_csv_file, "timestamp_us,sample_frame,status,data1,data2,type\n");
    }

    if (g_host_cfg.midi_log_stdout) {
        printf("timestamp_us,sample_frame,status,data1,data2,type\n");
    }

    return 0;
}

bool sp1_hal_midi_is_ready(void) {
    return g_midi_ready;
}

void sp1_hal_midi_set_ready(bool ready) {
    g_midi_ready = ready;
}

bool sp1_hal_midi_send(const sp1_midi_msg_t *msg) {
    return sp1_midi_queue_push(&g_midi_tx_q, msg);
}

bool sp1_hal_midi_receive(sp1_midi_msg_t *out_msg) {
    return sp1_midi_queue_pop(&g_midi_rx_q, out_msg);
}

void sp1_hal_midi_flush_tx(void) {
    sp1_midi_msg_t msg;
    while (sp1_midi_queue_pop(&g_midi_tx_q, &msg)) {
        g_stats.midi_tx_count++;

        if (msg.status == SP1_MIDI_STATUS_CLOCK) {
            g_stats.midi_tx_clock_count++;
        } else if (msg.status == SP1_MIDI_STATUS_START) {
            g_stats.midi_tx_start_count++;
        } else if (msg.status == SP1_MIDI_STATUS_CONTINUE) {
            g_stats.midi_tx_continue_count++;
        } else if (msg.status == SP1_MIDI_STATUS_STOP) {
            g_stats.midi_tx_stop_count++;
        } else {
            const uint8_t hi = (uint8_t)(msg.status & 0xF0U);
            if (hi == SP1_MIDI_STATUS_NOTE_ON) {
                g_stats.midi_tx_note_on_count++;
            } else if (hi == SP1_MIDI_STATUS_NOTE_OFF) {
                g_stats.midi_tx_note_off_count++;
            } else if (hi == SP1_MIDI_STATUS_CC) {
                g_stats.midi_tx_cc_count++;
            }
        }

        const uint64_t sample_frame =
            (((uint64_t)msg.timestamp_us * (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ) + 500000ULL) /
            1000000ULL;
        const char *name = midi_status_name(msg.status);

        if (g_midi_csv_file != NULL) {
            fprintf(g_midi_csv_file,
                    "%u,%llu,0x%02X,%u,%u,%s\n",
                    msg.timestamp_us,
                    (unsigned long long)sample_frame,
                    (unsigned)msg.status,
                    (unsigned)msg.data1,
                    (unsigned)msg.data2,
                    name);
        }
        if (g_host_cfg.midi_log_stdout) {
            printf("%u,%llu,0x%02X,%u,%u,%s\n",
                   msg.timestamp_us,
                   (unsigned long long)sample_frame,
                   (unsigned)msg.status,
                   (unsigned)msg.data1,
                   (unsigned)msg.data2,
                   name);
        }
    }
}

void sp1_hal_midi_on_rx_ump32(uint32_t word0) {
    sp1_midi_msg_t msg;
    if (sp1_midi_decode_ump32(word0, sp1_hal_time_us(), &msg)) {
        (void)sp1_midi_queue_push(&g_midi_rx_q, &msg);
    }
}

/* ── HAL Audio (48 kHz 16-bit Stereo WAV Writer) ─────────────────────────── */

int sp1_hal_audio_init(sp1_audio_render_cb_t render_cb, void *user_ctx) {
    g_audio_cb = render_cb;
    g_audio_ctx = user_ctx;
    g_audio_running = false;
    g_audio_muted = false;
    g_audio_volume = 100U;

    if (g_wav_file != NULL) {
        fclose(g_wav_file);
        g_wav_file = NULL;
    }

    if (g_host_cfg.wav_path != NULL) {
        g_wav_file = fopen(g_host_cfg.wav_path, "wb");
        if (g_wav_file == NULL) {
            return -1;
        }
        if (write_wav_header(g_wav_file, 0U) != 0) {
            fclose(g_wav_file);
            g_wav_file = NULL;
            return -1;
        }
    }

    return 0;
}

int sp1_hal_audio_start(void) {
    g_audio_running = true;
    return 0;
}

int sp1_hal_audio_stop(void) {
    g_audio_running = false;
    return 0;
}

int sp1_hal_audio_submit_block(const int16_t *interleaved_stereo, uint32_t num_frames) {
    if (interleaved_stereo == NULL || num_frames == 0U) {
        return -1;
    }

    if (g_wav_file != NULL) {
        /* Static RAM: 384 B chunk buffer for endian-safe little-endian PCM serialization */
        uint8_t pcm_bytes[SP1_AUDIO_BLOCK_SAMPLES * 2U];
        uint32_t remaining = num_frames;
        const int16_t *src = interleaved_stereo;

        while (remaining > 0U) {
            const uint32_t chunk_frames =
                (remaining > SP1_AUDIO_BLOCK_FRAMES) ? SP1_AUDIO_BLOCK_FRAMES : remaining;
            const uint32_t chunk_samples = chunk_frames * SP1_AUDIO_CHANNELS;

            for (uint32_t i = 0U; i < chunk_samples; ++i) {
                const int16_t sample = g_audio_muted ? 0 : src[i];
                put_le16(&pcm_bytes[i * 2U], (uint16_t)sample);
            }

            const size_t nbytes = (size_t)chunk_samples * 2U;
            if (fwrite(pcm_bytes, 1U, nbytes, g_wav_file) != nbytes) {
                return -1;
            }

            src += chunk_samples;
            remaining -= chunk_frames;
        }
    }

    g_stats.total_frames_written += (uint64_t)num_frames;
    return 0;
}

void sp1_hal_audio_set_volume(uint8_t volume_0_to_127) {
    g_audio_volume = (volume_0_to_127 > 127U) ? 127U : volume_0_to_127;
}

void sp1_hal_audio_set_mute(bool muted) {
    g_audio_muted = muted;
}

void sp1_hal_audio_shutdown(void) {
    g_audio_running = false;

    if (g_wav_file != NULL) {
        const uint32_t data_bytes =
            (uint32_t)(g_stats.total_frames_written * (uint64_t)SP1_AUDIO_CHANNELS * 2ULL);
        (void)write_wav_header(g_wav_file, data_bytes);
        fclose(g_wav_file);
        g_wav_file = NULL;
    }
}

/* ── Top-Level HAL Lifecycle & Deterministic Sample Clock ────────────────── */

int sp1_hal_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    (void)g_audio_cb;
    (void)g_audio_ctx;
    (void)g_audio_running;
    (void)g_audio_volume;

    if (sp1_hal_controls_init() != 0) {
        return -1;
    }
    if (sp1_hal_midi_init() != 0) {
        return -1;
    }
    if (sp1_hal_audio_init(NULL, NULL) != 0) {
        return -1;
    }
    return 0;
}

void sp1_hal_shutdown(void) {
    sp1_hal_midi_flush_tx();
    sp1_hal_audio_shutdown();
    sp1_hal_leds_all_off();

    if (g_midi_csv_file != NULL) {
        fclose(g_midi_csv_file);
        g_midi_csv_file = NULL;
    }
}

uint32_t sp1_hal_time_us(void) {
    return (uint32_t)((g_stats.total_frames_written * 1000000ULL) /
                      (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ);
}

uint32_t sp1_hal_time_ms(void) {
    return (uint32_t)((g_stats.total_frames_written * 1000ULL) /
                      (uint64_t)SP1_AUDIO_SAMPLE_RATE_HZ);
}

void sp1_hal_service_background(void) {
    sp1_hal_midi_flush_tx();
}
