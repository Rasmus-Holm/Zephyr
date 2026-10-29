/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_transport.h"
#include "tls_credentials.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/poll.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(mqtt_transport);

/* Replace the broker placeholder before running an actual MQTT test. */
#define MQTT_BROKER_ADDRESS CONFIG_MQTT_BROKER_ADDRESS
#define MQTT_BROKER_PORT 8883
#define MQTT_CLIENT_ID CONFIG_MQTT_CLIENT_ID
#define MQTT_TOPIC CONFIG_MQTT_TOPIC
#define MQTT_BUFFER_SIZE 256
#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MQTT_CA_CERT_TAG 1

static uint8_t mqtt_rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t mqtt_tx_buffer[MQTT_BUFFER_SIZE];
static struct mqtt_client mqtt_client;
static struct sockaddr_storage mqtt_broker;
static struct pollfd mqtt_pollfd;
static bool mqtt_connected;
static int mqtt_connect_result;

static sec_tag_t mqtt_sec_tags[] = {MQTT_CA_CERT_TAG};

int mqtt_transport_tls_init(void)
{
	int ret = tls_credential_add(MQTT_CA_CERT_TAG,
					 TLS_CREDENTIAL_CA_CERTIFICATE,
					 ca_certificate,
					 sizeof(ca_certificate));

	if (ret < 0)
	{
		LOG_ERR("Failed to register MQTT CA certificate: %d", ret);
	}

	return ret;
}

