/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO markers for the Nordic PPK2 logic inputs (test plan section 2.3). Without a USB
 * console the pulses show in Power Profiler what the firmware is doing:
 *   D0 (CONFIG_APP_PPK_MARKER_MODBUS_PIN, GPIO2):  high during a Modbus read
 *   D1 (CONFIG_APP_PPK_MARKER_PUBLISH_PIN, GPIO3): high during an MQTT publish
 * A 1 s pulse on D1 marks an error that the console would otherwise have shown.
 * Without CONFIG_APP_PPK_MARKERS all functions are empty.
 */

#ifndef PPK_MARKERS_H_
#define PPK_MARKERS_H_

#include <stdbool.h>

#if defined(CONFIG_APP_PPK_MARKERS)

int ppk_markers_init(void);
void ppk_mark_modbus(bool active);
void ppk_mark_publish(bool active);
void ppk_signal_error(void);

#else

static inline int ppk_markers_init(void)
{
	return 0;
}
static inline void ppk_mark_modbus(bool active)
{
	(void)active;
}
static inline void ppk_mark_publish(bool active)
{
	(void)active;
}
static inline void ppk_signal_error(void)
{
}

#endif /* CONFIG_APP_PPK_MARKERS */

#endif /* PPK_MARKERS_H_ */
