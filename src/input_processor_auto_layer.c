/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Auto-mouse layer that decides for itself which keys belong to it.
 *
 * Upstream's zmk,input-processor-temp-layer takes an excluded-positions list, and any
 * position bound on the layer but missing from that list is dead: the layer is torn
 * down on press, before the binding resolves, so the press falls through to the base
 * layer. Every binding added from a keymap editor then needs a matching firmware edit.
 *
 * This one asks the keymap instead. On each press it reads the binding the target
 * layer carries at that position:
 *
 *   bound (anything but &trans / &none)  ->  mine; refresh the timeout, layer persists
 *   transparent / none                   ->  not mine; drop the layer immediately
 */

#define DT_DRV_COMPAT zmk_input_processor_auto_layer

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_REGISTER(charybdis_auto_layer, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

struct auto_layer_config {
    int16_t require_prior_idle_ms;
};

struct auto_layer_data {
    struct k_mutex lock;
    uint8_t layer;
    uint32_t timeout_ms;
    bool is_active;
    int64_t last_tapped;
};

/* One instance drives one layer, but the disable work has to be per layer because the
 * work item carries the scheduling state. */
static struct k_work_delayable disable_works[MAX_LAYERS];
static const struct device *owner_of_layer[MAX_LAYERS];

static void set_active(struct auto_layer_data *data, bool active) {
    if (data->is_active == active) {
        return;
    }

    data->is_active = active;
    if (active) {
        zmk_keymap_layer_activate(data->layer, false);
    } else {
        zmk_keymap_layer_deactivate(data->layer, false);
    }
}

static void disable_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    int layer = ARRAY_INDEX(disable_works, dwork);

    const struct device *dev = owner_of_layer[layer];
    if (dev == NULL) {
        return;
    }

    struct auto_layer_data *data = dev->data;
    if (k_mutex_lock(&data->lock, K_MSEC(10)) < 0) {
        return;
    }

    set_active(data, false);

    k_mutex_unlock(&data->lock);
}

/* A press belongs to this layer if the layer binds it to something real. Reading the
 * live keymap is what keeps editor-made changes working with no firmware edit -- and it
 * follows &trans / &none automatically, since those are the markers for "not mine". */
static bool position_is_bound_on_layer(uint8_t layer, uint32_t position) {
    const struct zmk_behavior_binding *binding =
        zmk_keymap_get_layer_binding_at_idx(layer, (uint16_t)position);

    if (binding == NULL || binding->behavior_dev == NULL) {
        return false;
    }

    const struct device *bound = zmk_behavior_get_binding(binding->behavior_dev);

    return bound != NULL && bound != DEVICE_DT_GET(DT_NODELABEL(trans)) &&
           bound != DEVICE_DT_GET(DT_NODELABEL(none));
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct auto_layer_data *data = dev->data;
    if (k_mutex_lock(&data->lock, K_MSEC(10)) < 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->is_active) {
        if (position_is_bound_on_layer(data->layer, ev->position)) {
            /* Still mousing -- push the deadline out so a click does not cost the layer. */
            k_work_reschedule(&disable_works[data->layer], K_MSEC(data->timeout_ms));
            LOG_DBG("Position %d bound on layer %d, extending", ev->position, data->layer);
        } else {
            /* Back to typing. Dropping now rather than waiting out the timeout is what
             * keeps the layer from stealing the next few keystrokes. Safe to do before
             * the keymap resolves this press: the layer is transparent here by
             * definition, so it would have fallen through to the base layer anyway. */
            set_active(data, false);
            k_work_cancel_delayable(&disable_works[data->layer]);
            LOG_DBG("Position %d not bound on layer %d, dropping", ev->position, data->layer);
        }
    }

    k_mutex_unlock(&data->lock);

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct auto_layer_data *data = dev->data;
    if (k_mutex_lock(&data->lock, K_MSEC(10)) < 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    data->last_tapped = ev->timestamp;

    k_mutex_unlock(&data->lock);

    return ZMK_EV_EVENT_BUBBLE;
}

/* Something else moved the layer (a &mo, a &tog); stop claiming ownership of it. */
static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct auto_layer_data *data = dev->data;
    if (k_mutex_lock(&data->lock, K_MSEC(10)) < 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->is_active && !zmk_keymap_layer_active(data->layer)) {
        data->is_active = false;
        k_work_cancel_delayable(&disable_works[data->layer]);
    }

    k_mutex_unlock(&data->lock);

    return ZMK_EV_EVENT_BUBBLE;
}

static int dispatch(const struct device *dev, const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return handle_position_state_changed(dev, eh);
    }
    if (as_zmk_keycode_state_changed(eh) != NULL) {
        return handle_keycode_state_changed(dev, eh);
    }
    if (as_zmk_layer_state_changed(eh) != NULL) {
        return handle_layer_state_changed(dev, eh);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#define DISPATCH_EVENT(inst)                                                                       \
    {                                                                                              \
        int err = dispatch(DEVICE_DT_INST_GET(inst), eh);                                           \
        if (err < 0) {                                                                             \
            return err;                                                                            \
        }                                                                                          \
    }

static int event_dispatcher(const zmk_event_t *eh) {
    DT_INST_FOREACH_STATUS_OKAY(DISPATCH_EVENT)

    return 0;
}

static int auto_layer_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    if (param1 >= MAX_LAYERS) {
        LOG_ERR("Invalid layer %d", param1);
        return -EINVAL;
    }

    const struct auto_layer_config *cfg = dev->config;
    struct auto_layer_data *data = dev->data;

    if (k_mutex_lock(&data->lock, K_FOREVER) < 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    data->layer = param1;
    data->timeout_ms = param2;
    owner_of_layer[param1] = dev;

    /* Mid-typing brush of the ball must not raise the layer. */
    bool too_soon = (data->last_tapped + cfg->require_prior_idle_ms) > k_uptime_get();

    if (!data->is_active && !too_soon) {
        set_active(data, true);
    }

    if (data->is_active && param2 > 0) {
        k_work_reschedule(&disable_works[param1], K_MSEC(param2));
    }

    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int auto_layer_init(const struct device *dev) {
    struct auto_layer_data *data = dev->data;
    k_mutex_init(&data->lock);

    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&disable_works[i], disable_work_cb);
    }

    return 0;
}

static const struct zmk_input_processor_driver_api auto_layer_driver_api = {
    .handle_event = auto_layer_handle_event,
};

ZMK_LISTENER(processor_auto_layer, event_dispatcher);
ZMK_SUBSCRIPTION(processor_auto_layer, zmk_position_state_changed);
ZMK_SUBSCRIPTION(processor_auto_layer, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(processor_auto_layer, zmk_layer_state_changed);

#define AUTO_LAYER_INST(n)                                                                         \
    static struct auto_layer_data processor_auto_layer_data_##n = {};                               \
    static const struct auto_layer_config processor_auto_layer_config_##n = {                       \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                      \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, auto_layer_init, NULL, &processor_auto_layer_data_##n,                 \
                          &processor_auto_layer_config_##n, POST_KERNEL,                            \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &auto_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(AUTO_LAYER_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
