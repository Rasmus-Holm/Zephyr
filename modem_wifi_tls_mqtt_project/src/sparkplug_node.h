/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sparkplug B edge node c6-01 with the Modbus slave as device behind it.
 * Topics and metrics follow the "Sparkplug B i Projekt A" section of the report.
 * All functions run in the main thread.
 */

#ifndef SPARKPLUG_NODE_H_
#define SPARKPLUG_NODE_H_

#include <stdint.h>

/**
 * @brief Build the topics and register the NCMD/DCMD receive handler. Call once at startup.
 *
 * @return 0 on success, negative errno on failure.
 */
int sparkplug_node_init(void);

/**
 * @brief Connect to the broker as a Sparkplug edge node.
 *
 * Takes the next bdSeq, registers NDEATH as Last Will, connects and subscribes to STATE,
 * NCMD and DCMD. Without a primary host NBIRTH (and DBIRTH) follow at once; with one,
 * sparkplug_node_process() sends them when the host's STATE says online. The realtime
 * clock must be set, since all timestamps come from it.
 *
 * @return 0 on success, negative errno on failure.
 */
int sparkplug_node_connect(void);

/**
 * @brief Act on STATE and commands: send the births once the host is online, send NDEATH
 *        and disconnect when it goes offline, handle rebirth and a new sample interval.
 *        Call after each mqtt_transport_process() while connected.
 */
void sparkplug_node_process(void);

/**
 * @brief Current sample interval; NCMD Config/SampleInterval_s can change it.
 */
uint32_t sparkplug_node_sample_interval_ms(void);

/**
 * @brief Record a successful Modbus reading of the device's uptime.
 *
 * Publishes DBIRTH the first time, then collects CONFIG_SPARKPLUG_BATCH_SIZE samples
 * per DDATA, each with its own timestamp.
 */
void sparkplug_node_device_sample(uint16_t uptime_seconds);

/**
 * @brief Report that the device could not be read: sends DDEATH if it was online.
 */
void sparkplug_node_device_offline(void);

#endif /* SPARKPLUG_NODE_H_ */
