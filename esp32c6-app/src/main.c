#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zephyr_app, LOG_LEVEL_INF);

int main(void)
{
	printk("\nHello World! %s\n", CONFIG_BOARD);

	LOG_INF("USB Serial/JTAG konsol klar");
	LOG_WRN("Dette er en test-advarsel (LOG_WRN)");
	LOG_ERR("Dette er en test-fejl (LOG_ERR)");

	while (1) {
		LOG_INF("Kører - uptime: %llu ms", k_uptime_get());
		k_sleep(K_MSEC(5000));
	}

	return 0;
}
