#ifndef SP1_HAL_AUDIO_H
#define SP1_HAL_AUDIO_H

/**
 * @file hal_audio.h
 * @brief Pure C11 Hardware Abstraction Layer for 48 kHz stereo audio block output.
 *
 * Audio contract:
 *  - 48,000 Hz stereo, 96-frame (2.0 ms) DMA blocks.
 *  - DSP core produces interleaved stereo int16_t samples (L0, R0, L1, R1, ...).
 *  - Zephyr target packs int16_t frames into 24-bit-in-32-bit I2S DMA words for
 *    CS42L42 (word_size = 24U) and TAS2505 (24-bit I2S in 32-bit slots).
 *  - Host backend writes 48 kHz 16-bit stereo RIFF/WAVE PCM to disk.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_AUDIO_SAMPLE_RATE_HZ    48000U
#define SP1_AUDIO_CHANNELS          2U
#define SP1_AUDIO_BLOCK_FRAMES      96U    /* 2.0 ms @ 48 kHz */
#define SP1_AUDIO_BLOCK_SAMPLES     (SP1_AUDIO_BLOCK_FRAMES * SP1_AUDIO_CHANNELS)

/* 24-bit signed sample packed in 32-bit I2S EasyDMA word (bits [23:8] <- int16_t,
 * sign-extended in [31:24]; uses multiplication by 256 to avoid C11 signed left-shift UB). */
#define SP1_AUDIO_I2S24_SHIFT           8U
#define SP1_AUDIO_I2S24_SCALE           256

/* nRF52840 Cortex-M4F @ 64 MHz real-time cycle budget per 96-frame (2.0 ms) block:
 *  - Hard 100% ceiling: 64,000,000 * 96 / 48,000 = 128,000 cycles/block (1,333.3 cyc/frame)
 *  - Target <= 55% DSP load:                        70,400 cycles/block (  733.3 cyc/frame)
 *  - Peak   <= 65% stress ceiling:                  83,200 cycles/block (  866.7 cyc/frame) */
#define SP1_AUDIO_CPU_HZ                64000000U
#define SP1_AUDIO_BLOCK_BUDGET_CYCLES   128000U
#define SP1_AUDIO_TARGET_55PCT_CYCLES   70400U
#define SP1_AUDIO_PEAK_65PCT_CYCLES     83200U

/**
 * Real-time audio block render callback invoked by the audio HAL thread/driver.
 * MUST be hard-RT safe: zero heap allocation, zero blocking, zero logging.
 */
typedef void (*sp1_audio_render_cb_t)(int16_t *interleaved_stereo,
                                      uint32_t num_frames,
                                      void *user_ctx);

/**
 * Audio block cycle-counter telemetry snapshot (DWT CYCCNT on Cortex-M4F).
 * Static RAM: 28 bytes (32 bytes aligned).
 */
typedef struct {
    uint64_t total_cycles;      /* Cumulative CPU cycles across all rendered 96-frame blocks */
    uint32_t block_count;       /* Total 96-frame audio blocks rendered */
    uint32_t last_cycles;       /* CPU cycles elapsed in most recent 96-frame block */
    uint32_t max_cycles;        /* Worst-case peak CPU cycles observed across any block */
    uint32_t over_budget_count; /* Blocks exceeding SP1_AUDIO_TARGET_55PCT_CYCLES (70,400) */
    uint32_t underrun_count;    /* Blocks exceeding SP1_AUDIO_BLOCK_BUDGET_CYCLES (128,000) or DMA slab timeout */
} sp1_audio_cycle_stats_t;

/* ── Portable C11 Packing & Cycle Profiling Helpers (sp1_hal_common.c) ───── */

/**
 * Pack interleaved stereo int16_t frames (2 * num_frames samples) into 24-bit-in-32-bit
 * signed I2S DMA words (2 * num_frames int32_t words).
 */
void sp1_audio_pack_s16_to_i2s24_in_32(const int16_t *src_s16,
                                       int32_t *dst_i2s32,
                                       uint32_t num_frames);

/**
 * Reset a cycle telemetry struct to all zeros.
 */
void sp1_audio_cycle_stats_reset(sp1_audio_cycle_stats_t *stats);

/**
 * Record one 96-frame audio block's elapsed CPU cycles (RT-safe, zero division).
 */
void sp1_audio_cycle_stats_record(sp1_audio_cycle_stats_t *stats, uint32_t elapsed_cycles);

/**
 * Compute mean CPU cycles per rendered 96-frame block (call from non-RT background/test context).
 */
uint32_t sp1_audio_cycle_stats_avg(const sp1_audio_cycle_stats_t *stats);

/* ── Platform HAL Audio Lifecycle (sp1_hal_zephyr.c / sp1_hal_host.c) ────── */

int sp1_hal_audio_init(sp1_audio_render_cb_t render_cb, void *user_ctx);
int sp1_hal_audio_start(void);
int sp1_hal_audio_stop(void);

/**
 * Submit an interleaved stereo int16_t audio block synchronously (used by host simulator
 * and deterministic tests; on Zephyr, audio streaming is typically callback-driven).
 */
int sp1_hal_audio_submit_block(const int16_t *interleaved_stereo, uint32_t num_frames);

void sp1_hal_audio_set_volume(uint8_t volume_0_to_127);
void sp1_hal_audio_set_mute(bool muted);

/**
 * Poll 3.5 mm headphone jack insertion state (non-RT main thread) and route
 * output exclusively to CS42L42 headphones or TAS2505 speaker at current volume.
 */
void sp1_hal_audio_service_routing(void);

/**
 * Tear-free spinlock-guarded cycle telemetry accessors.
 */
void sp1_hal_audio_record_cycles(uint32_t elapsed_cycles);
void sp1_hal_audio_get_cycle_stats(sp1_audio_cycle_stats_t *out_stats);
void sp1_hal_audio_reset_cycle_stats(void);
size_t sp1_hal_audio_get_thread_free_stack(void);

/**
 * Safe teardown prior to sys_poweroff() (Zephyr) or WAV header finalization (Host):
 * stops I2S0, mutes/resets TAS2505 and CS42L42, and disables the 3.072 MHz oscillator.
 */
void sp1_hal_audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SP1_HAL_AUDIO_H */
