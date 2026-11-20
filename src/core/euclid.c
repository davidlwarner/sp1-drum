#include "core/euclid.h"

#include <stddef.h>

static uint8_t clamp_steps(uint8_t steps) {
    if (steps < SP1_EUCLID_MIN_STEPS) {
        return SP1_EUCLID_MIN_STEPS;
    }
    if (steps > SP1_EUCLID_MAX_STEPS) {
        return SP1_EUCLID_MAX_STEPS;
    }
    return steps;
}

static uint8_t wrap_rotation(uint8_t rotation, uint8_t steps) {
    /* Division-free modulo for uint8_t rotation and steps in [1..32]. */
    uint8_t r = rotation;
    while (r >= steps) {
        r = (uint8_t)(r - steps);
    }
    return r;
}

static uint32_t full_step_mask(uint8_t steps) {
    if (steps >= SP1_EUCLID_MAX_STEPS) {
        return 0xFFFFFFFFU;
    }
    return (1U << steps) - 1U;
}

uint32_t sp1_euclid_compute_mask(uint8_t pulses, uint8_t steps, uint8_t rotation) {
    const uint8_t n = clamp_steps(steps);
    const uint8_t k = (pulses > n) ? n : pulses;

    if (k == 0U) {
        return 0U;
    }
    if (k == n) {
        return full_step_mask(n);
    }

    /* Iterative Bjorklund algorithm (Toussaint 2005).
     * Stack-local scratch: 32 * 4 + 32 * 1 = 160 B, zero heap, zero division.
     * Bit 0 of each subsequence is the earliest step in that group. */
    uint32_t seq[SP1_EUCLID_MAX_STEPS];
    uint8_t len[SP1_EUCLID_MAX_STEPS];

    for (uint8_t i = 0U; i < k; ++i) {
        seq[i] = 1U;
        len[i] = 1U;
    }
    for (uint8_t i = k; i < n; ++i) {
        seq[i] = 0U;
        len[i] = 1U;
    }

    uint8_t head_count = k;
    uint8_t rem_count = (uint8_t)(n - k);

    while (rem_count > 1U) {
        const uint8_t pairs = (head_count < rem_count) ? head_count : rem_count;

        for (uint8_t i = 0U; i < pairs; ++i) {
            /* Total bit length across all sequences equals n <= 32, and every
             * remainder has len >= 1, so len[i] is guaranteed < 32 here. */
            seq[i] |= (seq[head_count + i] << len[i]);
            len[i] = (uint8_t)(len[i] + len[head_count + i]);
        }

        if (rem_count > head_count) {
            const uint8_t leftover = (uint8_t)(rem_count - head_count);
            for (uint8_t i = 0U; i < leftover; ++i) {
                seq[head_count + i] = seq[head_count + pairs + i];
                len[head_count + i] = len[head_count + pairs + i];
            }
            rem_count = leftover;
        } else {
            const uint8_t new_rem = (uint8_t)(head_count - rem_count);
            head_count = rem_count;
            rem_count = new_rem;
        }
    }

    uint32_t mask = 0U;
    uint8_t pos = 0U;
    const uint8_t total_groups = (uint8_t)(head_count + rem_count);
    for (uint8_t i = 0U; i < total_groups; ++i) {
        mask |= (seq[i] << pos);
        pos = (uint8_t)(pos + len[i]);
    }

    /* Apply forward circular phase rotation within the n-bit window (zero UB). */
    const uint8_t r = wrap_rotation(rotation, n);
    if (r != 0U) {
        const uint32_t valid = full_step_mask(n);
        mask = ((mask << r) | (mask >> (uint8_t)(n - r))) & valid;
    }

    return mask;
}

void sp1_euclid_pattern_set(sp1_euclid_pattern_t *pat,
                            uint8_t pulses,
                            uint8_t steps,
                            uint8_t rotation) {
    if (pat == NULL) {
        return;
    }

    const uint8_t n = clamp_steps(steps);
    const uint8_t k = (pulses > n) ? n : pulses;
    const uint8_t r = wrap_rotation(rotation, n);

    pat->pulses = k;
    pat->steps = n;
    pat->rotation = r;
    pat->reserved = 0U;
    pat->mask = sp1_euclid_compute_mask(k, n, r);
}

void sp1_euclid_pattern_set_pulses(sp1_euclid_pattern_t *pat, uint8_t pulses) {
    if (pat == NULL) {
        return;
    }
    sp1_euclid_pattern_set(pat, pulses, pat->steps, pat->rotation);
}

void sp1_euclid_pattern_set_steps(sp1_euclid_pattern_t *pat, uint8_t steps) {
    if (pat == NULL) {
        return;
    }
    sp1_euclid_pattern_set(pat, pat->pulses, steps, pat->rotation);
}

void sp1_euclid_pattern_set_rotation(sp1_euclid_pattern_t *pat, uint8_t rotation) {
    if (pat == NULL) {
        return;
    }
    sp1_euclid_pattern_set(pat, pat->pulses, pat->steps, rotation);
}

uint8_t sp1_euclid_popcount(uint32_t mask) {
    uint32_t v = mask;
    uint8_t count = 0U;
    while (v != 0U) {
        v &= (v - 1U);
        count++;
    }
    return count;
}
