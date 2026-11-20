#include "test_harness.h"

#include <stdint.h>

static void test_fixed_width_types(void) {
    TEST_ASSERT_EQ_INT(1, (int)sizeof(uint8_t));
    TEST_ASSERT_EQ_INT(2, (int)sizeof(int16_t));
    TEST_ASSERT_EQ_INT(4, (int)sizeof(uint32_t));
    TEST_ASSERT_EQ_INT(4, (int)sizeof(float));
}

static void test_phase_accumulator_wrap(void) {
    /* 12 kHz at 48 kHz sample rate = 1/4 cycle per sample = 0x40000000 */
    const uint32_t phase_inc = 0x40000000U;
    uint32_t phase = 0U;

    phase += phase_inc;
    TEST_ASSERT_EQ_U32(0x40000000U, phase);
    phase += phase_inc;
    TEST_ASSERT_EQ_U32(0x80000000U, phase);
    phase += phase_inc;
    TEST_ASSERT_EQ_U32(0xC0000000U, phase);
    phase += phase_inc;
    TEST_ASSERT_EQ_U32(0x00000000U, phase);
}

static void test_float_math_tolerance(void) {
    const float sample_rate_hz = 48000.0f;
    const float block_samples = 64.0f;
    const float block_ms = (block_samples / sample_rate_hz) * 1000.0f;

    TEST_ASSERT_FLOAT_NEAR(1.333333f, block_ms, 1e-4f);
}

int main(void) {
    RUN_TEST(test_fixed_width_types);
    RUN_TEST(test_phase_accumulator_wrap);
    RUN_TEST(test_float_math_tolerance);
    return sp1_test_summary();
}
