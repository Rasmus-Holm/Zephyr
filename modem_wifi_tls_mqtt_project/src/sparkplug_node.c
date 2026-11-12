/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sparkplug_node.h"
#include "mqtt_transport.h"
#include "sparkplug_payload.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(sparkplug_node);

/* Metric names and aliases. Aliases are unique across the node and its devices.
 * Alias 1-3 are kept for Process/Temperature, Status/CellularRSSI and
 * Status/SupplyVoltage from the report, once those measurements exist.
 */
#define METRIC_BDSEQ           "bdSeq"
#define METRIC_REBIRTH         "Node Control/Rebirth"
#define METRIC_SAMPLE_INTERVAL "Config/SampleInterval_s"
#define ALIAS_SAMPLE_INTERVAL  4
#define METRIC_DEVICE_UPTIME   "Status/Uptime_s"
#define ALIAS_DEVICE_UPTIME    10

#define SAMPLE_INTERVAL_MIN_S 1
#define SAMPLE_INTERVAL_MAX_S 3600

/* NBIRTH is about 90 bytes; a DDATA sample is about 15 bytes. */
#define TX_BUFFER_SIZE 384
#define DEATH_BUFFER_SIZE 64

#define DEVICE_ID CONFIG_SPARKPLUG_DEVICE_ID

struct device_sample {
	uint64_t timestamp_ms;
	uint16_t uptime_seconds;
};

/* Static: a Payload holds 16 metrics and is too large for the stack. */
static Payload tx_payload;
static Payload rx_payload;
static uint8_t tx_buffer[TX_BUFFER_SIZE];
static uint8_t death_buffer[DEATH_BUFFER_SIZE];
static char death_topic[SPARKPLUG_TOPIC_MAX];
static char ncmd_topic[SPARKPLUG_TOPIC_MAX];
static char dcmd_topic_prefix[SPARKPLUG_TOPIC_MAX];
static char state_topic[SPARKPLUG_TOPIC_MAX];

/* Primary host application; empty CONFIG_SPARKPLUG_PRIMARY_HOST_ID means none. */
#define HAS_PRIMARY_HOST (sizeof(CONFIG_SPARKPLUG_PRIMARY_HOST_ID) > 1)
#define STATE_PAYLOAD_MAX 128

/* NBIRTH has been sent on this connection; nothing else may be published before it. */
static bool node_born;
/* Last STATE from the primary host; reset on every connection, as the retained STATE
 * arrives again right after SUBSCRIBE.
 */
static bool host_online;
static bool host_went_offline;
static uint64_t host_state_timestamp;

/* bdSeq ties NBIRTH to the NDEATH will of the same connection (0-255). */
static uint8_t bd_seq;
static uint8_t next_bd_seq;
/* seq counts every message after NBIRTH (0-255, wraps); NBIRTH resets it to 0. */
static uint8_t seq;

static uint16_t sample_interval_s = CONFIG_SPARKPLUG_SAMPLE_INTERVAL_S;
static bool rebirth_requested;
static bool sample_interval_changed;

static bool device_born;
static bool device_has_value;
static uint16_t device_last_uptime;
static struct device_sample batch[CONFIG_SPARKPLUG_BATCH_SIZE];
static size_t batch_count;

/* Encode tx_payload and publish it; with_seq adds seq and counts it on success. */
static int publish_payload(const char *type, const char *device, bool with_seq)
{
	char topic[SPARKPLUG_TOPIC_MAX];
	size_t length;
	int ret;

	if (with_seq)
	{
		sparkplug_payload_set_seq(&tx_payload, seq);
	}

	ret = sparkplug_topic(topic, sizeof(topic), type, device);
	if (ret < 0)
	{
		return ret;
	}

	ret = sparkplug_encode(&tx_payload, tx_buffer, sizeof(tx_buffer), &length);
	if (ret < 0)
	{
		return ret;
	}

	ret = mqtt_transport_publish(topic, tx_buffer, length);
	if (ret == 0 && with_seq)
	{
		seq++;
	}

	return ret;
}

static int publish_nbirth(void)
{
	uint64_t now = sparkplug_now_ms();
	Payload_Metric *metric;

	sparkplug_payload_init(&tx_payload, now);

	metric = sparkplug_add_metric(&tx_payload, METRIC_BDSEQ, 0, DataType_UInt64, now);
	sparkplug_metric_set_uint64(metric, bd_seq);

	metric = sparkplug_add_metric(&tx_payload, METRIC_REBIRTH, 0, DataType_Boolean, now);
	sparkplug_metric_set_bool(metric, false);

	metric = sparkplug_add_metric(&tx_payload, METRIC_SAMPLE_INTERVAL, ALIAS_SAMPLE_INTERVAL,
				      DataType_UInt16, now);
	sparkplug_metric_set_uint32(metric, sample_interval_s);

	seq = 0;
	LOG_INF("Publishing NBIRTH, bdSeq %u", bd_seq);
	return publish_payload(SPARKPLUG_NBIRTH, NULL, true);
}