static void mqtt_event_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	ARG_UNUSED(client);

	switch (evt->type)
	{
	case MQTT_EVT_CONNACK:
		mqtt_connect_result = evt->result;
		if (evt->result == 0)
		{
			mqtt_connected = true;
			LOG_INF("MQTT connected");
		}
		else
		{
			LOG_ERR("MQTT CONNACK failed: %d", evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		if (!mqtt_connected)
		{
			mqtt_connect_result = evt->result;
		}
		mqtt_connected = false;
		LOG_INF("MQTT disconnected: %d", evt->result);
		break;
	default:
		break;
	}
}

static int mqtt_client_setup(void)
{
	struct sockaddr_in *broker = (struct sockaddr_in *)&mqtt_broker;

	memset(&mqtt_broker, 0, sizeof(mqtt_broker));
	broker->sin_family = AF_INET;
	broker->sin_port = htons(MQTT_BROKER_PORT);
	if (inet_pton(AF_INET, MQTT_BROKER_ADDRESS, &broker->sin_addr) != 1)
	{
		LOG_ERR("Invalid MQTT broker address: %s", MQTT_BROKER_ADDRESS);
		return -EINVAL;
	}

	mqtt_client_init(&mqtt_client);
	mqtt_client.broker = &mqtt_broker;
	mqtt_client.evt_cb = mqtt_event_handler;
	mqtt_client.client_id.utf8 = (uint8_t *)MQTT_CLIENT_ID;
	mqtt_client.client_id.size = sizeof(MQTT_CLIENT_ID) - 1;
	mqtt_client.protocol_version = MQTT_VERSION_3_1_1;
	mqtt_client.rx_buf = mqtt_rx_buffer;
	mqtt_client.rx_buf_size = sizeof(mqtt_rx_buffer);
	mqtt_client.tx_buf = mqtt_tx_buffer;
	mqtt_client.tx_buf_size = sizeof(mqtt_tx_buffer);
	mqtt_client.transport.type = MQTT_TRANSPORT_SECURE;
	struct mqtt_sec_config *tls_config = &mqtt_client.transport.tls.config;

	tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = mqtt_sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(mqtt_sec_tags);
	/* Verify the broker certificate against its IP without sending IP-as-SNI. */
	tls_config->hostname = MQTT_BROKER_ADDRESS;

	return 0;
}

/* Establish the MQTT client and wait for broker acceptance. */
int mqtt_transport_establish(void)
{
	int ret;

	LOG_INF("MQTT setup running");

	ret = mqtt_client_setup();
	if (ret < 0)
	{
		return ret;
	}

	mqtt_connected = false;
	mqtt_connect_result = -ETIMEDOUT;
	ret = mqtt_connect(&mqtt_client);
	if (ret < 0)
	{
		LOG_ERR("MQTT connect request failed: %d", ret);
		mqtt_abort(&mqtt_client);
		return ret;
	}

	mqtt_pollfd.fd = mqtt_client.transport.tls.sock;
	mqtt_pollfd.events = POLLIN;
	for (int elapsed = 0; elapsed < MQTT_CONNECT_TIMEOUT_MS && !mqtt_connected; elapsed += 100)
	{
		LOG_INF("MQTT connecting...");

		ret = poll(&mqtt_pollfd, 1, 100);
		if (ret < 0)
		{
			LOG_ERR("MQTT poll failed: %d", errno);
			mqtt_abort(&mqtt_client);
			return -errno;
		}
		if (ret > 0 && (mqtt_pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
		{
			LOG_ERR("MQTT socket error, poll events: 0x%x", mqtt_pollfd.revents);
			mqtt_abort(&mqtt_client);
			return -EIO;
		}
		if (ret > 0 && (mqtt_pollfd.revents & POLLIN))
		{
			ret = mqtt_input(&mqtt_client);
			if (ret < 0)
			{
				LOG_ERR("MQTT input failed: %d", ret);
				mqtt_abort(&mqtt_client);
				return ret;
			}
		}
	}

	if (!mqtt_connected)
	{
		LOG_ERR("MQTT CONNACK timeout/result: %d", mqtt_connect_result);
		mqtt_abort(&mqtt_client);
		return -ETIMEDOUT;
	}

	LOG_INF("MQTT CONNACK accepted");
	return 0;
}

int mqtt_transport_process(int timeout_ms)
{
	int ret = poll(&mqtt_pollfd, 1, timeout_ms);

	if (ret < 0)
	{
		int poll_errno = errno;

		LOG_ERR("MQTT poll failed: %d", poll_errno);
		mqtt_abort(&mqtt_client);
		mqtt_connected = false;
		return -poll_errno;
	}

	if (ret > 0 && (mqtt_pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
	{
		LOG_ERR("MQTT socket error, poll events: 0x%x", mqtt_pollfd.revents);
		mqtt_abort(&mqtt_client);
		mqtt_connected = false;
		return -EIO;
	}

	if (ret > 0 && (mqtt_pollfd.revents & POLLIN))
	{
		ret = mqtt_input(&mqtt_client);
		if (ret < 0)
		{
			LOG_ERR("MQTT input failed: %d", ret);
			mqtt_abort(&mqtt_client);
			mqtt_connected = false;
			return ret;
		}
	}
	else if (ret == 0)
	{
		ret = mqtt_live(&mqtt_client);
		if (ret < 0 && ret != -EAGAIN)
		{
			LOG_ERR("MQTT keepalive failed: %d", ret);
			mqtt_abort(&mqtt_client);
			mqtt_connected = false;
			return ret;
		}
	}

	return 0;
}

/* Publish one string payload on the established MQTT connection. */
int mqtt_transport_publish(const char *payload)
{
	struct mqtt_publish_param publish_param = {0};
	int ret;

	if (!payload)
	{
		return -EINVAL;
	}

	publish_param.message.topic.topic.utf8 = (uint8_t *)MQTT_TOPIC;
	publish_param.message.topic.topic.size = sizeof(MQTT_TOPIC) - 1;
	publish_param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	publish_param.message.payload.data = (uint8_t *)payload;
	publish_param.message.payload.len = strlen(payload);
	publish_param.message_id = 1;

	LOG_INF("Publishing to mqtt");
	ret = mqtt_publish(&mqtt_client, &publish_param);
	if (ret < 0)
	{
		LOG_ERR("MQTT publish failed: %d", ret);
		LOG_INF("publish failed");
	}
	else
	{
		LOG_INF("MQTT QoS 0 publish accepted locally: topic=%s; broker delivery unverified",
				MQTT_TOPIC);
		LOG_INF("Succesfully published");
	}

	return ret;
}

bool mqtt_transport_is_connected(void)
{
	return mqtt_connected;
}
