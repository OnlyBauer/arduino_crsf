/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_util.h
 * @brief Minimal assertion helpers for the host test binaries.
 *
 * Deliberately framework-free: these tests only need to link the protocol
 * sources (crsf_crc.c, crsf_codec.c, crsf_parser.c), which carry no ESP-IDF
 * dependency, so a plain gcc invocation is enough.
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <inttypes.h>

/** @brief Checks that failed in the current suite. */
static int tu_failures;
/** @brief Checks evaluated in the current suite. */
static int tu_checks;
/** @brief Name of the current suite, for the summary line. */
static const char *tu_suite;

/**
 * Start a suite and reset its counters.
 * @param suite Short suite name, printed in the summary.
 */
static inline void test_begin(const char *suite)
{
    tu_suite = suite;
    tu_failures = 0;
    tu_checks = 0;
}

/**
 * Print the suite summary.
 * @return 0 when every check passed, 1 otherwise, ready to return from
 *         main().
 */
static inline int test_end(void)
{
    if (tu_failures == 0) {
        printf("PASS  %-10s  %d checks\n", tu_suite, tu_checks);
        return 0;
    }
    printf("FAIL  %-10s  %d/%d checks failed\n", tu_suite, tu_failures, tu_checks);
    return 1;
}

/**
 * @brief Assert a condition, counting the check either way.
 *
 * Reported at the call site's line so a failure points straight at the case.
 *
 * @param cond Condition that must hold.
 * @param what Short description of what is being checked.
 */
#define CHECK(cond, what)                                                    \
    do {                                                                     \
        tu_checks++;                                                         \
        if (!(cond)) {                                                       \
            tu_failures++;                                                   \
            printf("  %s:%d: %s: expected %s\n", __FILE__, __LINE__, (what), \
                   #cond);                                                   \
        }                                                                    \
    } while (0)

/**
 * @brief Assert two signed values are equal, printing both on failure.
 * @param got  Value produced.
 * @param want Value expected.
 * @param what Short description of what is being checked.
 */
#define CHECK_EQ_INT(got, want, what)                                  \
    do {                                                               \
        tu_checks++;                                                   \
        int64_t g_ = (int64_t)(got), w_ = (int64_t)(want);             \
        if (g_ != w_) {                                                \
            tu_failures++;                                             \
            printf("  %s:%d: %s: got %" PRId64 ", want %" PRId64 "\n", \
                   __FILE__, __LINE__, (what), g_, w_);                \
        }                                                              \
    } while (0)

/**
 * @brief Assert two unsigned values are equal, printing both in hex on failure.
 * @param got  Value produced.
 * @param want Value expected.
 * @param what Short description of what is being checked.
 */
#define CHECK_EQ_HEX(got, want, what)                                      \
    do {                                                                   \
        tu_checks++;                                                       \
        uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);              \
        if (g_ != w_) {                                                    \
            tu_failures++;                                                 \
            printf("  %s:%d: %s: got 0x%" PRIx64 ", want 0x%" PRIx64 "\n", \
                   __FILE__, __LINE__, (what), g_, w_);                    \
        }                                                                  \
    } while (0)

/*
 * Invariant check for hot loops (fuzzing). Unlike CHECK it does not count every
 * evaluation — a fuzz run evaluates these millions of times and the totals would
 * be meaningless — and it reports each call site only once, so a systematic
 * violation produces one line instead of a flood. A failure still fails the suite.
 */
/**
 * @brief Assert an invariant inside a hot loop, reporting each site once.
 *
 * Unlike CHECK() it does not count every evaluation — a fuzz run evaluates these
 * millions of times and the totals would be meaningless — and it reports each
 * call site only once, so a systematic violation produces one line instead of a
 * flood. A failure still fails the suite.
 *
 * @param cond Invariant that must hold.
 * @param what Short description of the invariant.
 */
#define CHECK_BOUND(cond, what)                                            \
    do {                                                                   \
        if (!(cond)) {                                                     \
            static int reported_;                                          \
            if (!reported_) {                                              \
                reported_ = 1;                                             \
                tu_failures++;                                             \
                printf("  %s:%d: invariant violated: %s (%s)\n", __FILE__, \
                       __LINE__, (what), #cond);                           \
            }                                                              \
        }                                                                  \
    } while (0)

/**
 * @brief Assert two buffers are equal, hex-dumping both on failure.
 * @param got  Buffer produced.
 * @param want Buffer expected.
 * @param len  Bytes to compare.
 * @param what Short description of what is being checked.
 */
#define CHECK_MEM(got, want, len, what)                                \
    do {                                                               \
        tu_checks++;                                                   \
        if (memcmp((got), (want), (len)) != 0) {                       \
            tu_failures++;                                             \
            printf("  %s:%d: %s: buffers differ\n    got: ", __FILE__, \
                   __LINE__, (what));                                  \
            for (size_t i_ = 0; i_ < (size_t)(len); i_++)              \
                printf("%02X ", ((const uint8_t *)(got))[i_]);         \
            printf("\n   want: ");                                     \
            for (size_t i_ = 0; i_ < (size_t)(len); i_++)              \
                printf("%02X ", ((const uint8_t *)(want))[i_]);        \
            printf("\n");                                              \
        }                                                              \
    } while (0)

#endif /* TEST_UTIL_H */
