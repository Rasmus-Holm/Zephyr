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
#include <stdarg.h>
#include <time.h>

LOG_MODULE_REGISTER(MAIN);

#define MQTT_SNTP_SERVER CONFIG_MQTT_SNTP_SERVER
#define MODBUS_PUBLISH_INTERVAL_MILLISECONDS 5000
#define MODBUS_PAYLOAD_BUFFER_SIZE 1536
#define BATCH_SIZE CONFIG_APP_BATCH_SIZE

#define SPB_GROUP_ID "UCL"
#define SPB_EDGE_NODE_ID CONFIG_MQTT_CLIENT_ID
#define SPB_TOPIC_NBIRTH "spBv1.0/" SPB_GROUP_ID "/NBIRTH/" SPB_EDGE_NODE_ID
#define SPB_TOPIC_NDATA "spBv1.0/" SPB_GROUP_ID "/NDATA/" SPB_EDGE_NODE_ID
#define SPB_TOPIC_NDEATH "spBv1.0/" SPB_GROUP_ID "/NDEATH/" SPB_EDGE_NODE_ID

struct modbus_sample {
	uint64_t timestamp_ms;
	uint16_t adc_millivolts;
	uint16_t uptime_seconds;
};

static struct modbus_sample sample_batch[BATCH_SIZE];
static uint8_t batch_count;

/* False after a failed pre-TLS network check, so no TLS or MQTT is attempted on that link. */
static bool mqtt_allowed = true;
/* True once SNTP has set the clock; certificate dates are checked against it. */
static bool clock_synchronized = false;

/* Sparkplug B state counters */
static int64_t spb_bdseq = -1;
static uint8_t spb_seq = 0;

static uint64_t get_posix_timestamp_ms(void)
{
	struct timespec clock_spec;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &clock_spec) == 0)
	{
		return ((uint64_t)clock_spec.tv_sec * 1000ULL) + ((uint64_t)clock_spec.tv_nsec / 1000000ULL);
	}

	return (uint64_t)k_uptime_get();
}

/* Log the IPv4 DNS servers the resolver holds, so a missing or wrong server is visible. */
static void log_dns_servers(void)
{
	struct dns_resolve_context *dns = dns_resolve_get_default();
	char address_text[NET_IPV4_ADDR_LEN];
	int server_count = 0;

	for (int i = 0; i < DNS_RESOLVER_MAX_POLL; i++)
	{
		const struct net_sockaddr *server = &dns->servers[i].dns_server;

		if (server->sa_family != NET_AF_INET)
		{
			continue;
		}

		net_addr_ntop(NET_AF_INET, &net_sin(server)->sin_addr, address_text, sizeof(address_text));
		LOG_INF("DNS server %d: %s", i, address_text);
		server_count++;
	}

	if (server_count == 0)
	{
		LOG_WRN("No IPv4 DNS server configured");
	}
}

static int synchronize_clock(void)
{
	struct sntp_time sntp_time;
	struct timespec realtime_spec;
	int return_code;

	int64_t started_ms = k_uptime_get();

	return_code = sntp_simple(MQTT_SNTP_SERVER, 5000, &sntp_time);
	LOG_INF("SNTP name lookup and query took %d ms", (int)(k_uptime_get() - started_ms));
	if (return_code < 0)
	{
		LOG_ERR("SNTP query failed: %d", return_code);
		log_dns_servers();
		return return_code;
	}

	realtime_spec.tv_sec = (time_t)sntp_time.seconds;
	realtime_spec.tv_nsec = ((uint64_t)sntp_time.fraction * NSEC_PER_SEC) >> 32;
	return_code = sys_clock_settime(SYS_CLOCK_REALTIME, &realtime_spec);
	if (return_code < 0)
	{
		LOG_ERR("Setting realtime clock failed: %d", return_code);
		return return_code;
	}

	LOG_INF("Realtime clock synchronized before TLS");
	clock_synchronized = true;
	return 0;
}

