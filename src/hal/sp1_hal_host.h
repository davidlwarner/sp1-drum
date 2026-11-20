#ifndef SP1_HAL_HOST_H
#define SP1_HAL_HOST_H

/**
 * @file sp1_hal_host.h
 * @brief Host-only configuration and inspection hooks for the offline WAV/MIDI simulator
 *        and CTest unit suites. Never included by Zephyr ARM firmware sources.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal/sp1_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *wav_path;       /* Output 48 kHz 16-bit stereo WAV file (NULL to disable) */
    const char *midi_csv_path;  /* Output CSV MIDI log file (NULL to disable) */
    bool midi_log_stdout;       /* Mirror timestamped MIDI events to stdout */
} sp1_hal_host_config_t;

typedef struct {
    uint64_t total_frames_written;
    uint32_t midi_tx_count;
    uint32_t midi_tx_clock_count;
    uint32_t midi_tx_start_count;
    uint32_t midi_tx_continue_count;
    uint32_t midi_tx_stop_count;
    uint32_t midi_tx_note_on_count;
    uint32_t midi_tx_note_off_count;
    uint32_t midi_tx_cc_count;
    uint32_t midi_tx_dropped;
    uint32_t midi_rx_dropped;
} sp1_hal_host_stats_t;

/**
 * Configure host simulator outputs before calling sp1_hal_init().
 */
void sp1_hal_host_configure(const sp1_hal_host_config_t *cfg);

/**
 * Inject a synthetic incoming MIDI message into the host RX SPSC queue.
 */
bool sp1_hal_host_inject_rx_midi(const sp1_midi_msg_t *msg);

/**
 * Read back the most recent 8-channel PWM LED frame written via sp1_hal_leds_set_frame().
 */
void sp1_hal_host_get_leds(sp1_led_frame_t *out_frame);

/**
 * Read back cumulative host HAL audio/MIDI counters.
 */
void sp1_hal_host_get_stats(sp1_hal_host_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* SP1_HAL_HOST_H */
