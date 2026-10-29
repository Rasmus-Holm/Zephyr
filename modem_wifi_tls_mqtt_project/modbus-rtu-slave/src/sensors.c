#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "sensors.h"

LOG_MODULE_REGISTER(sensors, LOG_LEVEL_INF);

static const struct device *const ds18b20_dev = DEVICE_DT_GET(DT_NODELABEL(ds18b20));
static struct sensor_data data;

void sensors_init(void)
{
	if (!device_is_ready(ds18b20_dev)) {
		LOG_ERR("DS18B20 device not ready");
		data.valid = false;
		return;
	}
	LOG_INF("DS18B20 ready");
	data.valid = true;
	data.temp_c_x10 = 0;
	data.uptime_s_lsw = 0;
}

void sensors_update(void)
{
	struct sensor_value temp_val;

	data.uptime_s_lsw = (uint16_t)(k_uptime_get() / 1000);

	if (!data.valid || !device_is_ready(ds18b20_dev)) {
		return;
	}

	if (sensor_sample_fetch(ds18b20_dev) < 0) {
		LOG_ERR("Failed to fetch DS18B20 sample");
		return;
	}

	if (sensor_channel_get(ds18b20_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp_val) < 0) {
		LOG_ERR("Failed to get DS18B20 temperature");
		return;
	}

	/* Convert to °C * 10 */
	data.temp_c_x10 = (int16_t)(temp_val.val1 * 10 + temp_val.val2 / 100000);
	LOG_DBG("Temp: %d.%06d C -> %d (x10)", temp_val.val1, temp_val.val2, data.temp_c_x10);
}

const struct sensor_data *sensors_get(void)
{
	return &data;
}
