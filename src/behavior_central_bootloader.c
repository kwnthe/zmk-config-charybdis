/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Puts the CENTRAL into its UF2 bootloader.
 *
 * ZMK's own &bootloader cannot: it declares BEHAVIOR_LOCALITY_EVENT_SOURCE, so it
 * always flashes the half the key physically sits on and never the board resolving the
 * keymap. With a dongle that is the one device with no keys of its own, leaving its
 * reset button as the only way in.
 *
 * Same reboot sequence as app/src/behaviors/behavior_reset.c; the only difference is
 * the default CENTRAL locality, which is the entire point.
 *
 * Follows whatever is central at the time, by design: in dongle mode that is the
 * dongle, in standalone it is the right half. The key means "bootloader whichever
 * board is running the keymap", which is the one you are about to reflash anyway.
 */

#define DT_DRV_COMPAT zmk_behavior_central_bootloader

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <drivers/behavior.h>

#include <dt-bindings/zmk/reset.h>
#include <zmk/behavior.h>

#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

LOG_MODULE_REGISTER(charybdis_cboot, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    LOG_WRN("Rebooting the central into its bootloader");

#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
    int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (ret < 0) {
        LOG_ERR("Failed to set the bootloader mode (%d)", ret);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    sys_reboot(SYS_REBOOT_WARM);
#else
    /* RST_UF2 is the magic the Adafruit nRF52 bootloader looks for on a warm boot. */
    sys_reboot(RST_UF2);
#endif

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

/* No .locality: the default is BEHAVIOR_LOCALITY_CENTRAL, which is what makes this
 * different from &bootloader. */
static const struct behavior_driver_api central_bootloader_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define CBOOT_INST(n)                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                 \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &central_bootloader_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CBOOT_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
