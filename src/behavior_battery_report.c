/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Types the split halves' battery percentages, because on this keyboard there is
 * nowhere else for them to appear. ZMK publishes battery only via the BLE GATT Battery
 * Service (see bt_bas_set_battery_level in app/src/battery.c) and USB HID has no
 * battery channel, so a dongle cabled to the host can never tell it. The dongle does
 * hold the numbers -- CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING collects
 * them -- so this spells them out as keystrokes instead.
 *
 * Default CENTRAL locality is deliberate and correct here: the central is both where
 * the levels are cached and where HID output is produced. Unlike &tb_cpi this never
 * crosses the split, so the 9-byte behavior-name limit does not apply.
 */

#define DT_DRV_COMPAT zmk_behavior_battery_report

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_REGISTER(charybdis_batt, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * Peripherals do not link the keycode event -- they forward positions and never build
 * HID reports -- so raise_zmk_keycode_state_changed is simply absent there and merely
 * referencing it fails the link. Everything that types is gated on being able to.
 */
#define BATT_CAN_REPORT (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

struct battery_report_config {
    uint16_t tap_ms;
};

#if BATT_CAN_REPORT

#if defined(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)
#define PERIPHERAL_SLOTS CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#else
#define PERIPHERAL_SLOTS 2
#endif

/* HID keyboard usage page, and the usage ids for the characters we emit. Spelled out
 * rather than pulled from dt-bindings/zmk/keys.h, whose single-letter macros (A, B, ...)
 * would collide with ordinary identifiers in C. */
#define ENC(usage) (((uint32_t)0x07 << 16) | (usage))
#define USAGE_1 0x1E
#define USAGE_0 0x27
#define USAGE_A 0x04
#define USAGE_SPACE 0x2C

/* Long enough for "B0 100 B1 100" plus slack. */
#define MAX_CHARS 40

/* One instance drives one output stream, so this state is file-scope: the typing work
 * item cannot carry a device pointer without a lookup, and a second instance would be
 * meaningless anyway. */
static uint32_t queue[MAX_CHARS];
static uint8_t queue_len;
static uint8_t queue_idx;
static bool key_is_down;
static uint16_t queue_tap_ms = 12;
static struct k_work_delayable type_work;

/* Slot -> percent, 0xFF meaning "never reported". */
static uint8_t peripheral_soc[PERIPHERAL_SLOTS];

static uint32_t enc_digit(uint8_t d) { return ENC(d == 0 ? USAGE_0 : USAGE_1 + d - 1); }

static uint32_t enc_letter(char c) { return ENC(USAGE_A + (c - 'A')); }

static void push(uint32_t encoded) {
    if (queue_len < MAX_CHARS) {
        queue[queue_len++] = encoded;
    }
}

static void push_number(uint8_t n) {
    if (n >= 100) {
        push(enc_digit(n / 100));
    }
    if (n >= 10) {
        push(enc_digit((n / 10) % 10));
    }
    push(enc_digit(n % 10));
}

/* Two work passes per character: press, then release. Doing both in one pass would put
 * a press and a release in the same HID report, and the host would see nothing. */
static void type_work_cb(struct k_work *work) {
    if (queue_idx >= queue_len) {
        queue_len = 0;
        queue_idx = 0;
        return;
    }

    const int64_t now = k_uptime_get();

    raise_zmk_keycode_state_changed_from_encoded(queue[queue_idx], !key_is_down, now);

    if (key_is_down) {
        key_is_down = false;
        queue_idx++;
    } else {
        key_is_down = true;
    }

    k_work_reschedule(&type_work, K_MSEC(queue_tap_ms));
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    /* Still spelling out the previous report -- ignore rather than interleave. */
    if (queue_len > 0) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    queue_idx = 0;
    key_is_down = false;

    bool any = false;
    for (uint8_t i = 0; i < PERIPHERAL_SLOTS; i++) {
        if (peripheral_soc[i] == 0xFF) {
            continue;
        }
        if (any) {
            push(ENC(USAGE_SPACE));
        }
        push(enc_letter('B'));
        push(enc_digit(i));
        push(ENC(USAGE_SPACE));
        push_number(peripheral_soc[i]);
        any = true;
    }

    if (!any) {
        /* Nothing reported yet: say so rather than type an empty line. */
        push(enc_letter('N'));
        push(enc_letter('A'));
    }

    LOG_DBG("Typing battery report, %d chars", queue_len);
    k_work_reschedule(&type_work, K_NO_WAIT);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int battery_report_init(const struct device *dev) {
    const struct battery_report_config *cfg = dev->config;

    queue_tap_ms = cfg->tap_ms;
    for (uint8_t i = 0; i < PERIPHERAL_SLOTS; i++) {
        peripheral_soc[i] = 0xFF;
    }
    k_work_init_delayable(&type_work, type_work_cb);

    return 0;
}

#else /* !BATT_CAN_REPORT */

/* Still instantiated so the one shared keymap keeps building for every shield; there is
 * just nothing here to report, and no way to say it. */
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int battery_report_init(const struct device *dev) { return 0; }

#endif /* BATT_CAN_REPORT */

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api battery_report_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

/* The peripheral battery event only ever fires on a central, and on other roles the
 * behavior simply has nothing to report. */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int battery_report_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev != NULL && ev->source < PERIPHERAL_SLOTS) {
        peripheral_soc[ev->source] = ev->state_of_charge;
        LOG_DBG("Peripheral %d battery %d%%", ev->source, ev->state_of_charge);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(charybdis_battery_report, battery_report_listener);
ZMK_SUBSCRIPTION(charybdis_battery_report, zmk_peripheral_battery_state_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

#define BATT_REPORT_INST(n)                                                                        \
    static const struct battery_report_config battery_report_config_##n = {                        \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                          \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, battery_report_init, NULL, NULL, &battery_report_config_##n,         \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                       \
                            &battery_report_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BATT_REPORT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
