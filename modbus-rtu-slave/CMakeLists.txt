cmake_minimum_required(VERSION 3.20.0)

if(NOT DEFINED BOARD)
	set(BOARD esp32_devkitc/esp32/procpu)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(modbus_rtu_slave)

target_sources(app PRIVATE src/main.c src/modbus_server.c src/sensors.c)
