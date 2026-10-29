/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "network.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include <errno.h>

LOG_MODULE_REGISTER(network_wifi, CONFIG_NET_CONNECTION_MANAGER_LOG_LEVEL);

#define NET_EVENT_WIFI_MASK \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_if *sta_iface;
static struct wifi_connect_req_params sta_config;
static struct net_mgmt_event_callback cb;
static bool sta_connected;

static const char *wifi_iface_name(struct net_if *iface)
{
	if (iface == sta_iface)
	{
		return "STA";
	}
	return "unknown";
}

BUILD_ASSERT(sizeof(CONFIG_WIFI_SAMPLE_SSID) > 1,
			 "CONFIG_WIFI_SAMPLE_SSID is empty. Please set it in conf file.");

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
							   struct net_if *iface)
{
	switch (mgmt_event)
	{
	case NET_EVENT_WIFI_CONNECT_RESULT:
	{
		struct wifi_status *status = (struct wifi_status *)cb->info;
		int result = status ? status->status : -ENODATA;

		if (result == 0)
		{
			sta_connected = true;
			LOG_INF("Wi-Fi %s connected (status=%d)", wifi_iface_name(iface), result);
		}
		else
		{
			sta_connected = false;
			LOG_ERR("Wi-Fi %s connection failed (status=%d)", wifi_iface_name(iface), result);
		}
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
	{
		struct wifi_status *status = (struct wifi_status *)cb->info;
		int result = status ? status->status : -ENODATA;

		sta_connected = false;
		LOG_WRN("Wi-Fi %s disconnected (status=%d)", wifi_iface_name(iface), result);
		break;
	}
	default:
		break;
	}
}

void network_log_status(void)
{
	struct net_in_addr *ipv4 = NULL;
	char ipv4_text[NET_IPV4_ADDR_LEN] = "none";

	if (sta_iface)
	{
		ipv4 = net_if_ipv4_get_global_addr(sta_iface, NET_ADDR_PREFERRED);
	}
	if (ipv4)
	{
		net_addr_ntop(NET_AF_INET, ipv4, ipv4_text, sizeof(ipv4_text));
	}

	LOG_INF("Wi-Fi status: %s, IPv4: %s",
			sta_connected ? "connected" : "disconnected",
			ipv4_text);
}

static int connect_to_wifi(void)
{
	if (!sta_iface)
	{
		LOG_INF("STA: interface not initialized");
		return -EIO;
	}

	sta_config.ssid = (const uint8_t *)CONFIG_WIFI_SAMPLE_SSID;
	sta_config.ssid_length = sizeof(CONFIG_WIFI_SAMPLE_SSID) - 1;
	sta_config.psk = (const uint8_t *)CONFIG_WIFI_SAMPLE_PSK;
	sta_config.psk_length = sizeof(CONFIG_WIFI_SAMPLE_PSK) - 1;
	sta_config.security = WIFI_SECURITY_TYPE_PSK;
	sta_config.channel = WIFI_CHANNEL_ANY;
	sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("Connecting to configured Wi-Fi network");

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config,
					   sizeof(struct wifi_connect_req_params));
	if (ret)
	{
		LOG_ERR("Unable to connect to configured Wi-Fi network");
	}

	return ret;
}

int network_wait_ipv4(int timeout_seconds)
{
	for (int i = 0; i < timeout_seconds; i++)
	{
		LOG_INF("Waiting for ipv4");

		if (sta_connected && net_if_ipv4_get_global_addr(sta_iface, NET_ADDR_PREFERRED))
		{
			return 0;
		}

		k_sleep(K_SECONDS(1));
	}

	LOG_ERR("Timed out waiting for Wi-Fi IPv4 address");
	return -ETIMEDOUT;
}

/* Wi-Fi has no pre-TLS network check; the broker check is cellular-only. */
int network_precheck(void)
{
	return 0;
}

/* Start the Wi-Fi station and request connection to the configured AP. */
int network_start(void)
{
	net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	net_mgmt_add_event_callback(&cb);

	/* Get the station interface. */
	sta_iface = net_if_get_wifi_sta();

	/* Connect failures are logged by connect_to_wifi; the IPv4 wait reports the outcome. */
	connect_to_wifi();
	return 0;
}

/* True when the station is connected and has an IPv4 address. */
bool network_has_ipv4(void)
{
	return sta_connected && net_if_ipv4_get_global_addr(sta_iface, NET_ADDR_PREFERRED) != NULL;
}
