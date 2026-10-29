#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>

#include "modbus_server.h"
#include "sensors.h"

LOG_MODULE_REGISTER(modbus_server, LOG_LEVEL_DBG);

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)
#define SLAVE_UNIT_ID 1

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	const struct sensor_data *sd = sensors_get();

	LOG_INF("HOLDING_RD called: addr=%u", addr);

	switch (addr) {
	case 0:
		/* Temperature in °C * 10 */
		*reg = (uint16_t)sd->temp_c_x10;
		break;
	case 1:
		/* Uptime LSW in seconds */
		*reg = sd->uptime_s_lsw;
		break;
	default:
		return -ENOTSUP;
	}

	LOG_INF("Holding read addr %u = %u", addr, *reg);
	return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	LOG_WRN("Holding write to addr %u ignored (value %u)", addr, reg);
	return -ENOTSUP;
}

static struct modbus_user_callbacks mbs_cbs = {
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

static struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RTU,
	.server = {
		.user_cb = &mbs_cbs,
		.unit_id = SLAVE_UNIT_ID,
	},
	.serial = {
		.baud = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
	},
};

int modbus_server_init(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_NODE)};

	int server_iface = modbus_iface_get_by_name(iface_name);
	if (server_iface < 0) {
		LOG_ERR("No Modbus interface named %s: %d", iface_name, server_iface);
		return server_iface;
	}

	LOG_INF("Starting Modbus RTU server on %s, unit ID %d @ 9600 8N1",
		iface_name, SLAVE_UNIT_ID);

	return modbus_init_server(server_iface, server_param);
}
