/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NETWORK_H_
#define NETWORK_H_

#include <stdbool.h>

/**
 * @brief Start the lower network layer (Wi-Fi or Cellular).
 *
 * @return 0 on success, negative errno on failure.
 */
int network_start(void);

/**
 * @brief Wait for the interface to obtain an IPv4 address.
 *
 * @param timeout_seconds Maximum seconds to wait.
 * @return 0 on success, negative errno on timeout or failure.
 */
int network_wait_ipv4(int timeout_seconds);

/**
 * @brief Pre-check TCP link to broker before attempting TLS.
 *
 * @return 0 on success, negative errno on failure.
 */
int network_precheck(void);

/**
 * @brief Check if interface currently has an assigned IPv4 address.
 *
 * @return true if IPv4 is ready, false otherwise.
 */
bool network_has_ipv4(void);

/**
 * @brief Log current network connection and IPv4 status.
 */
void network_log_status(void);

/**
 * @brief Enable the link's power save mode (Wi-Fi: CONFIG_APP_WIFI_POWER_SAVE).
 *
 * Call after network_wait_ipv4() has succeeded.
 *
 * @return 0 on success or when not enabled in Kconfig, negative errno on failure
 *         (-ENOTSUP if the driver does not support it).
 */
int network_enable_power_save(void);

#endif /* NETWORK_H_ */
