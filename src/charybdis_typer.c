/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * See include/charybdis/typer.h for why this exists.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <charybdis/typer.h>

LOG_MODULE_REGISTER(charybdis_typer, CONFIG_ZMK_LOG_LEVEL);

#if CHARYBDIS_TYPER_CAN_TYPE

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

/* HID keyboard usage page and the ids we can emit. Spelled out rather than taken from
 * dt-bindings/zmk/keys.h, whose single-letter macros collide with C identifiers. */
#define ENC(usage) (((uint32_t)0x07 << 16) | (usage))
#define USAGE_A 0x04
#define USAGE_1 0x1E
#define USAGE_0 0x27
#define USAGE_SPACE 0x2C

/* Enough for a full stats line with room to spare. */
#define MAX_CHARS 64

static uint32_t queue[MAX_CHARS];
static uint8_t queue_len;
static uint8_t queue_idx;
static bool key_is_down;
static uint16_t tap_ms = 12;

static void type_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(type_work, type_work_cb);

/* Two passes per character. Emitting press and release in one pass would put both in
 * the same HID report and the host would see nothing at all. */
static void type_work_cb(struct k_work *work) {
    if (queue_idx >= queue_len) {
        queue_len = 0;
        queue_idx = 0;
        return;
    }

    raise_zmk_keycode_state_changed_from_encoded(queue[queue_idx], !key_is_down, k_uptime_get());

    if (key_is_down) {
        key_is_down = false;
        queue_idx++;
    } else {
        key_is_down = true;
    }

    k_work_reschedule(&type_work, K_MSEC(tap_ms));
}

static void push(uint32_t encoded) {
    if (queue_len < MAX_CHARS) {
        queue[queue_len++] = encoded;
    }
}

bool charybdis_typer_busy(void) { return queue_len > 0; }

void charybdis_typer_begin(void) {
    queue_len = 0;
    queue_idx = 0;
    key_is_down = false;
}

void charybdis_typer_str(const char *s) {
    for (; *s != '\0'; s++) {
        char c = *s;
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        }

        if (c >= 'A' && c <= 'Z') {
            push(ENC(USAGE_A + (c - 'A')));
        } else if (c >= '1' && c <= '9') {
            push(ENC(USAGE_1 + (c - '1')));
        } else if (c == '0') {
            push(ENC(USAGE_0));
        } else if (c == ' ') {
            push(ENC(USAGE_SPACE));
        }
        /* Anything else is skipped rather than guessed at. */
    }
}

void charybdis_typer_num(uint32_t n) {
    char buf[11];
    int i = sizeof(buf) - 1;

    buf[i--] = '\0';
    if (n == 0) {
        buf[i--] = '0';
    }
    while (n > 0 && i >= 0) {
        buf[i--] = '0' + (n % 10);
        n /= 10;
    }

    charybdis_typer_str(&buf[i + 1]);
}

void charybdis_typer_send(uint16_t ms) {
    if (queue_len == 0) {
        return;
    }

    tap_ms = ms > 0 ? ms : 12;
    LOG_DBG("Typing %d chars", queue_len);
    k_work_reschedule(&type_work, K_NO_WAIT);
}

#else /* !CHARYBDIS_TYPER_CAN_TYPE */

bool charybdis_typer_busy(void) { return false; }
void charybdis_typer_begin(void) {}
void charybdis_typer_str(const char *s) { ARG_UNUSED(s); }
void charybdis_typer_num(uint32_t n) { ARG_UNUSED(n); }
void charybdis_typer_send(uint16_t ms) { ARG_UNUSED(ms); }

#endif /* CHARYBDIS_TYPER_CAN_TYPE */
