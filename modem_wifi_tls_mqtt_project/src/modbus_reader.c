#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>

#include "modbus_reader.h"

LOG_MODULE_REGISTER(modbus_reader, LOG_LEVEL_INF);

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)
#define SLAVE_ID 1
#define FIRST_REGISTER 0
#define REGISTER_COUNT 2
#define RESPONSE_TIMEOUT_US 500000

static int client_iface;

static const struct modbus_iface_param client_param = {
	.mode = MODBUS_MODE_RTU,
	.rx_timeout = RESPONSE_TIMEOUT_US,
	.serial = {
		.baud = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
	},
};

/* Find and initialize the enabled Modbus UART. */
int modbus_reader_init(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_NODE)};

	client_iface = modbus_iface_get_by_name(iface_name);
	if (client_iface < 0)
	{
		LOG_ERR("No Modbus interface named %s: %d", iface_name, client_iface);
		return client_iface;
	}

	return modbus_init_client(client_iface, client_param);
}

/* Read both holding registers; output parameters change only on success. */
int modbus_reader_read(uint16_t *test_value, uint16_t *uptime_seconds)
{
	uint16_t registers[REGISTER_COUNT];
	int err;

	if (test_value == NULL || uptime_seconds == NULL)
	{
		return -EINVAL;
	}

	err = modbus_read_holding_regs(client_iface, SLAVE_ID, FIRST_REGISTER, registers,
					       REGISTER_COUNT);
	if (err != 0)
	{
		return err;
	}

	*test_value = registers[0];
	*uptime_seconds = registers[1];
	return 0;
}
