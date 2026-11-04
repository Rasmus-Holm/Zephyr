#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

#include "sensors.h"
#include "modbus_server.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("TEST: Modbus debug firmware er startet");
	LOG_INF("Modbus RTU slave starting (DS18B20 on GPIO4, UART2 16/17)");

	sensors_init();

	if (modbus_server_init() < 0) {
		LOG_ERR("Failed to init Modbus server");
		return 0;
	}

	while (1) {
		sensors_update();
		const struct sensor_data *sd = sensors_get();
		if (sd->valid) {
			int temp_abs = abs(sd->temp_c_x10);
			LOG_INF("Temp: %d.%d C (reg0=%d), uptime_lsw=%u",
				sd->temp_c_x10 / 10, temp_abs % 10,
				sd->temp_c_x10, sd->uptime_s_lsw);
		} else {
			LOG_WRN("Sensor not valid");
		}
		k_sleep(K_SECONDS(2));
	}

	return 0;
}