/* DBIRTH carries the device's metrics with full names and their latest values. */
static int publish_dbirth(void)
{
	uint64_t now = sparkplug_now_ms();
	Payload_Metric *metric;
	int ret;

	sparkplug_payload_init(&tx_payload, now);
	metric = sparkplug_add_metric(&tx_payload, METRIC_DEVICE_UPTIME, ALIAS_DEVICE_UPTIME,
				      DataType_UInt16, now);
	sparkplug_metric_set_uint32(metric, device_last_uptime);

	LOG_INF("Publishing DBIRTH for %s", DEVICE_ID);
	ret = publish_payload(SPARKPLUG_DBIRTH, DEVICE_ID, true);
	if (ret == 0)
	{
		device_born = true;
		batch_count = 0;
	}

	return ret;
}

/* Send the collected samples as one DDATA with alias only and one timestamp per sample. */
static int publish_ddata(void)
{
	int ret;

	if (batch_count == 0)
	{
		return 0;
	}

	sparkplug_payload_init(&tx_payload, sparkplug_now_ms());
	for (size_t i = 0; i < batch_count; i++)
	{
		Payload_Metric *metric = sparkplug_add_metric(&tx_payload, NULL, ALIAS_DEVICE_UPTIME,
							      DataType_Unknown,
							      batch[i].timestamp_ms);

		if (metric == NULL)
		{
			break;
		}
		sparkplug_metric_set_uint32(metric, batch[i].uptime_seconds);
	}

	LOG_INF("Publishing DDATA for %s with %zu samples", DEVICE_ID, batch_count);
	ret = publish_payload(SPARKPLUG_DDATA, DEVICE_ID, true);
	/* Dropped on failure too: a lost connection ends in a new birth with fresh values. */
	batch_count = 0;

	return ret;
}

static int publish_ddeath(void)
{
	int ret;

	sparkplug_payload_init(&tx_payload, sparkplug_now_ms());
	LOG_INF("Publishing DDEATH for %s", DEVICE_ID);
	ret = publish_payload(SPARKPLUG_DDEATH, DEVICE_ID, true);
	device_born = false;
	batch_count = 0;

	return ret;
}

/* Report a changed node metric by exception, using its alias. */
static int publish_sample_interval(void)
{
	uint64_t now = sparkplug_now_ms();
	Payload_Metric *metric;

	sparkplug_payload_init(&tx_payload, now);
	metric = sparkplug_add_metric(&tx_payload, NULL, ALIAS_SAMPLE_INTERVAL, DataType_Unknown,
				      now);
	sparkplug_metric_set_uint32(metric, sample_interval_s);

	LOG_INF("Publishing NDATA: sample interval %u s", sample_interval_s);
	return publish_payload(SPARKPLUG_NDATA, NULL, true);
}

static int publish_births(void)
{
	int ret;

	device_born = false;
	batch_count = 0;

	ret = publish_nbirth();
	node_born = (ret == 0);
	if (ret == 0 && device_has_value)
	{
		ret = publish_dbirth();
	}

	return ret;
}

static void handle_ncmd(const Payload *payload)
{
	for (pb_size_t i = 0; i < payload->metrics_count; i++)
	{
		const Payload_Metric *metric = &payload->metrics[i];
		bool is_rebirth = metric->has_name && strcmp(metric->name, METRIC_REBIRTH) == 0;
		bool is_interval = (metric->has_name &&
				    strcmp(metric->name, METRIC_SAMPLE_INTERVAL) == 0) ||
				   (metric->has_alias && metric->alias == ALIAS_SAMPLE_INTERVAL);
		uint64_t value;

		if (is_rebirth)
		{
			if (metric->which_value == Payload_Metric_boolean_value_tag &&
			    metric->value.boolean_value)
			{
				LOG_INF("NCMD: rebirth requested");
				rebirth_requested = true;
			}
		}
		else if (is_interval)
		{
			if (sparkplug_metric_get_uint64(metric, &value) < 0 ||
			    value < SAMPLE_INTERVAL_MIN_S || value > SAMPLE_INTERVAL_MAX_S)
			{
				LOG_WRN("NCMD: invalid sample interval ignored");
				continue;
			}
			LOG_INF("NCMD: sample interval %u -> %u s", sample_interval_s,
				(unsigned int)value);
			sample_interval_s = (uint16_t)value;
			sample_interval_changed = true;
		}
		else
		{
			LOG_WRN("NCMD: unknown metric %s ignored",
				metric->has_name ? metric->name : "(alias only)");
		}
	}
}

