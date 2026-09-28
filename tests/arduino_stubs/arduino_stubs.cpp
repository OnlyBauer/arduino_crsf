/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file arduino_stubs.cpp
 * @brief The mock Arduino clock. See Arduino.h.
 */

#include "Arduino.h"

/** The clock the test moves. */
static uint32_t s_micros;

uint32_t micros(void)
{
    return s_micros;
}

uint32_t millis(void)
{
    return s_micros / 1000u;
}

void mock_set_micros(uint32_t us)
{
    s_micros = us;
}

void mock_advance_micros(uint32_t us)
{
    s_micros += us;
}
