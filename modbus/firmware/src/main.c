/*
 * Modbus RTU server (slave) på ESP32-C6 via HW-519 TTL<->RS485.
 *
 * Registerkort (unit id 1):
 *   Coils           0     : "LED"/aktuator (læs/skriv)
 *   Input registers 0     : uptime i sekunder (low 16 bit)
 *                   1     : simuleret temperatur, 0.1 °C (fx 231 = 23.1 °C)
 *                   2     : antal håndterede Modbus-forespørgsler
 *   Holding regs    0..7  : frie parametre (fx setpunkt), læs/skriv
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mbs, LOG_LEVEL_INF);

#define UNIT_ID        1
#define NUM_COILS      1
#define NUM_INPUT_REGS 3
#define NUM_HOLD_REGS  8

static bool coils[NUM_COILS];
static uint16_t holding_reg[NUM_HOLD_REGS];
static atomic_t request_count;

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#define HAS_LED 1
#endif

static uint16_t simulated_temp(void)
{
	/* Savtak mellem 20.0 og 25.0 °C, så værdien ændrer sig synligt */
	return 200 + (k_uptime_get() / 1000) % 50;
}

static int coil_rd(uint16_t addr, bool *state)
{
	atomic_inc(&request_count);
	if (addr >= NUM_COILS) {
		return -ENOTSUP;
	}
	*state = coils[addr];
	return 0;
}

static int coil_wr(uint16_t addr, bool state)
{
	atomic_inc(&request_count);
	if (addr >= NUM_COILS) {
		return -ENOTSUP;
	}
	coils[addr] = state;
#ifdef HAS_LED
	gpio_pin_set_dt(&led, state);
#endif
	LOG_INF("Coil %u <- %s", addr, state ? "ON" : "OFF");
	return 0;
}

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	atomic_inc(&request_count);
	switch (addr) {
	case 0:
		*reg = (uint16_t)(k_uptime_get() / 1000);
		return 0;
	case 1:
		*reg = simulated_temp();
		return 0;
	case 2:
		*reg = (uint16_t)atomic_get(&request_count);
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	atomic_inc(&request_count);
	if (addr >= NUM_HOLD_REGS) {
		return -ENOTSUP;
	}
	*reg = holding_reg[addr];
	return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	atomic_inc(&request_count);
	if (addr >= NUM_HOLD_REGS) {
		return -ENOTSUP;
	}
	holding_reg[addr] = reg;
	LOG_INF("Holding reg %u <- %u", addr, reg);
	return 0;
}

static struct modbus_user_callbacks mbs_cbs = {
	.coil_rd = coil_rd,
	.coil_wr = coil_wr,
	.input_reg_rd = input_reg_rd,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

static const struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RTU,
	.server = {
		.user_cb = &mbs_cbs,
		.unit_id = UNIT_ID,
	},
	.serial = {
		.baud = 9600,
		.parity = UART_CFG_PARITY_NONE,
	},
};

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

int main(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_NODE)};
	int iface = modbus_iface_get_by_name(iface_name);
	int err;

#ifdef HAS_LED
	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
#endif

	if (iface < 0) {
		LOG_ERR("Modbus-interface '%s' ikke fundet (%d)", iface_name, iface);
		return 0;
	}

	err = modbus_init_server(iface, server_param);
	if (err) {
		LOG_ERR("modbus_init_server fejlede (%d)", err);
		return 0;
	}

	LOG_INF("Modbus RTU server klar: unit %d, 9600 8N1 på %s", UNIT_ID, iface_name);
	return 0;
}
