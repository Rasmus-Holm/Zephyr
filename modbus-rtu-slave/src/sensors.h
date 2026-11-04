#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include <stdbool.h>

struct sensor_data {
	int16_t temp_c_x10; /* temperature * 10 */
	uint16_t uptime_s_lsw;
	bool valid;
};

void sensors_init(void);
void sensors_update(void);
const struct sensor_data *sensors_get(void);

#endif /* SENSORS_H */