/* Value after "key": in a flat JSON object, or NULL. Enough for {"online":true,"timestamp":N}. */
static const char *json_value(const char *json, const char *key)
{
	const char *p = strstr(json, key);

	if (p == NULL)
	{
		return NULL;
	}

	p += strlen(key);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
	{
		p++;
	}

	return p;
}

/* STATE from the primary host (JSON, retained). Older messages than the last one are ignored. */
static void handle_state(const uint8_t *data, size_t length)
{
	char json[STATE_PAYLOAD_MAX];
	const char *online;
	const char *timestamp;
	uint64_t state_timestamp = 0;

	if (length >= sizeof(json))
	{
		LOG_WRN("STATE: payload too long (%zu bytes)", length);
		return;
	}
	memcpy(json, data, length);
	json[length] = '\0';

	online = json_value(json, "\"online\"");
	if (online == NULL || (strncmp(online, "true", 4) != 0 && strncmp(online, "false", 5) != 0))
	{
		LOG_WRN("STATE: no valid \"online\" field");
		return;
	}

	timestamp = json_value(json, "\"timestamp\"");
	if (timestamp != NULL)
	{
		state_timestamp = strtoull(timestamp, NULL, 10);
	}
	if (state_timestamp < host_state_timestamp)
	{
		LOG_WRN("STATE: older than the last one, ignored");
		return;
	}
	host_state_timestamp = state_timestamp;

	if (online[0] == 't')
	{
		LOG_INF("STATE: host %s online", CONFIG_SPARKPLUG_PRIMARY_HOST_ID);
		host_online = true;
	}
	else
	{
		LOG_WRN("STATE: host %s offline", CONFIG_SPARKPLUG_PRIMARY_HOST_ID);
		if (host_online || node_born)
		{
			host_went_offline = true;
		}
		host_online = false;
	}
}

/* Runs inside mqtt_transport_process(): only decode and set flags, never publish here. */
static void on_message(const char *topic, const uint8_t *data, size_t length)
{
	if (HAS_PRIMARY_HOST && strcmp(topic, state_topic) == 0)
	{
		handle_state(data, length);
		return;
	}

	bool is_ncmd = strcmp(topic, ncmd_topic) == 0;
	bool is_dcmd = strncmp(topic, dcmd_topic_prefix, strlen(dcmd_topic_prefix)) == 0;

	if (!is_ncmd && !is_dcmd)
	{
		LOG_WRN("Message on unexpected topic %s", topic);
		return;
	}

	if (sparkplug_decode(data, length, &rx_payload) < 0)
	{
		return;
	}

	if (is_ncmd)
	{
		handle_ncmd(&rx_payload);
	}
	else
	{
		/* No writable device metrics yet; later this becomes a Modbus write. */
		LOG_WRN("DCMD on %s ignored: device has no writable metrics", topic);
	}
}

int sparkplug_node_init(void)
{
	int ret;

	ret = sparkplug_topic(death_topic, sizeof(death_topic), SPARKPLUG_NDEATH, NULL);
	if (ret == 0)
	{
		ret = sparkplug_topic(ncmd_topic, sizeof(ncmd_topic), SPARKPLUG_NCMD, NULL);
	}
	if (ret == 0)
	{
		/* "spBv1.0/<group>/DCMD/<node>/" matches DCMD for every device of this node. */
		ret = sparkplug_topic(dcmd_topic_prefix, sizeof(dcmd_topic_prefix), SPARKPLUG_DCMD,
				      "");
	}
	if (ret == 0 && HAS_PRIMARY_HOST)
	{
		int len = snprintf(state_topic, sizeof(state_topic), "spBv1.0/STATE/%s",
				   CONFIG_SPARKPLUG_PRIMARY_HOST_ID);

		if (len < 0 || (size_t)len >= sizeof(state_topic))
		{
			ret = -ENAMETOOLONG;
		}
	}
	if (ret < 0)
	{
		return ret;
	}

	mqtt_transport_set_rx_callback(on_message);
	LOG_INF("Sparkplug B edge node %s/%s, device %s", CONFIG_SPARKPLUG_GROUP_ID,
		CONFIG_SPARKPLUG_EDGE_NODE_ID, DEVICE_ID);
	return 0;
}

