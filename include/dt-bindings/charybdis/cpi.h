/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Parameters for &tb_cpi.
 *
 * Any param1 at or above PMW3610_MIN_CPI (200) is taken as an absolute CPI, so the
 * command values below are kept in the 0..2 range where they cannot collide with one.
 */

#pragma once

#define CPI_INC   0
#define CPI_DEC   1
#define CPI_RESET 2
