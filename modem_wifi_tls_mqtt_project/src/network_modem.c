/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network.h"

#include <zephyr/device.h>
#include <zephyr/drivers/cellular.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/pm/device.h>
#include <zephyr/posix/arpa/inet.h>

#include <errno.h>

LOG_MODULE_REGISTER(network_modem, CONFIG_NET_CONNECTION_MANAGER_LOG_LEVEL);

/* Devicetree is read before Kconfig, so the modem overlay must be passed at build time. */
#if !DT_NODE_EXISTS(DT_ALIAS(modem))
#error "Cellular build needs the modem alias; pass -DEXTRA_DTC_OVERLAY_FILE with modem-uart1.overlay"
#endif

#define MQTT_BROKER_ADDRESS CONFIG_MQTT_BROKER_ADDRESS
#define MQTT_BROKER_PORT 8883

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define L4_CONNECTED_FLAG 1
#define NETWORK_POLL_SECONDS 5
#define MODEM_INFO_BUFFER_SIZE 64
/* Onomondo ICCIDs start with this prefix; only the prefix is logged. */
#define ICCID_PREFIX_LENGTH 6

static const struct device *const modem = DEVICE_DT_GET(DT_ALIAS(modem));
static const struct device *const modem_uart = DEVICE_DT_GET(DT_PARENT(DT_ALIAS(modem)));
/* 3GPP 27.010 basic-mode CLD (close multiplexer) on DLCI 0. */
static const uint8_t cmux_close_frame[] = {0xF9, 0x03, 0xEF, 0x05, 0xC3, 0x01, 0xF2, 0xF9};
#define CMUX_CLOSE_SETTLE_MS 500
/* Fixed wait for the modem to restart; the driver's init script sends its first AT 11.5 s after boot. */
#define MODEM_RESTART_WAIT_MS 4000
static struct net_if *ppp_iface;
K_EVENT_DEFINE(l4_event);

