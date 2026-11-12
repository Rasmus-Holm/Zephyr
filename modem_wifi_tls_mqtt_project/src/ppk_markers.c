/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ppk_markers.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ppk_markers);

#define MARK_MODBUS  CONFIG_APP_PPK_MARKER_MODBUS_PIN
#define MARK_PUBLISH CONFIG_APP_PPK_MARKER_PUBLISH_PIN
#define ERROR_PULSE_MS 1000

static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

int ppk_markers_init(void)
{
	int ret;

	if (!device_is_ready(gpio0))
	{
		LOG_ERR("gpio0 not ready; PPK markers disabled");
		return -ENODEV;
	}

	ret = gpio_pin_configure(gpio0, MARK_MODBUS, GPIO_OUTPUT_INACTIVE);
	if (ret == 0)
	{
		ret = gpio_pin_configure(gpio0, MARK_PUBLISH, GPIO_OUTPUT_INACTIVE);
	}
	if (ret < 0)
	{
		LOG_ERR("PPK marker GPIO setup failed: %d", ret);
		return ret;
	}

	LOG_INF("PPK markers: Modbus on GPIO%d (D0), publish on GPIO%d (D1)", MARK_MODBUS,
		MARK_PUBLISH);
	return 0;
}

void ppk_mark_modbus(bool active)
{
	gpio_pin_set(gpio0, MARK_MODBUS, active ? 1 : 0);
}

void ppk_mark_publish(bool active)
{
	gpio_pin_set(gpio0, MARK_PUBLISH, active ? 1 : 0);
}

void ppk_signal_error(void)
{
	gpio_pin_set(gpio0, MARK_PUBLISH, 1);
	k_msleep(ERROR_PULSE_MS);
	gpio_pin_set(gpio0, MARK_PUBLISH, 0);
}
