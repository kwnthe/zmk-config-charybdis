/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Types text by raising keycode events, for reporting values the keyboard has no other
 * way to show: no display on the dongle, and USB HID carries no side channel.
 *
 * One shared queue, so two report keys pressed together serialise instead of
 * interleaving their digits. Reject work while busy() rather than queueing behind it --
 * a stale report typed later is worse than none.
 *
 * Peripherals do not link raise_zmk_keycode_state_changed (they forward key positions
 * and never build HID reports), so on those builds every function here compiles to a
 * no-op and busy() is always false.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CHARYBDIS_TYPER_CAN_TYPE                                                                   \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

/* True while a report is still being spelled out. */
bool charybdis_typer_busy(void);

/* Discard anything pending and start a fresh report. */
void charybdis_typer_begin(void);

/* Append to the pending report. Letters are upper-cased; anything outside
 * [A-Z a-z 0-9 space] is skipped rather than guessed at. */
void charybdis_typer_str(const char *s);
void charybdis_typer_num(uint32_t n);

/* Send it. tap_ms separates each key event; a character costs two of them. */
void charybdis_typer_send(uint16_t tap_ms);
