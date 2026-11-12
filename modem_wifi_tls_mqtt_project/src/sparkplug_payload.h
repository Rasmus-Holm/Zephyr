/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sparkplug B payloads and topics: builds, encodes and decodes the protobuf messages with
 * nanopb. Knows nothing about which metrics the node has; see sparkplug_node.c for that.
 */

#ifndef SPARKPLUG_PAYLOAD_H_
#define SPARKPLUG_PAYLOAD_H_

#include "proto/sparkplug_b.pb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Longest topic: spBv1.0/<group>/DBIRTH/<edge node>/<device>. */
#define SPARKPLUG_TOPIC_MAX 96

/* Message types used in the topic. */
#define SPARKPLUG_NBIRTH "NBIRTH"
#define SPARKPLUG_NDEATH "NDEATH"
#define SPARKPLUG_NDATA  "NDATA"
#define SPARKPLUG_NCMD   "NCMD"
#define SPARKPLUG_DBIRTH "DBIRTH"
#define SPARKPLUG_DDEATH "DDEATH"
#define SPARKPLUG_DDATA  "DDATA"
#define SPARKPLUG_DCMD   "DCMD"

/**
 * @brief Milliseconds since 1970 (UTC) from the realtime clock set by SNTP.
 */
uint64_t sparkplug_now_ms(void);

/**
 * @brief Build spBv1.0/<group>/<type>/<edge node>[/<device>] from Kconfig IDs.
 *
 * @param device Device ID, or NULL for a node topic. "#" gives a subscription wildcard.
 * @return 0 on success, -ENAMETOOLONG if the topic does not fit.
 */
int sparkplug_topic(char *buf, size_t size, const char *type, const char *device);

/**
 * @brief Clear the payload and set its timestamp. seq is only set by sparkplug_payload_set_seq,
 *        because NDEATH must not carry one.
 */
void sparkplug_payload_init(Payload *payload, uint64_t timestamp_ms);

void sparkplug_payload_set_seq(Payload *payload, uint8_t seq);

/**
 * @brief Append a metric.
 *
 * @param name         Full name (BIRTH and NCMD), or NULL to send only the alias.
 * @param alias        Alias, or 0 for none. Our aliases start at 1.
 * @param datatype     DataType_..., or DataType_Unknown to leave it out (DATA messages).
 * @param timestamp_ms Time of the measurement, or 0 to leave it out.
 * @return The metric, or NULL when the payload is full. Set its value with
 *         sparkplug_metric_set_*.
 */
Payload_Metric *sparkplug_add_metric(Payload *payload, const char *name, uint64_t alias,
				     DataType datatype, uint64_t timestamp_ms);

/* Int8/16/32 and UInt8/16/32 go in int_value; signed values as two's complement. */
void sparkplug_metric_set_uint32(Payload_Metric *metric, uint32_t value);
void sparkplug_metric_set_int32(Payload_Metric *metric, int32_t value);
void sparkplug_metric_set_uint64(Payload_Metric *metric, uint64_t value);
void sparkplug_metric_set_float(Payload_Metric *metric, float value);
void sparkplug_metric_set_bool(Payload_Metric *metric, bool value);

/**
 * @brief Read an unsigned integer metric value, whichever of int_value and long_value the
 *        sender used.
 *
 * @return 0 on success, -EINVAL if the metric holds no integer.
 */
int sparkplug_metric_get_uint64(const Payload_Metric *metric, uint64_t *value);

/**
 * @brief Encode to protobuf.
 *
 * @param[out] length Bytes written.
 * @return 0 on success, -EMSGSIZE if the buffer is too small.
 */
int sparkplug_encode(const Payload *payload, uint8_t *buf, size_t size, size_t *length);

/**
 * @brief Decode protobuf (NCMD/DCMD). Unknown fields are skipped.
 *
 * @return 0 on success, -EBADMSG if the payload is invalid or does not fit the static sizes
 *         in proto/sparkplug_b.options.
 */
int sparkplug_decode(const uint8_t *buf, size_t length, Payload *payload);

#endif /* SPARKPLUG_PAYLOAD_H_ */
