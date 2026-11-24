/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MQTT_TRANSPORT_H_
#define MQTT_TRANSPORT_H_

#include <stdbool.h>
#include <zephyr/net/mqtt.h>

/**
 * @brief Register the MQTT CA certificate in the TLS credential subsystem.
 *
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_tls_init(void);

/**
 * @brief Configure the MQTT client with optional Last Will, open TLS connection, and wait for CONNACK.
 *
 * @param will_topic Optional Last Will topic (e.g. NDEATH), or NULL if not used.
 * @param will_payload Optional Last Will payload, or NULL if not used.
 * @param will_qos Last Will QoS (e.g. MQTT_QOS_1_AT_LEAST_ONCE).
 * @return 0 on success, negative errno on failure or timeout.
 */
int mqtt_transport_establish_will(const char *will_topic, const char *will_payload, enum mqtt_qos will_qos);

/**
 * @brief Backward-compatible establish without Last Will.
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
 * @brief Publish a null-terminated string payload to a specific MQTT topic with given QoS.
 *
 * @param topic MQTT topic string.
 * @param payload Null-terminated string to publish.
 * @param qos Quality of Service (MQTT_QOS_0_AT_MOST_ONCE or MQTT_QOS_1_AT_LEAST_ONCE).
 * @return 0 on success, negative errno on failure.
 */
int mqtt_transport_publish_topic(const char *topic, const char *payload, enum mqtt_qos qos);

/**
 * @brief Check whether the MQTT client currently has an active connection.
 *
 * @return true if connected, false otherwise.
 */
bool mqtt_transport_is_connected(void);

#endif /* MQTT_TRANSPORT_H_ */
