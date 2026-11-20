#ifndef SP1_DRUM_TEST_HARNESS_H
#define SP1_DRUM_TEST_HARNESS_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>

static int g_sp1_tests_run = 0;
static int g_sp1_tests_failed = 0;
static int g_sp1_current_failed = 0;

#define TEST_ASSERT(cond)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "  FAIL %s:%d: assertion failed: %s\n",              \
                    __FILE__, __LINE__, #cond);                                  \
            g_sp1_current_failed = 1;                                            \
            return;                                                              \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_EQ_INT(expected, actual)                                     \
    do {                                                                         \
        long long _exp = (long long)(expected);                                  \
        long long _act = (long long)(actual);                                    \
        if (_exp != _act) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: expected %lld, got %lld (%s)\n",      \
                    __FILE__, __LINE__, _exp, _act, #actual);                    \
            g_sp1_current_failed = 1;                                            \
            return;                                                              \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_EQ_U32(expected, actual)                                     \
    do {                                                                         \
        uint32_t _exp = (uint32_t)(expected);                                    \
        uint32_t _act = (uint32_t)(actual);                                      \
        if (_exp != _act) {                                                      \
            fprintf(stderr, "  FAIL %s:%d: expected 0x%08X, got 0x%08X (%s)\n",  \
                    __FILE__, __LINE__, _exp, _act, #actual);                    \
            g_sp1_current_failed = 1;                                            \
            return;                                                              \
        }                                                                        \
    } while (0)

#define TEST_ASSERT_FLOAT_NEAR(expected, actual, tol)                            \
    do {                                                                         \
        float _exp = (float)(expected);                                          \
        float _act = (float)(actual);                                            \
        float _tol = (float)(tol);                                               \
        float _diff = fabsf(_exp - _act);                                        \
        if (_diff > _tol || isnan(_act)) {                                       \
            fprintf(stderr,                                                      \
                    "  FAIL %s:%d: expected %.6f +/- %.6f, got %.6f (%s)\n",     \
                    __FILE__, __LINE__, (double)_exp, (double)_tol,              \
                    (double)_act, #actual);                                      \
            g_sp1_current_failed = 1;                                            \
            return;                                                              \
        }                                                                        \
    } while (0)

#define RUN_TEST(fn)                                                             \
    do {                                                                         \
        g_sp1_tests_run++;                                                       \
        g_sp1_current_failed = 0;                                                \
        fn();                                                                    \
        if (g_sp1_current_failed) {                                              \
            g_sp1_tests_failed++;                                                \
            fprintf(stderr, "[FAIL] %s\n", #fn);                                 \
        } else {                                                                 \
            printf("[PASS] %s\n", #fn);                                          \
        }                                                                        \
    } while (0)

static inline int sp1_test_summary(void) {
    printf("Summary: %d/%d tests passed\n",
           g_sp1_tests_run - g_sp1_tests_failed, g_sp1_tests_run);
    return (g_sp1_tests_failed == 0) ? 0 : 1;
}

#endif /* SP1_DRUM_TEST_HARNESS_H */
