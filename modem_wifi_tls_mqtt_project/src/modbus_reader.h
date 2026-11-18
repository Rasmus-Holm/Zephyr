#ifndef MODBUS_READER_H
#define MODBUS_READER_H

#include <stdint.h>

/* Initialize the Modbus RTU client. Returns 0 or a negative error. */
int modbus_reader_init(void);

/* Read the slave ADC reading in millivolts and the uptime from holding registers 0 and 1. */
int modbus_reader_read(uint16_t *adc_millivolts, uint16_t *uptime_seconds);

#endif