static int publish_spb_birth(void)
{
	char payload_json[MODBUS_PAYLOAD_BUFFER_SIZE];
	uint64_t now_ms = get_posix_timestamp_ms();
	int return_code;

	/* Reset seq to 0 on NBIRTH as per Sparkplug B standard */
	spb_seq = 0;

	return_code = snprintk(payload_json, sizeof(payload_json),
		       "{\"timestamp\":%llu,\"device_id\":\"%s\",\"seq\":0,\"bdSeq\":%llu,\"metrics\":["
		       "{\"name\":\"bdSeq\",\"value\":%llu},"
		       "{\"name\":\"node_state\",\"value\":\"ONLINE\"}]}",
		       (unsigned long long)now_ms,
		       SPB_EDGE_NODE_ID,
		       (unsigned long long)spb_bdseq,
		       (unsigned long long)spb_bdseq);
	if (return_code < 0 || return_code >= sizeof(payload_json))
	{
		LOG_ERR("Sparkplug NBIRTH payload formatting failed: %d", return_code);
		return -EMSGSIZE;
	}

	LOG_INF("Publishing Sparkplug B NBIRTH (bdSeq=%llu)", (unsigned long long)spb_bdseq);
	return_code = mqtt_transport_publish_topic(SPB_TOPIC_NBIRTH, payload_json, MQTT_QOS_0_AT_MOST_ONCE);
	if (return_code == 0)
	{
		/* First telemetry NDATA starts at seq 1 */
		spb_seq = 1;
	}

	return return_code;
}

/* Append a formatted fragment to the JSON buffer; returns 0 or negative on error/truncation. */
static int payload_append(char *buffer, size_t buffer_size, int *offset, const char *fmt, ...)
{
	va_list args;
	int written;

	va_start(args, fmt);
	written = vsnprintk(buffer + *offset, buffer_size - *offset, fmt, args);
	va_end(args);

	if (written < 0 || (size_t)written >= (buffer_size - *offset))
	{
		return -EMSGSIZE;
	}

	*offset += written;
	return 0;
}

/* Publish the collected batch as one Sparkplug B NDATA JSON message (format A: parallel arrays). */
static int publish_batch(void)
{
	char payload_json[MODBUS_PAYLOAD_BUFFER_SIZE];
	int offset = 0;
	int return_code;

	if (batch_count < BATCH_SIZE)
	{
		return 0;
	}

	return_code = payload_append(payload_json, sizeof(payload_json), &offset,
				     "{\"timestamp\":%llu,\"device_id\":\"%s\",\"seq\":%u,\"metrics\":["
				     "{\"name\":\"sample_timestamps\",\"value\":[",
				     (unsigned long long)sample_batch[batch_count - 1].timestamp_ms,
				     SPB_EDGE_NODE_ID,
				     spb_seq);
	if (return_code < 0)
	{
		goto truncated;
	}

	for (uint8_t i = 0; i < batch_count; i++)
	{
		return_code = payload_append(payload_json, sizeof(payload_json), &offset, "%s%llu",
					     (i == 0) ? "" : ",",
					     (unsigned long long)sample_batch[i].timestamp_ms);
		if (return_code < 0)
		{
			goto truncated;
		}
	}

	return_code = payload_append(payload_json, sizeof(payload_json), &offset,
				     "]},{\"name\":\"adc_millivolts\",\"value\":[");
	if (return_code < 0)
	{
		goto truncated;
	}

	for (uint8_t i = 0; i < batch_count; i++)
	{
		return_code = payload_append(payload_json, sizeof(payload_json), &offset, "%s%u",
					     (i == 0) ? "" : ",",
					     sample_batch[i].adc_millivolts);
		if (return_code < 0)
		{
			goto truncated;
		}
	}

	return_code = payload_append(payload_json, sizeof(payload_json), &offset,
				     "]},{\"name\":\"uptime_seconds\",\"value\":[");
	if (return_code < 0)
	{
		goto truncated;
	}

	for (uint8_t i = 0; i < batch_count; i++)
	{
		return_code = payload_append(payload_json, sizeof(payload_json), &offset, "%s%u",
					     (i == 0) ? "" : ",",
					     sample_batch[i].uptime_seconds);
		if (return_code < 0)
		{
			goto truncated;
		}
	}

	return_code = payload_append(payload_json, sizeof(payload_json), &offset, "]}]}");
	if (return_code < 0)
	{
		goto truncated;
	}

	return_code = mqtt_transport_publish_topic(SPB_TOPIC_NDATA, payload_json, MQTT_QOS_0_AT_MOST_ONCE);
	if (return_code < 0)
	{
		LOG_ERR("MQTT batch publish failed: %d; discarding batch for newer data", return_code);
		batch_count = 0;
		return return_code;
	}

	spb_seq = (spb_seq + 1) & 0xFF;
	batch_count = 0;
	LOG_INF("Published batched NDATA with %u samples", BATCH_SIZE);
	return 0;

truncated:
	LOG_ERR("Sparkplug NDATA payload formatting failed/truncated");
	/* Discard to prioritize newer data over an unsendable old batch. */
	batch_count = 0;
	return -EMSGSIZE;
}