int sparkplug_node_connect(void)
{
	char dcmd_subscription[SPARKPLUG_TOPIC_MAX];
	Payload_Metric *metric;
	size_t length;
	int ret;

	/* New bdSeq for every connection; NBIRTH below repeats the value in the will. */
	bd_seq = next_bd_seq++;

	sparkplug_payload_init(&tx_payload, sparkplug_now_ms());
	metric = sparkplug_add_metric(&tx_payload, METRIC_BDSEQ, 0, DataType_UInt64, 0);
	sparkplug_metric_set_uint64(metric, bd_seq);
	ret = sparkplug_encode(&tx_payload, death_buffer, sizeof(death_buffer), &length);
	if (ret < 0)
	{
		return ret;
	}
	mqtt_transport_set_will(death_topic, death_buffer, length);

	ret = mqtt_transport_establish();
	if (ret < 0)
	{
		return ret;
	}

	node_born = false;
	host_online = false;
	host_went_offline = false;

	ret = sparkplug_topic(dcmd_subscription, sizeof(dcmd_subscription), SPARKPLUG_DCMD, "#");
	/* STATE first, so the retained host status arrives before anything else. */
	if (ret == 0 && HAS_PRIMARY_HOST)
	{
		ret = mqtt_transport_subscribe(state_topic);
	}
	if (ret == 0)
	{
		ret = mqtt_transport_subscribe(ncmd_topic);
	}
	if (ret == 0)
	{
		ret = mqtt_transport_subscribe(dcmd_subscription);
	}
	if (ret < 0)
	{
		return ret;
	}

	rebirth_requested = false;
	sample_interval_changed = false;

	if (HAS_PRIMARY_HOST)
	{
		/* NBIRTH follows in sparkplug_node_process() once the host reports online. */
		LOG_INF("Waiting for STATE online from %s", CONFIG_SPARKPLUG_PRIMARY_HOST_ID);
		return 0;
	}

	return publish_births();
}

/* The host went offline: announce NDEATH ourselves and disconnect (Sparkplug 3.0).
 * The main loop reconnects, and the next NBIRTH waits for the host to come back.
 */
static void go_offline_for_host(void)
{
	size_t length;

	LOG_WRN("Primary host offline: sending NDEATH and disconnecting");
	sparkplug_payload_init(&tx_payload, sparkplug_now_ms());
	Payload_Metric *metric = sparkplug_add_metric(&tx_payload, METRIC_BDSEQ, 0,
						      DataType_UInt64, 0);
	sparkplug_metric_set_uint64(metric, bd_seq);
	if (sparkplug_encode(&tx_payload, tx_buffer, sizeof(tx_buffer), &length) == 0)
	{
		mqtt_transport_publish(death_topic, tx_buffer, length);
	}

	node_born = false;
	device_born = false;
	batch_count = 0;
	mqtt_transport_close();
}

void sparkplug_node_process(void)
{
	if (!mqtt_transport_is_connected())
	{
		return;
	}

	if (host_went_offline)
	{
		host_went_offline = false;
		if (node_born)
		{
			go_offline_for_host();
			return;
		}
	}

	if (!node_born)
	{
		if (!HAS_PRIMARY_HOST || host_online)
		{
			rebirth_requested = false;
			publish_births();
		}
		return;
	}

	if (rebirth_requested)
	{
		rebirth_requested = false;
		/* bdSeq stays the same; the will registered at connect is still valid. */
		publish_births();
	}

	if (sample_interval_changed)
	{
		sample_interval_changed = false;
		publish_sample_interval();
	}
}

uint32_t sparkplug_node_sample_interval_ms(void)
{
	return (uint32_t)sample_interval_s * MSEC_PER_SEC;
}

void sparkplug_node_device_sample(uint16_t uptime_seconds)
{
	uint64_t now = sparkplug_now_ms();

	device_last_uptime = uptime_seconds;
	device_has_value = true;

	if (!mqtt_transport_is_connected() || !node_born)
	{
		return;
	}

	if (!device_born)
	{
		/* The birth carries this sample, so it is not batched again. */
		publish_dbirth();
		return;
	}

	batch[batch_count].timestamp_ms = now;
	batch[batch_count].uptime_seconds = uptime_seconds;
	batch_count++;

	if (batch_count >= CONFIG_SPARKPLUG_BATCH_SIZE)
	{
		publish_ddata();
	}
}

void sparkplug_node_device_offline(void)
{
	/* No DBIRTH with a stale value at the next rebirth or connection. */
	device_has_value = false;

	if (!mqtt_transport_is_connected() || !node_born || !device_born)
	{
		return;
	}

	/* Deliver what was read before the device went quiet, then announce its death. */
	publish_ddata();
	publish_ddeath();
}
