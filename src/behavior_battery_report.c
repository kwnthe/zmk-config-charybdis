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

#include <charybdis/typer.h>

LOG_MODULE_REGISTER(charybdis_batt, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if defined(CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)
#define PERIPHERAL_SLOTS CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS
#else
#define PERIPHERAL_SLOTS 2
#endif

/* 0xFF means that slot has never reported. */
#define SOC_UNKNOWN 0xFF

struct battery_report_config {
    uint16_t tap_ms;
};

static uint8_t peripheral_soc[PERIPHERAL_SLOTS] = {[0 ...(PERIPHERAL_SLOTS - 1)] = SOC_UNKNOWN};

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (dev == NULL || charybdis_typer_busy()) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    const struct battery_report_config *cfg = dev->config;

    charybdis_typer_begin();

    bool any = false;
    for (uint8_t i = 0; i < PERIPHERAL_SLOTS; i++) {
        if (peripheral_soc[i] == SOC_UNKNOWN) {
            continue;
        }
        if (any) {
            charybdis_typer_str(" ");
        }
        charybdis_typer_str("B");
        charybdis_typer_num(i);
        charybdis_typer_str(" ");
        charybdis_typer_num(peripheral_soc[i]);
        any = true;
    }

    if (!any) {
        /* Levels arrive on a report interval, not at boot, so early presses find
         * nothing. Say so rather than type an empty line. */
        charybdis_typer_str("NA");
    }

    charybdis_typer_send(cfg->tap_ms);

    return ZMK_BEHAVIOR_OPAQUE;
}

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

/* The peripheral battery event only ever fires on a central; elsewhere the behavior
 * simply has nothing to report. */
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
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &battery_report_config_##n, POST_KERNEL,           \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &battery_report_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BATT_REPORT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
