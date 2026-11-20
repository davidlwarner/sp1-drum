#include "test_harness.h"
#include "core/euclid.h"

#include <stdint.h>

static uint32_t mask_from_bits(const char *pattern) {
    uint32_t mask = 0U;
    for (uint8_t i = 0U; pattern[i] != '\0' && i < 32U; ++i) {
        if (pattern[i] == '1') {
            mask |= (1U << i);
        }
    }
    return mask;
}

static void test_edge_cases_and_clamping(void) {
    /* k = 0 -> empty mask for any n */
    TEST_ASSERT_EQ_U32(0x00000000U, sp1_euclid_compute_mask(0U, 1U, 0U));
    TEST_ASSERT_EQ_U32(0x00000000U, sp1_euclid_compute_mask(0U, 16U, 5U));
    TEST_ASSERT_EQ_U32(0x00000000U, sp1_euclid_compute_mask(0U, 32U, 17U));

    /* k = 1 -> single pulse at step 0 (rotated cleanly to step r) */
    TEST_ASSERT_EQ_U32(0x00000001U, sp1_euclid_compute_mask(1U, 1U, 0U));
    TEST_ASSERT_EQ_U32(0x00000001U, sp1_euclid_compute_mask(1U, 8U, 0U));
    TEST_ASSERT_EQ_U32(0x00000008U, sp1_euclid_compute_mask(1U, 8U, 3U));
    TEST_ASSERT_EQ_U32(0x80000000U, sp1_euclid_compute_mask(1U, 32U, 31U));

    /* k = n -> all n low bits set (including full 32-bit boundary) */
    TEST_ASSERT_EQ_U32(0x00000001U, sp1_euclid_compute_mask(1U, 1U, 0U));
    TEST_ASSERT_EQ_U32(0x000000FFU, sp1_euclid_compute_mask(8U, 8U, 0U));
    TEST_ASSERT_EQ_U32(0x0000FFFFU, sp1_euclid_compute_mask(16U, 16U, 7U));
    TEST_ASSERT_EQ_U32(0xFFFFFFFFU, sp1_euclid_compute_mask(32U, 32U, 13U));

    /* Out-of-range clamping: n=0 -> 1, n>32 -> 32, k>n -> n */
    sp1_euclid_pattern_t pat;
    sp1_euclid_pattern_set(&pat, 5U, 0U, 3U);
    TEST_ASSERT_EQ_INT(1, pat.steps);
    TEST_ASSERT_EQ_INT(1, pat.pulses);
    TEST_ASSERT_EQ_INT(0, pat.rotation);
    TEST_ASSERT_EQ_U32(0x00000001U, pat.mask);

    sp1_euclid_pattern_set(&pat, 99U, 64U, 35U);
    TEST_ASSERT_EQ_INT(32, pat.steps);
    TEST_ASSERT_EQ_INT(32, pat.pulses);
    TEST_ASSERT_EQ_INT(3, pat.rotation);
    TEST_ASSERT_EQ_U32(0xFFFFFFFFU, pat.mask);
}

static void test_canonical_bjorklund_rhythms(void) {
    /* E(3, 8) = Cuban Tresillo: 10010010 -> steps {0, 3, 6} */
    TEST_ASSERT_EQ_U32(mask_from_bits("10010010"), sp1_euclid_compute_mask(3U, 8U, 0U));
    TEST_ASSERT_EQ_U32(0x00000049U, sp1_euclid_compute_mask(3U, 8U, 0U));

    /* E(5, 8) = Cuban Cinquillo: 10110110 -> steps {0, 2, 3, 5, 6} */
    TEST_ASSERT_EQ_U32(mask_from_bits("10110110"), sp1_euclid_compute_mask(5U, 8U, 0U));
    TEST_ASSERT_EQ_U32(0x0000006DU, sp1_euclid_compute_mask(5U, 8U, 0U));

    /* E(4, 8) = Even divisor: 10101010 -> steps {0, 2, 4, 6} */
    TEST_ASSERT_EQ_U32(mask_from_bits("10101010"), sp1_euclid_compute_mask(4U, 8U, 0U));

    /* E(5, 12) = Afro-Cuban bell necklace: 100101001010 */
    TEST_ASSERT_EQ_U32(mask_from_bits("100101001010"), sp1_euclid_compute_mask(5U, 12U, 0U));

    /* E(5, 16) = Bossa-Nova necklace: 1001001001001000 */
    TEST_ASSERT_EQ_U32(mask_from_bits("1001001001001000"), sp1_euclid_compute_mask(5U, 16U, 0U));
    TEST_ASSERT_EQ_U32(0x00001249U, sp1_euclid_compute_mask(5U, 16U, 0U));

    /* E(7, 16) = Brazilian Samba necklace: 1001010100101010 */
    TEST_ASSERT_EQ_U32(mask_from_bits("1001010100101010"), sp1_euclid_compute_mask(7U, 16U, 0U));
    TEST_ASSERT_EQ_U32(0x000054A9U, sp1_euclid_compute_mask(7U, 16U, 0U));

    /* Verify popcount == k across every valid (k, n) pair in 1..32 */
    for (uint8_t n = 1U; n <= 32U; ++n) {
        for (uint8_t k = 0U; k <= n; ++k) {
            const uint32_t m = sp1_euclid_compute_mask(k, n, 0U);
            TEST_ASSERT_EQ_INT(k, sp1_euclid_popcount(m));
            if (n < 32U) {
                TEST_ASSERT_EQ_U32(0U, m & ~((1U << n) - 1U));
            }
        }
    }
}

