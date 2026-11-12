/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sparkplug_payload.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

LOG_MODULE_REGISTER(sparkplug_payload);

#define SPARKPLUG_NAMESPACE "spBv1.0"

uint64_t sparkplug_now_ms(void)
{
	struct timespec now;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &now) < 0)
	{
		return 0;
	}

	return (uint64_t)now.tv_sec * MSEC_PER_SEC + now.tv_nsec / NSEC_PER_MSEC;
}

int sparkplug_topic(char *buf, size_t size, const char *type, const char *device)
{
	int ret;

	if (device != NULL)
	{
		ret = snprintf(buf, size, "%s/%s/%s/%s/%s", SPARKPLUG_NAMESPACE,
			       CONFIG_SPARKPLUG_GROUP_ID, type, CONFIG_SPARKPLUG_EDGE_NODE_ID, device);
	}
	else
	{
		ret = snprintf(buf, size, "%s/%s/%s/%s", SPARKPLUG_NAMESPACE,
			       CONFIG_SPARKPLUG_GROUP_ID, type, CONFIG_SPARKPLUG_EDGE_NODE_ID);
	}

	if (ret < 0 || (size_t)ret >= size)
	{
		LOG_ERR("Sparkplug topic too long for %s", type);
		return -ENAMETOOLONG;
	}

	return 0;
}

void sparkplug_payload_init(Payload *payload, uint64_t timestamp_ms)
{
	*payload = (Payload)Payload_init_zero;
	payload->has_timestamp = true;
	payload->timestamp = timestamp_ms;
}

void sparkplug_payload_set_seq(Payload *payload, uint8_t seq)
{
	payload->has_seq = true;
	payload->seq = seq;
}

Payload_Metric *sparkplug_add_metric(Payload *payload, const char *name, uint64_t alias,
				     DataType datatype, uint64_t timestamp_ms)
{
	Payload_Metric *metric;

	if (payload->metrics_count >= ARRAY_SIZE(payload->metrics))
	{
		LOG_ERR("Sparkplug payload full (%zu metrics)", ARRAY_SIZE(payload->metrics));
		return NULL;
	}

	metric = &payload->metrics[payload->metrics_count++];
	*metric = (Payload_Metric)Payload_Metric_init_zero;

	if (name != NULL)
	{
		metric->has_name = true;
		strncpy(metric->name, name, sizeof(metric->name) - 1);
	}
	if (alias != 0)
	{
		metric->has_alias = true;
		metric->alias = alias;
	}
	if (datatype != DataType_Unknown)
	{
		metric->has_datatype = true;
		metric->datatype = datatype;
	}
	if (timestamp_ms != 0)
	{
		metric->has_timestamp = true;
		metric->timestamp = timestamp_ms;
	}

	return metric;
}

void sparkplug_metric_set_uint32(Payload_Metric *metric, uint32_t value)
{
	metric->which_value = Payload_Metric_int_value_tag;
	metric->value.int_value = value;
}

void sparkplug_metric_set_int32(Payload_Metric *metric, int32_t value)
{
	sparkplug_metric_set_uint32(metric, (uint32_t)value);
}

void sparkplug_metric_set_uint64(Payload_Metric *metric, uint64_t value)
{
	metric->which_value = Payload_Metric_long_value_tag;
	metric->value.long_value = value;
}

void sparkplug_metric_set_float(Payload_Metric *metric, float value)
{
	metric->which_value = Payload_Metric_float_value_tag;
	metric->value.float_value = value;
}

void sparkplug_metric_set_bool(Payload_Metric *metric, bool value)
{
	metric->which_value = Payload_Metric_boolean_value_tag;
	metric->value.boolean_value = value;
}

int sparkplug_metric_get_uint64(const Payload_Metric *metric, uint64_t *value)
{
	switch (metric->which_value)
	{
	case Payload_Metric_int_value_tag:
		*value = metric->value.int_value;
		return 0;
	case Payload_Metric_long_value_tag:
		*value = metric->value.long_value;
		return 0;
	default:
		return -EINVAL;
	}
}

int sparkplug_encode(const Payload *payload, uint8_t *buf, size_t size, size_t *length)
{
	pb_ostream_t stream = pb_ostream_from_buffer(buf, size);

	if (!pb_encode(&stream, Payload_fields, payload))
	{
		LOG_ERR("Sparkplug encode failed: %s", PB_GET_ERROR(&stream));
		return -EMSGSIZE;
	}

	*length = stream.bytes_written;
	return 0;
}

int sparkplug_decode(const uint8_t *buf, size_t length, Payload *payload)
{
	pb_istream_t stream = pb_istream_from_buffer(buf, length);

	*payload = (Payload)Payload_init_zero;
	if (!pb_decode(&stream, Payload_fields, payload))
	{
		LOG_ERR("Sparkplug decode failed: %s", PB_GET_ERROR(&stream));
		return -EBADMSG;
	}

	return 0;
}
