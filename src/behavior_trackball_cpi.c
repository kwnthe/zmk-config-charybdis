/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Runtime CPI control for the PMW3610 trackball, so finding a comfortable pointer
 * speed does not cost a devicetree edit and a reflash each time.
 *
 * Not persisted: CPI returns to `cpi-default` on every reboot. Once a value feels
 * right, bake it into `cpi` in charybdis_right_common.dtsi (and rescale the divisors
 * in charybdis_trackball_processors.dtsi alongside it).
 */

#define DT_DRV_COMPAT zmk_behavior_trackball_cpi

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>

#include <dt-bindings/charybdis/cpi.h>

LOG_MODULE_REGISTER(charybdis_cpi, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/*
 * PMW3610_ALT_ATTR_CPI, the first member of `enum pmw3610_alt_attribute` in
 * zmk-pmw3610-driver/src/pmw3610.h. That header lives under the driver's src/ and is
 * not on any include path -- its CMakeLists only exports ${APPLICATION_SOURCE_DIR}/include
 * -- so the value is mirrored rather than included. The driver dispatches on
 * `switch ((uint32_t)attr)`, so the plain integer is what it compares against.
 */
#define PMW3610_ATTR_CPI 0

struct behavior_tb_cpi_config {
    const struct device *sensor;
    uint16_t cpi_min;
    uint16_t cpi_max;
    uint16_t cpi_step;
    uint16_t cpi_default;
};

struct behavior_tb_cpi_data {
    uint16_t current;
};

/* Clamp into range, then round down onto a step boundary: the driver derives the
 * register value as cpi/200, so an off-grid value would quietly land somewhere else. */
static uint16_t clamp_to_step(const struct behavior_tb_cpi_config *cfg, int32_t cpi) {
    if (cpi > (int32_t)cfg->cpi_max) {
        cpi = cfg->cpi_max;
    }
    if (cfg->cpi_step > 0) {
        cpi -= cpi % cfg->cpi_step;
    }
    if (cpi < (int32_t)cfg->cpi_min) {
        cpi = cfg->cpi_min;
    }

    return (uint16_t)cpi;
}

static void apply_cpi(const struct device *dev, uint16_t cpi) {
    const struct behavior_tb_cpi_config *cfg = dev->config;
    struct behavior_tb_cpi_data *data = dev->data;

    if (cfg->sensor == NULL) {
        LOG_DBG("No trackball on this half, ignoring CPI change");
        return;
    }

    if (!device_is_ready(cfg->sensor)) {
        LOG_WRN("Trackball not ready, ignoring CPI change");
        return;
    }

    /* Reaching for .attr_set through the driver API rather than sensor_attr_set():
     * the latter is a Zephyr syscall wrapper and would pull CONFIG_SENSOR into a build
     * that needs nothing else from the sensor subsystem. */
    const struct sensor_driver_api *api = cfg->sensor->api;
    if (api == NULL || api->attr_set == NULL) {
        LOG_ERR("Trackball exposes no attr_set");
        return;
    }

    const struct sensor_value val = {.val1 = (int32_t)cpi, .val2 = 0};
    int ret = api->attr_set(cfg->sensor, SENSOR_CHAN_ALL, (enum sensor_attribute)PMW3610_ATTR_CPI,
                            &val);
    if (ret < 0) {
        /* -EBUSY here means the sensor's async init has not finished yet. */
        LOG_WRN("Failed to set CPI to %u (%d)", cpi, ret);
        return;
    }

    data->current = cpi;
    LOG_INF("Trackball CPI now %u", cpi);
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (dev == NULL) {
        return ZMK_BEHAVIOR_OPAQUE;
    }

    const struct behavior_tb_cpi_config *cfg = dev->config;
    struct behavior_tb_cpi_data *data = dev->data;

    int32_t target;
    switch (binding->param1) {
    case CPI_INC:
        target = (int32_t)data->current + cfg->cpi_step;
        break;
    case CPI_DEC:
        target = (int32_t)data->current - cfg->cpi_step;
        break;
    case CPI_RESET:
        target = cfg->cpi_default;
        break;
    default:
        /* Anything >= cpi_min is an absolute value; see dt-bindings/charybdis/cpi.h. */
        target = (int32_t)binding->param1;
        break;
    }

    apply_cpi(dev, clamp_to_step(cfg, target));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_tb_cpi_init(const struct device *dev) {
    const struct behavior_tb_cpi_config *cfg = dev->config;
    struct behavior_tb_cpi_data *data = dev->data;

    /* Mirrors the sensor's own devicetree `cpi`; the hardware is never read back. */
    data->current = cfg->cpi_default;

    return 0;
}

static const struct behavior_driver_api behavior_tb_cpi_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define TB_CPI_SENSOR(n)                                                                           \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(n, device), (DEVICE_DT_GET(DT_INST_PHANDLE(n, device))),     \
                (NULL))

#define TB_CPI_INST(n)                                                                             \
    static struct behavior_tb_cpi_data behavior_tb_cpi_data_##n;                                   \
    static const struct behavior_tb_cpi_config behavior_tb_cpi_config_##n = {                      \
        .sensor = TB_CPI_SENSOR(n),                                                                \
        .cpi_min = DT_INST_PROP(n, cpi_min),                                                       \
        .cpi_max = DT_INST_PROP(n, cpi_max),                                                       \
        .cpi_step = DT_INST_PROP(n, cpi_step),                                                     \
        .cpi_default = DT_INST_PROP(n, cpi_default),                                               \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tb_cpi_init, NULL, &behavior_tb_cpi_data_##n,              \
                            &behavior_tb_cpi_config_##n, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_tb_cpi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TB_CPI_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
