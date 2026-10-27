# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Modem mode is chosen by the file NAME of an -DOVERLAY_CONFIG entry: cellular*.conf.
# Modem mode loads cellular-local.conf (broker values only, never Wi-Fi credentials).
# Every other choice is Wi-Fi mode: wifi.conf and wifi-local.conf.
set(MODEM_MODE FALSE)
set(WIFI_CONF_CHOSEN FALSE)
if(DEFINED OVERLAY_CONFIG)
  foreach(config_entry IN LISTS OVERLAY_CONFIG)
    get_filename_component(config_name "${config_entry}" NAME)
    if(config_name MATCHES "^cellular.*\\.conf$")
      set(MODEM_MODE TRUE)
    endif()
    if(config_name MATCHES "^wifi\\.conf$")
      set(WIFI_CONF_CHOSEN TRUE)
    endif()
  endforeach()
endif()

if(MODEM_MODE)
  message(STATUS "wifi-tls: modem mode (cellular-local.conf)")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cellular-local.conf")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cellular-local.conf")
  endif()
else()
  message(STATUS "wifi-tls: Wi-Fi mode (wifi.conf, wifi-local.conf)")
  # wifi.conf is already loaded through OVERLAY_CONFIG when it is the chosen file.
  if(NOT WIFI_CONF_CHOSEN)
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/wifi.conf")
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wifi-local.conf")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/wifi-local.conf")
  endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wifi-tls)

target_sources(app PRIVATE src/main.c src/modbus_reader.c)
