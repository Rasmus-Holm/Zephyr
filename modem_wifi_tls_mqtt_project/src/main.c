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
#define MODBUS_PAYLOAD_BUFFER_SIZE 256

#define SPB_GROUP_ID "UCL"
#define SPB_EDGE_NODE_ID CONFIG_MQTT_CLIENT_ID
#define SPB_TOPIC_NBIRTH "spBv1.0/" SPB_GROUP_ID "/NBIRTH/" SPB_EDGE_NODE_ID
#define SPB_TOPIC_NDATA "spBv1.0/" SPB_GROUP_ID "/NDATA/" SPB_EDGE_NODE_ID
#define SPB_TOPIC_NDEATH "spBv1.0/" SPB_GROUP_ID "/NDEATH/" SPB_EDGE_NODE_ID

/* False after a failed pre-TLS network check, so no TLS or MQTT is attempted on that link. */
static bool mqtt_allowed = true;
/* True once SNTP has set the clock; certificate dates are checked against it. */
static bool clock_synchronized = false;

/* Sparkplug B state counters */
static int64_t spb_bdseq = -1;
static uint8_t spb_seq = 0;

static uint64_t get_posix_timestamp_ms(void)
{
	struct timespec ts;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) == 0)
	{
		return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
	}

	return (uint64_t)k_uptime_get();
}

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

static int publish_spb_birth(void)
{
	char payload[MODBUS_PAYLOAD_BUFFER_SIZE];
	uint64_t now_ms = get_posix_timestamp_ms();
	int ret;

	/* Reset seq to 0 on NBIRTH as per Sparkplug B standard */
	spb_seq = 0;

	ret = snprintk(payload, sizeof(payload),
		       "{\"timestamp\":%llu,\"device_id\":\"%s\",\"seq\":0,\"bdSeq\":%llu,\"metrics\":["
		       "{\"name\":\"bdSeq\",\"value\":%llu},"
		       "{\"name\":\"node_state\",\"value\":\"ONLINE\"}]}",
		       (unsigned long long)now_ms,
		       SPB_EDGE_NODE_ID,
		       (unsigned long long)spb_bdseq,
		       (unsigned long long)spb_bdseq);
	if (ret < 0 || ret >= sizeof(payload))
	{
		LOG_ERR("Sparkplug NBIRTH payload formatting failed: %d", ret);
		return -EMSGSIZE;
	}

	LOG_INF("Publishing Sparkplug B NBIRTH (bdSeq=%llu)", (unsigned long long)spb_bdseq);
	ret = mqtt_transport_publish_topic(SPB_TOPIC_NBIRTH, payload, MQTT_QOS_0_AT_MOST_ONCE);
	if (ret == 0)
	{
		/* First telemetry NDATA starts at seq 1 */
		spb_seq = 1;
	}

	return ret;
}

/* Read and validate the RTU values before publishing telemetry as Sparkplug B NDATA JSON. */
static int publish_modbus_uptime(void)
{
	char payload[MODBUS_PAYLOAD_BUFFER_SIZE];
	uint16_t test_value;
	uint16_t uptime_seconds;
	uint64_t now_ms;
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

	now_ms = get_posix_timestamp_ms();

	/* Format NDATA payload with device_id, Unix ms timestamp, seq and metrics */
	ret = snprintk(payload, sizeof(payload),
		       "{\"timestamp\":%llu,\"device_id\":\"%s\",\"seq\":%u,\"metrics\":["
		       "{\"name\":\"modbus_reg0\",\"value\":%u},"
		       "{\"name\":\"modbus_reg1\",\"value\":%u}]}",
		       (unsigned long long)now_ms,
		       SPB_EDGE_NODE_ID,
		       spb_seq,
		       test_value,
		       uptime_seconds);
	if (ret < 0 || ret >= sizeof(payload))
	{
		LOG_ERR("Sparkplug NDATA payload formatting failed: %d", ret);
		return -EMSGSIZE;
	}

	/* Sequence counter increments 0..255 and wraps around */
	spb_seq = (spb_seq + 1) & 0xFF;

	return mqtt_transport_publish_topic(SPB_TOPIC_NDATA, payload, MQTT_QOS_0_AT_MOST_ONCE);
}

static int establish_mqtt_connection(void)
{
	char death_payload[128];
	uint64_t now_ms;
	int ret;

	/* Increment bdSeq on each new session */
	spb_bdseq++;
	now_ms = get_posix_timestamp_ms();

	ret = snprintk(death_payload, sizeof(death_payload),
		       "{\"timestamp\":%llu,\"device_id\":\"%s\",\"bdSeq\":%llu}",
		       (unsigned long long)now_ms,
		       SPB_EDGE_NODE_ID,
		       (unsigned long long)spb_bdseq);
	if (ret < 0 || ret >= sizeof(death_payload))
	{
		LOG_ERR("NDEATH payload formatting failed: %d", ret);
		return -EMSGSIZE;
	}

	/* Establish TLS & MQTT with Last Will (NDEATH, QoS 1) */
	ret = mqtt_transport_establish_will(SPB_TOPIC_NDEATH, death_payload, MQTT_QOS_1_AT_LEAST_ONCE);
	if (ret < 0)
	{
		return ret;
	}

	/* Immediately send NBIRTH after connection establishment */
	ret = publish_spb_birth();
	if (ret < 0)
	{
		LOG_WRN("Failed to publish NBIRTH: %d", ret);
	}

	return 0;
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
		else if (synchronize_clock() == 0 && establish_mqtt_connection() == 0)
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
				if (establish_mqtt_connection() == 0)
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