/* Track PPP link state so the IP wait and the status check see the same flag. */
static void l4_event_handler(uint64_t event, struct net_if *iface, void *info,
							 size_t info_length, void *user_data)
{
	if (iface != ppp_iface)
	{
		return;
	}

	if (event == NET_EVENT_L4_CONNECTED)
	{
		k_event_post(&l4_event, L4_CONNECTED_FLAG);
	}
	else if (event == NET_EVENT_L4_DISCONNECTED)
	{
		k_event_set(&l4_event, 0);
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(l4_events, L4_EVENT_MASK, l4_event_handler, NULL);

/* Name the registration status values so a rejected SIM is easy to recognise. */
static const char *registration_text(enum cellular_registration_status status)
{
	switch (status)
	{
	case CELLULAR_REGISTRATION_NOT_REGISTERED:
		return "not registered, not searching";
	case CELLULAR_REGISTRATION_REGISTERED_HOME:
		return "registered, home network";
	case CELLULAR_REGISTRATION_SEARCHING:
		return "searching for network";
	case CELLULAR_REGISTRATION_DENIED:
		return "DENIED by network";
	case CELLULAR_REGISTRATION_REGISTERED_ROAMING:
		return "registered, roaming";
	default:
		return "unknown or other";
	}
}

/* Log LTE registration status while waiting, so progress or rejection is visible. */
static void log_registration_progress(void)
{
	enum cellular_registration_status registration = CELLULAR_REGISTRATION_UNKNOWN;

	if (cellular_get_registration_status(modem, CELLULAR_ACCESS_TECHNOLOGY_E_UTRAN,
										 &registration) < 0)
	{
		LOG_WRN("Registration status: unavailable");
		return;
	}

	LOG_INF("Registration status: %d (%s)", registration, registration_text(registration));
}

/* Log one text modem field, marking it unknown when the read fails. */
static void log_modem_info(const char *label, enum cellular_modem_info_type type, size_t length)
{
	char value[MODEM_INFO_BUFFER_SIZE] = "-";

	if (cellular_get_modem_info(modem, type, value, sizeof(value)) < 0)
	{
		LOG_WRN("%s: unknown", label);
		return;
	}

	LOG_INF("%s: %.*s", label, (int)length, value);
}

/* Log facts that show which modem, SIM and radio link are in use. Runs once after IP. */
static void log_modem_facts(void)
{
	int16_t rssi_dbm = 0;

	log_modem_info("Model", CELLULAR_MODEM_INFO_MODEL_ID, MODEM_INFO_BUFFER_SIZE);
	log_modem_info("Firmware", CELLULAR_MODEM_INFO_FW_VERSION, MODEM_INFO_BUFFER_SIZE);
	log_modem_info("ICCID prefix", CELLULAR_MODEM_INFO_SIM_ICCID, ICCID_PREFIX_LENGTH);

	if (cellular_get_signal(modem, CELLULAR_SIGNAL_RSSI, &rssi_dbm) == 0)
	{
		LOG_INF("RSSI: %d dBm", rssi_dbm);
	}
	else
	{
		LOG_WRN("RSSI: unknown");
	}
}

/*
 * Close a CMUX session the modem kept from the previous boot, so the driver's AT script is
 * answered. The driver opens the UART early, but its init script sends the first AT only
 * 11.5 s after boot (1.5 s power pulse and 10 s startup time for the SIM7080).
 */
static void modem_close_stale_cmux(void)
{
	unsigned char stale_byte;

	LOG_INF("Closing any CMUX session left by the previous boot");
	for (size_t i = 0; i < sizeof(cmux_close_frame); i++)
	{
		uart_poll_out(modem_uart, cmux_close_frame[i]);
	}

	/* Let the modem answer, then discard the answer so the driver does not read it. */
	k_msleep(CMUX_CLOSE_SETTLE_MS);
	while (uart_poll_in(modem_uart, &stale_byte) == 0)
	{
	}
}

/* Send one AT command on the modem UART in plain AT mode. */
static void modem_send_at(const char *command)
{
	for (size_t i = 0; command[i] != '\0'; i++)
	{
		uart_poll_out(modem_uart, command[i]);
	}
	uart_poll_out(modem_uart, '\r');
}

/*
 * Reset the module with AT+CFUN=1,1 and wait for it to restart. A modem that is still attached
 * from the previous boot refuses ATD*99***1# (NO CARRIER); a reset gives the same state as a
 * power cycle. The modem's reply was not readable here with uart_poll_in in most measured runs,
 * so the wait is fixed. The restart took 2.5 to 2.8 s in measurements.
 */
static void modem_reset_module(void)
{
	LOG_INF("Resetting the modem module");
	modem_send_at("AT+CFUN=1,1");
	k_msleep(MODEM_RESTART_WAIT_MS);
}

/* Power the modem and bring the PPP interface up. */
int network_start(void)
{
	int ret;

	modem_close_stale_cmux();
	modem_reset_module();

	ppp_iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	if (!ppp_iface)
	{
		LOG_ERR("No PPP interface found");
		return -ENODEV;
	}

	LOG_INF("Powering on modem");
	/* The driver resumes itself at boot, so -EALREADY means it is already running. */
	ret = pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);
	if (ret < 0 && ret != -EALREADY)
	{
		LOG_ERR("Modem resume failed: %d", ret);
		return ret;
	}

	ret = net_if_up(ppp_iface);
	if (ret < 0)
	{
		LOG_ERR("PPP interface up failed: %d", ret);
	}

	return ret;
}

/* True when the PPP link has an IPv4 address. */
bool network_has_ipv4(void)
{
	return k_event_test(&l4_event, L4_CONNECTED_FLAG) != 0;
}

/* Wait for PPP to assign an address, logging LTE registration while waiting. */
int network_wait_ipv4(int timeout_seconds)
{
	for (int waited_seconds = 0; waited_seconds < timeout_seconds;
		 waited_seconds += NETWORK_POLL_SECONDS)
	{
		uint32_t events = k_event_wait(&l4_event, L4_CONNECTED_FLAG, false,
									   K_SECONDS(NETWORK_POLL_SECONDS));

		if (events == L4_CONNECTED_FLAG)
		{
			LOG_INF("PPP link up, IP address assigned");
			log_modem_facts();
			return 0;
		}

		log_registration_progress();
	}

	LOG_ERR("No IP address within %d s", timeout_seconds);
	return -ETIMEDOUT;
}

void network_log_status(void)
{
	LOG_INF("PPP status: %s", network_has_ipv4() ? "IP assigned" : "no IP");
}

/* Open a plain TCP connection to the broker to prove the link before TLS. */
int network_precheck(void)
{
	struct sockaddr_in endpoint = {0};
	int sock;
	int ret;
	int connect_errno;

	endpoint.sin_family = AF_INET;
	endpoint.sin_port = htons(MQTT_BROKER_PORT);
	if (inet_pton(AF_INET, MQTT_BROKER_ADDRESS, &endpoint.sin_addr) != 1)
	{
		LOG_ERR("Invalid TCP endpoint address: %s", MQTT_BROKER_ADDRESS);
		return -EINVAL;
	}

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
	{
		LOG_ERR("TCP socket open failed: %d", errno);
		return -errno;
	}

	/* Blocking connect; the TCP stack sets the timeout. */
	ret = zsock_connect(sock, (struct sockaddr *)&endpoint, sizeof(endpoint));
	/* Keep errno before close, because close may overwrite it. */
	connect_errno = errno;
	zsock_close(sock);
	if (ret < 0)
	{
		LOG_ERR("TCP connect to %s:%d failed: %d", MQTT_BROKER_ADDRESS, MQTT_BROKER_PORT,
				connect_errno);
		return -connect_errno;
	}

	LOG_INF("TCP connection to %s:%d succeeded", MQTT_BROKER_ADDRESS, MQTT_BROKER_PORT);
	return 0;
}
