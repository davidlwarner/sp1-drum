#ifndef SP1_HAL_H
#define SP1_HAL_H

/**
 * @file sp1_hal.h
 * @brief Umbrella Hardware Abstraction Layer header for SP-1 Euclidean Drum Machine.
 *
 * Compiles cleanly as both strict C11 (-std=c11 -Wall -Wextra -Werror) and C++17
 * (-std=c++17 -fno-exceptions -fno-rtti) with zero Zephyr header dependencies.
 */

#include <stdint.h>

#include "hal/hal_audio.h"
#include "hal/hal_controls.h"
#include "hal/hal_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize all HAL subsystems (controls/LEDs, MIDI SPSC queues, audio state).
 * Returns 0 on success or a negative errno code on failure.
 */
int sp1_hal_init(void);

/**
 * Perform safe platform teardown before deep sleep / process exit:
 * stops audio, mutes/resets codecs and oscillator (Zephyr) or finalizes WAV/CSV (Host),
 * and blanks all 8 PWM LEDs.
 */
void sp1_hal_shutdown(void);

/**
 * Monotonic microsecond timestamp (hardware cycle/uptime clock on Zephyr,
 * deterministic sample-derived clock on host simulator).
 */
uint32_t sp1_hal_time_us(void);

/**
 * Monotonic millisecond timestamp.
 */
uint32_t sp1_hal_time_ms(void);

/**
 * Background housekeeping hook (drains MIDI TX queue, non-RT services).
 * Call from the low-priority control/HSM thread, never from hard-RT audio callbacks.
 */
void sp1_hal_service_background(void);

#ifdef __cplusplus
}
#endif

#endif /* SP1_HAL_H */
