/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Counts keystrokes and types the totals back on request.
 *
 * Runs on the central, which is where keycode events are raised and where HID output
 * happens. In dongle mode that is the dongle -- USB powered, so the counting costs no
 * battery at all. Note the consequence: switch to standalone and the counter moves to
 * the right half, which keeps its own separate history.
 *
 * Persistence has two layers. NVS holds the running totals and survives reboots and
 * firmware flashes; settings_reset wipes it, because erasing that partition is exactly
 * what settings_reset is for and the nice!nano flash map has no spare region to hide in.
 * The baseline-* devicetree properties cover that case: they seed the counters only when
 * NVS comes up empty, so a total read out before a reset can be pasted back into the
 * devicetree and counting resumes from it. A normal flash leaves NVS intact, so the
 * baseline is ignored and nothing is double-counted.
 */

#define DT_DRV_COMPAT zmk_behavior_typing_stats

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/keycode_state_changed.h>

#include <charybdis/typer.h>

LOG_MODULE_REGISTER(charybdis_stats, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* HID keyboard page and the usage ids we classify on. */
#define PAGE_KEYBOARD 0x07
#define USAGE_A 0x04
#define USAGE_Z 0x1D
#define USAGE_ENTER 0x28
#define USAGE_BSPC 0x2A
#define USAGE_TAB 0x2B
#define USAGE_SPACE 0x2C

/* A gap longer than this is a pause, not typing, and is left out of active time --
 * otherwise a night of idling would drag the average to nothing. */
#define IDLE_GAP_MS 3000

/* Flash wear, not correctness, sets this. Saving per keystroke would burn through the
 * nRF52840's ~10k erase cycles per page; a save every N keys plus one on going idle
 * keeps it to a handful per hour. */
#define SAVE_EVERY_KEYS 1000

#define SETTINGS_KEY "charybdis/stats"

struct stats {
    uint32_t keys;
    uint32_t words;
    uint32_t bksp;
    uint64_t active_ms;
};

struct stats_config {
    uint16_t tap_ms;
    struct stats baseline;
};

/*
 * The keycode event lives only where HID is produced -- a peripheral forwards key
 * positions and never links it, so even referencing as_zmk_keycode_state_changed there
 * fails the link. Same predicate as the typer: if it cannot type, it cannot count.
 */
#if CHARYBDIS_TYPER_CAN_TYPE

static struct stats totals;
static int64_t last_press_ms;
static bool word_pending;
static uint32_t keys_since_save;
static bool loaded_from_nvs;

static void save(void) {
    int ret = settings_save_one(SETTINGS_KEY, &totals, sizeof(totals));
    if (ret < 0) {
        LOG_WRN("Failed to save stats (%d)", ret);
        return;
    }

    keys_since_save = 0;
    LOG_DBG("Saved stats: %u keys", totals.keys);
}

static int stats_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    if (settings_name_next(name, NULL) != 0 || len != sizeof(totals)) {
        return -ENOENT;
    }

    int ret = read_cb(cb_arg, &totals, sizeof(totals));
    if (ret <= 0) {
        return ret;
    }

    loaded_from_nvs = true;
    LOG_DBG("Loaded stats: %u keys", totals.keys);

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(charybdis_stats, SETTINGS_KEY, NULL, stats_settings_set, NULL, NULL);

static void count(uint16_t page, uint32_t id, int64_t now) {
    /* Active time accrues only across gaps short enough to be typing. */
    if (last_press_ms > 0) {
        int64_t gap = now - last_press_ms;
        if (gap > 0 && gap < IDLE_GAP_MS) {
            totals.active_ms += gap;
        }
    }
    last_press_ms = now;

    totals.keys++;
    keys_since_save++;

    if (page == PAGE_KEYBOARD) {
        if (id >= USAGE_A && id <= USAGE_Z) {
            word_pending = true;
        } else if (id == USAGE_BSPC) {
            totals.bksp++;
        } else if (id == USAGE_SPACE || id == USAGE_ENTER || id == USAGE_TAB) {
            /* A boundary only closes a word if letters came before it, so runs of
             * spaces do not inflate the count. */
            if (word_pending) {
                totals.words++;
                word_pending = false;
            }
        }
    }

    if (keys_since_save >= SAVE_EVERY_KEYS) {
        save();
    }
}

static int stats_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    if (kc != NULL) {
        /* Skip the characters our own reports type. The typer raises the same event the
         * counter listens to, so without this a stats line adds its own ~35 characters
         * to the total it just reported, and a battery line adds ~12. */
        if (kc->state && !charybdis_typer_is_emitting()) {
            count(kc->usage_page, kc->keycode, kc->timestamp);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);
    if (act != NULL && act->state != ZMK_ACTIVITY_ACTIVE && keys_since_save > 0) {
        /* Going idle is the natural moment to persist: the work is already happening
         * and nothing is being typed. */
        save();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(charybdis_stats, stats_listener);
ZMK_SUBSCRIPTION(charybdis_stats, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(charybdis_stats, zmk_activity_state_changed);

/* Gross WPM: five keystrokes to a word, over active minutes only. */
static uint32_t wpm(void) {
    if (totals.active_ms < 1000) {
        return 0;
    }

    return (uint32_t)(((uint64_t)totals.keys * 12000ULL) / totals.active_ms);
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (dev == NULL || charybdis_typer_busy()) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    const struct stats_config *cfg = dev->config;

    /* The counters are about to be typed, so persist first -- if the point of reading
     * them is to note them down before a settings_reset, what is on flash should match
     * what appears on screen. */
    if (keys_since_save > 0) {
        save();
    }

    charybdis_typer_begin();
    charybdis_typer_num(totals.keys);
    charybdis_typer_str(" keys ");
    charybdis_typer_num(totals.words);
    charybdis_typer_str(" words ");
    charybdis_typer_num(wpm());
    charybdis_typer_str(" wpm ");
    charybdis_typer_num(totals.bksp);
    charybdis_typer_str(" bksp");
    charybdis_typer_send(cfg->tap_ms);

    return ZMK_BEHAVIOR_OPAQUE;
}

static int stats_init(const struct device *dev) {
    const struct stats_config *cfg = dev->config;

    /* Settings load runs before POST_KERNEL device init, so NVS has already had its say.
     * Seed from the devicetree baseline only if it stayed silent -- that is the
     * post-settings_reset case. */
    if (!loaded_from_nvs) {
        totals = cfg->baseline;
        LOG_DBG("No stored stats, seeding from baseline: %u keys", totals.keys);
    }

    return 0;
}

#else /* !CHARYBDIS_TYPER_CAN_TYPE */

/* Instantiated anyway so the one shared keymap keeps building for every shield. */
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                    struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int stats_init(const struct device *dev) { return 0; }

#endif /* CHARYBDIS_TYPER_CAN_TYPE */

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api stats_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define STATS_INST(n)                                                                              \
    static const struct stats_config stats_config_##n = {                                          \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                          \
        .baseline =                                                                                \
            {                                                                                      \
                .keys = DT_INST_PROP(n, baseline_keys),                                             \
                .words = DT_INST_PROP(n, baseline_words),                                           \
                .bksp = DT_INST_PROP(n, baseline_bksp),                                             \
                .active_ms = (uint64_t)DT_INST_PROP(n, baseline_active_sec) * 1000ULL,              \
            },                                                                                     \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, stats_init, NULL, NULL, &stats_config_##n, POST_KERNEL,              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &stats_driver_api);

DT_INST_FOREACH_STATUS_OKAY(STATS_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
