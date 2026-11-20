#ifndef SP1_CORE_EUCLID_H
#define SP1_CORE_EUCLID_H

/**
 * @file euclid.h
 * @brief Pure C11 Bjorklund Euclidean rhythm generator (1..32 steps, static uint32_t masks).
 *
 * Hard real-time contract:
 *  - Zero Zephyr headers, zero dynamic allocation, zero runtime division.
 *  - Bitmasks are precomputed on parameter change via sp1_euclid_compute_mask().
 *  - Audio/clock hot path uses O(1) bit tests and comparison-wrapped step indices.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_EUCLID_MIN_STEPS    1U
#define SP1_EUCLID_MAX_STEPS    32U

/* Precomputed Euclidean track pattern.
 * Static RAM: 8 bytes per track (32 bytes for 4 tracks). */
typedef struct {
    uint8_t pulses;     /* k: active pulse count [0..steps] */
    uint8_t steps;      /* n: polymeter step length [1..32] */
    uint8_t rotation;   /* r: forward phase offset [0..steps-1] */
    uint8_t reserved;   /* Explicit padding for 4-byte alignment */
    uint32_t mask;      /* Precomputed step bitmask: bit s == 1 iff step s fires */
} sp1_euclid_pattern_t;

/**
 * Compute a Bjorklund Euclidean rhythm bitmask for pulse count `pulses` (k),
 * step length `steps` (n in 1..32), and forward circular phase offset `rotation` (0..n-1).
 *
 * Inputs outside bounds are clamped safely (steps -> [1, 32], pulses -> [0, steps],
 * rotation wrapped modulo steps without hardware division).
 * Bit 0 represents step 0 (the pattern downbeat).
 */
uint32_t sp1_euclid_compute_mask(uint8_t pulses, uint8_t steps, uint8_t rotation);

/**
 * Initialize or update a pattern descriptor and recompute its precomputed bitmask.
 * Intended for control/parameter-change rate (never needed inside per-sample loops).
 */
void sp1_euclid_pattern_set(sp1_euclid_pattern_t *pat,
                            uint8_t pulses,
                            uint8_t steps,
                            uint8_t rotation);

/**
 * Convenience mutators preserving the other pattern parameters and recomputing mask.
 */
void sp1_euclid_pattern_set_pulses(sp1_euclid_pattern_t *pat, uint8_t pulses);
void sp1_euclid_pattern_set_steps(sp1_euclid_pattern_t *pat, uint8_t steps);
void sp1_euclid_pattern_set_rotation(sp1_euclid_pattern_t *pat, uint8_t rotation);

/**
 * Count the number of set pulse bits in a 32-bit pattern mask.
 */
uint8_t sp1_euclid_popcount(uint32_t mask);

/**
 * O(1) RT-safe step trigger query (zero division).
 */
static inline bool sp1_euclid_step_active(uint32_t mask, uint8_t step_idx) {
    if (step_idx >= SP1_EUCLID_MAX_STEPS) {
        return false;
    }
    return ((mask >> step_idx) & 1U) != 0U;
}

/**
 * O(1) RT-safe polymeter step advance without modulo/division.
 */
static inline uint8_t sp1_euclid_next_step(uint8_t step_idx, uint8_t steps) {
    if (steps <= 1U) {
        return 0U;
    }
    const uint8_t next = (uint8_t)(step_idx + 1U);
    return (next < steps) ? next : 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* SP1_CORE_EUCLID_H */
