/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modbus_reader.h"
#include "mqtt_transport.h"
#include "network.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys/clock.h>

#include <errno.h>
#include <time.h>

LOG_MODULE_REGISTER(MAIN);

#define MQTT_SNTP_SERVER CONFIG_MQTT_SNTP_SERVER
#define MODBUS_EXPECTED_TEST_VALUE 1234
#define MODBUS_PUBLISH_INTERVAL_MS 5000
#define MODBUS_PAYLOAD_BUFFER_SIZE 64

/* False after a failed pre-TLS network check, so no TLS or MQTT is attempted on that link. */
static bool mqtt_allowed = true;
/* True once SNTP has set the clock; certificate dates are checked against it. */
static bool clock_synchronized = false;

/* Log the IPv4 DNS servers the resolver holds, so a missing or wrong server is visible. */
static void log_dns_servers(void)
{
	struct dns_resolve_context *dns = dns_resolve_get_default();
	char text[NET_IPV4_ADDR_LEN];
	int found = 0;

	for (int i = 0; i < DNS_RESOLVER_MAX_POLL; i++)
	{
		const struct net_sockaddr *server = &dns->servers[i].dns_server;

		if (server->sa_family != NET_AF_INET)
		{
			continue;
		}

		net_addr_ntop(NET_AF_INET, &net_sin(server)->sin_addr, text, sizeof(text));
		LOG_INF("DNS server %d: %s", i, text);
		found++;
	}

	if (found == 0)
	{
		LOG_WRN("No IPv4 DNS server configured");
	}
}

static int synchronize_clock(void)
{
	struct sntp_time sntp_time;
	struct timespec realtime;
	int ret;

	int64_t started_ms = k_uptime_get();

	ret = sntp_simple(MQTT_SNTP_SERVER, 5000, &sntp_time);
	LOG_INF("SNTP name lookup and query took %d ms", (int)(k_uptime_get() - started_ms));
	if (ret < 0)
	{
		LOG_ERR("SNTP query failed: %d", ret);
		log_dns_servers();
		return ret;
	}

	realtime.tv_sec = (time_t)sntp_time.seconds;
	realtime.tv_nsec = ((uint64_t)sntp_time.fraction * NSEC_PER_SEC) >> 32;
	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &realtime);
	if (ret < 0)
	{
		LOG_ERR("Setting realtime clock failed: %d", ret);
		return ret;
	}

	LOG_INF("Realtime clock synchronized before TLS");
	clock_synchronized = true;
	return 0;
}

/* Read and validate the RTU values before publishing the slave's uptime. */
static int publish_modbus_uptime(void)
{
	char payload[MODBUS_PAYLOAD_BUFFER_SIZE];
	uint16_t test_value;
	uint16_t uptime_seconds;
	int ret;

	ret = modbus_reader_read(&test_value, &uptime_seconds);
	if (ret < 0)
	{
		LOG_ERR("Modbus RTU read failed: %d", ret);
		return ret;
	}

	if (test_value != MODBUS_EXPECTED_TEST_VALUE)
	{
		LOG_ERR("Modbus validation failed: register 0 = %u", test_value);
		return -EINVAL;
	}

	LOG_INF("Modbus sample: register 0 = %u, uptime = %u s", test_value, uptime_seconds);
	ret = snprintk(payload, sizeof(payload), "payload %u", uptime_seconds);
	if (ret < 0 || ret >= sizeof(payload))
	{
		LOG_ERR("Modbus payload formatting failed: %d", ret);
		return -EMSGSIZE;
	}

	return mqtt_transport_publish(payload);
}

int main(void)
{
	int64_t next_modbus_publish_ms = 0;
	bool modbus_ready = false;
	int ret;

	k_sleep(K_SECONDS(5));

	ret = modbus_reader_init();
	if (ret < 0)
	{
		LOG_ERR("Modbus client initialization failed: %d; telemetry disabled", ret);
	}
	else
	{
		modbus_ready = true;
		LOG_INF("Modbus RTU client ready; sample interval %d ms", MODBUS_PUBLISH_INTERVAL_MS);
	}

	if (mqtt_transport_tls_init() < 0)
	{
		LOG_ERR("TLS initialization failed");
		return 0;
	}

	if (network_start() < 0)
	{
		LOG_ERR("Lower network layer failed to start");
	}
	else if (network_wait_ipv4(CONFIG_NETWORK_IPV4_TIMEOUT_SECONDS) == 0)
	{
		LOG_INF("ipv4 recieved");
		if (network_precheck() < 0)
		{
			LOG_ERR("Stopping before TLS: plain TCP to broker failed");
			mqtt_allowed = false;
		}
		else if (synchronize_clock() == 0 && mqtt_transport_establish() == 0)
		{
			next_modbus_publish_ms = k_uptime_get();
		}
	}

	while (true)
	{
		network_log_status();

		if (mqtt_transport_is_connected())
		{
			mqtt_transport_process(1000);
		}
		else
		{
			k_sleep(K_SECONDS(5));
			/* Retry the clock first: a wrong clock makes TLS reject the certificate. */
			if (mqtt_allowed && network_has_ipv4() &&
			    (clock_synchronized || synchronize_clock() == 0))
			{
				if (mqtt_transport_establish() == 0)
				{
					next_modbus_publish_ms = k_uptime_get();
				}
			}
		}

		if (mqtt_transport_is_connected() && modbus_ready &&
		    k_uptime_get() >= next_modbus_publish_ms)
		{
			publish_modbus_uptime();
			next_modbus_publish_ms = k_uptime_get() + MODBUS_PUBLISH_INTERVAL_MS;
		}
	}
}
