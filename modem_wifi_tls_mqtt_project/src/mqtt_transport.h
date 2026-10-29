/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_TRANSPORT_H_
#define MQTT_TRANSPORT_H_

#include <stdbool.h>

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
 * @brief Publish a null-terminated string payload to the configured MQTT topic.
 *
 * @param payload Null-terminated string to publish.
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_publish(const char *payload);

/**
 * @brief Check whether the MQTT client currently has an active connection.
 *
 * @return true if connected, false otherwise.
 */
bool mqtt_transport_is_connected(void);

#endif /* MQTT_TRANSPORT_H_ */
