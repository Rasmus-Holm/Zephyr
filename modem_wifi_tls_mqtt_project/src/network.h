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

#endif /* NETWORK_H_ */