static void test_phase_rotations_and_polymeter_helpers(void) {
    /* Rotate E(3, 8) = {0, 3, 6} by 0..8 and verify exact circular wrap */
    const uint32_t base = sp1_euclid_compute_mask(3U, 8U, 0U);
    TEST_ASSERT_EQ_U32(mask_from_bits("01001001"), sp1_euclid_compute_mask(3U, 8U, 1U));
    TEST_ASSERT_EQ_U32(mask_from_bits("10100100"), sp1_euclid_compute_mask(3U, 8U, 2U));
    TEST_ASSERT_EQ_U32(base, sp1_euclid_compute_mask(3U, 8U, 8U));
    TEST_ASSERT_EQ_U32(sp1_euclid_compute_mask(3U, 8U, 3U),
                       sp1_euclid_compute_mask(3U, 8U, 11U));

    /* Full 32-step rotation wrapping across bit 31 -> bit 0 */
    const uint32_t m32_r0 = sp1_euclid_compute_mask(7U, 32U, 0U);
    const uint32_t m32_r1 = sp1_euclid_compute_mask(7U, 32U, 1U);
    const uint32_t m32_r32 = sp1_euclid_compute_mask(7U, 32U, 32U);
    TEST_ASSERT_EQ_INT(7, sp1_euclid_popcount(m32_r0));
    TEST_ASSERT_EQ_INT(7, sp1_euclid_popcount(m32_r1));
    TEST_ASSERT_EQ_U32(m32_r0, m32_r32);
    TEST_ASSERT(sp1_euclid_step_active(m32_r0, 0U));
    TEST_ASSERT(!sp1_euclid_step_active(m32_r1, 0U));
    TEST_ASSERT(sp1_euclid_step_active(m32_r1, 1U));

    /* O(1) RT step query bounds & polymeter wrap */
    TEST_ASSERT(!sp1_euclid_step_active(0xFFFFFFFFU, 32U));
    TEST_ASSERT(!sp1_euclid_step_active(0xFFFFFFFFU, 99U));
    TEST_ASSERT_EQ_INT(1, sp1_euclid_next_step(0U, 5U));
    TEST_ASSERT_EQ_INT(4, sp1_euclid_next_step(3U, 5U));
    TEST_ASSERT_EQ_INT(0, sp1_euclid_next_step(4U, 5U));
    TEST_ASSERT_EQ_INT(0, sp1_euclid_next_step(0U, 1U));

    /* Pattern mutators keep invariants consistent */
    sp1_euclid_pattern_t pat;
    sp1_euclid_pattern_set(&pat, 5U, 16U, 4U);
    sp1_euclid_pattern_set_steps(&pat, 4U);
    TEST_ASSERT_EQ_INT(4, pat.steps);
    TEST_ASSERT_EQ_INT(4, pat.pulses);
    TEST_ASSERT_EQ_INT(0, pat.rotation);
    TEST_ASSERT_EQ_U32(0x0000000FU, pat.mask);
}

int main(void) {
    RUN_TEST(test_edge_cases_and_clamping);
    RUN_TEST(test_canonical_bjorklund_rhythms);
    RUN_TEST(test_phase_rotations_and_polymeter_helpers);
    return sp1_test_summary();
}
