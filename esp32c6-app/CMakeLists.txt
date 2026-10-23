cmake_minimum_required(VERSION 3.20.0)

# Board: ESP32-C6 DevKitC (4 MB flash). Kan overskrives med `west build -b ...`.
if(NOT DEFINED BOARD)
	set(BOARD esp32c6_devkitc/esp32c6/hpcore)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32c6_app)

target_sources(app PRIVATE src/main.c)
