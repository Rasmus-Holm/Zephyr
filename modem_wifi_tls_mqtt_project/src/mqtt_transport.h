/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_TRANSPORT_H_
#define MQTT_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Register the MQTT CA certificate in the TLS credential subsystem.
 *
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_tls_init(void);

/**
 * @brief Configure the MQTT client, open TLS connection, and wait for CONNACK.
 *
 * @return 0 on success, negative errno on failure or timeout.
 */
int mqtt_transport_establish(void);

/**
 * @brief Poll the MQTT socket, process incoming packets, and send keepalive pings.
 *
 * If an error occurs, the connection is aborted internally and
 * mqtt_transport_is_connected() becomes false.
 *
 * @param timeout_ms Maximum time to wait in poll() in milliseconds.
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_process(int timeout_ms);

/**
 * @brief Called for each PUBLISH received on a subscribed topic.
 *
 * Runs inside mqtt_transport_process(); it must not publish or block.
 * Both pointers are only valid during the call.
 */
typedef void (*mqtt_transport_rx_cb_t)(const char *topic, const uint8_t *payload, size_t length);

void mqtt_transport_set_rx_callback(mqtt_transport_rx_cb_t callback);

/**
 * @brief Set the Last Will that the broker publishes if the connection dies.
 *
 * Used by the next mqtt_transport_establish(). The will is sent with QoS 1 and no retain,
 * as Sparkplug B requires for NDEATH. The caller keeps both buffers alive until the next call.
 *
 * @param topic Null-terminated topic, or NULL for no will.
 */
void mqtt_transport_set_will(const char *topic, const uint8_t *payload, size_t length);

/**
 * @brief Subscribe to one topic with QoS 1. The SUBACK is not awaited.
 *
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_subscribe(const char *topic);

/**
 * @brief Publish a binary payload with QoS 0 on the established connection.
 *
 * @param topic Null-terminated topic.
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_publish(const char *topic, const uint8_t *payload, size_t length);

/**
 * @brief Send DISCONNECT and close the connection. The broker does not publish the will.
 *
 * @return 0 on success, negative errno on failure (the connection is aborted anyway).
 */
int mqtt_transport_close(void);

/**
 * @brief Check whether the MQTT client currently has an active connection.
 *
 * @return true if connected, false otherwise.
 */
bool mqtt_transport_is_connected(void);

#endif /* MQTT_TRANSPORT_H_ */