/* Read one Modbus sample, collect into a batch, and publish when BATCH_SIZE samples are ready.
 * Newer data is prioritized: on publish failure or when a full unsent batch would block a new
 * sample, the old batch is discarded.
 */
static int sample_and_publish_modbus(void)
{
	uint16_t adc_millivolts;
	uint16_t uptime_seconds;
	int return_code;

	return_code = modbus_reader_read(&adc_millivolts, &uptime_seconds);
	if (return_code < 0)
	{
		LOG_ERR("Modbus RTU read failed: %d", return_code);
		return return_code;
	}

	if (batch_count >= BATCH_SIZE)
	{
		LOG_WRN("Discarding unsent batch to prioritize newer sample");
		batch_count = 0;
	}

	sample_batch[batch_count].timestamp_ms = get_posix_timestamp_ms();
	sample_batch[batch_count].adc_millivolts = adc_millivolts;
	sample_batch[batch_count].uptime_seconds = uptime_seconds;
	batch_count++;

	LOG_INF("Modbus sample: adc = %u mV, uptime = %u s (%u/%u)", adc_millivolts, uptime_seconds,
		batch_count, BATCH_SIZE);

	if (batch_count == BATCH_SIZE)
	{
		return publish_batch();
	}

	return 0;
}

static int establish_mqtt_connection(void)
{
	char death_payload[128];
	uint64_t now_ms;
	int return_code;

	/* Increment bdSeq on each new session */
	spb_bdseq++;
	now_ms = get_posix_timestamp_ms();

	return_code = snprintk(death_payload, sizeof(death_payload),
		       "{\"timestamp\":%llu,\"device_id\":\"%s\",\"bdSeq\":%llu}",
		       (unsigned long long)now_ms,
		       SPB_EDGE_NODE_ID,
		       (unsigned long long)spb_bdseq);
	if (return_code < 0 || return_code >= sizeof(death_payload))
	{
		LOG_ERR("NDEATH payload formatting failed: %d", return_code);
		return -EMSGSIZE;
	}

	/* Establish TLS & MQTT with Last Will (NDEATH, QoS 1) */
	return_code = mqtt_transport_establish_will(SPB_TOPIC_NDEATH, death_payload, MQTT_QOS_1_AT_LEAST_ONCE);
	if (return_code < 0)
	{
		return return_code;
	}

	/* Immediately send NBIRTH after connection establishment */
	return_code = publish_spb_birth();
	if (return_code < 0)
	{
		LOG_WRN("Failed to publish NBIRTH: %d", return_code);
	}

	return 0;
}

int main(void)
{
	int64_t next_modbus_publish_ms = 0;
	bool modbus_ready = false;
	int return_code;

	k_sleep(K_SECONDS(5));

	return_code = modbus_reader_init();
	if (return_code < 0)
	{
		LOG_ERR("Modbus client initialization failed: %d; telemetry disabled", return_code);
	}
	else
	{
		modbus_ready = true;
		LOG_INF("Modbus RTU client ready; sample interval %d ms", MODBUS_PUBLISH_INTERVAL_MILLISECONDS);
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
			sample_and_publish_modbus();
			next_modbus_publish_ms = k_uptime_get() + MODBUS_PUBLISH_INTERVAL_MILLISECONDS;
		}
	}
}
