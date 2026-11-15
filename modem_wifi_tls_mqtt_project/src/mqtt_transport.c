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
#define MQTT_BUFFER_SIZE 512
#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MQTT_CA_CERT_TAG 1

static uint8_t mqtt_rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t mqtt_tx_buffer[MQTT_BUFFER_SIZE];
static struct mqtt_client mqtt_client;
static struct sockaddr_storage mqtt_broker;
static struct pollfd mqtt_pollfd;
static bool mqtt_connected;
static int mqtt_connect_result;
static uint16_t mqtt_message_id_counter = 1;

/* Static storage for Last Will to prevent stack corruption.
 * Zephyr keeps the Last Will topic and its QoS together in struct mqtt_topic. */
static struct mqtt_topic mqtt_will_topic;
static struct mqtt_utf8 will_msg_utf8;
static char static_will_topic[128];
static char static_will_payload[256];

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
	case MQTT_EVT_PUBACK:
		if (evt->result == 0)
		{
			LOG_INF("MQTT PUBACK received (message id: %u)", evt->param.puback.message_id);
		}
		else
		{
			LOG_WRN("MQTT PUBACK error: %d", evt->result);
		}
		break;
	default:
		break;
	}
}

static int mqtt_client_setup(const char *will_topic, const char *will_payload, enum mqtt_qos will_qos)
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

	/* Configure Last Will (NDEATH) if provided */
	if (will_topic && will_payload)
	{
		size_t topic_len = strlen(will_topic);
		size_t payload_len = strlen(will_payload);

		if (topic_len < sizeof(static_will_topic) && payload_len < sizeof(static_will_payload))
		{
			memcpy(static_will_topic, will_topic, topic_len + 1);
			memcpy(static_will_payload, will_payload, payload_len + 1);

			mqtt_will_topic.topic.utf8 = (uint8_t *)static_will_topic;
			mqtt_will_topic.topic.size = topic_len;
			mqtt_will_topic.qos = will_qos;
			will_msg_utf8.utf8 = (uint8_t *)static_will_payload;
			will_msg_utf8.size = payload_len;

			mqtt_client.will_topic = &mqtt_will_topic;
			mqtt_client.will_message = &will_msg_utf8;
			mqtt_client.will_retain = 0;
			LOG_INF("Configured MQTT Last Will: topic=%s, qos=%d", will_topic, will_qos);
		}
		else
		{
			LOG_ERR("Last Will topic or payload exceeds buffer size");
			return -ENOMEM;
		}
	}

	struct mqtt_sec_config *tls_config = &mqtt_client.transport.tls.config;

	tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = mqtt_sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(mqtt_sec_tags);
	/* Verify the broker certificate against its IP without sending IP-as-SNI. */
	tls_config->hostname = MQTT_BROKER_ADDRESS;

	return 0;
}

/* Establish the MQTT client and wait for broker acceptance with optional Last Will. */
int mqtt_transport_establish_will(const char *will_topic, const char *will_payload, enum mqtt_qos will_qos)
{
	int ret;

	LOG_INF("MQTT setup running");

	ret = mqtt_client_setup(will_topic, will_payload, will_qos);
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

int mqtt_transport_establish(void)
{
	return mqtt_transport_establish_will(NULL, NULL, MQTT_QOS_0_AT_MOST_ONCE);
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

/* Publish one string payload to a specified topic with given QoS. */
int mqtt_transport_publish_topic(const char *topic, const char *payload, enum mqtt_qos qos)
{
	struct mqtt_publish_param publish_param = {0};
	int ret;

	if (!topic || !payload)
	{
		return -EINVAL;
	}

	publish_param.message.topic.topic.utf8 = (uint8_t *)topic;
	publish_param.message.topic.topic.size = strlen(topic);
	publish_param.message.topic.qos = qos;
	publish_param.message.payload.data = (uint8_t *)payload;
	publish_param.message.payload.len = strlen(payload);
	if (qos > MQTT_QOS_0_AT_MOST_ONCE)
	{
		if (++mqtt_message_id_counter == 0)
		{
			mqtt_message_id_counter = 1;
		}
		publish_param.message_id = mqtt_message_id_counter;
	}
	else
	{
		publish_param.message_id = 0;
	}

	LOG_INF("Publishing to %s (len=%u, qos=%d)", topic, (unsigned int)strlen(payload), qos);
	ret = mqtt_publish(&mqtt_client, &publish_param);
	if (ret < 0)
	{
		LOG_ERR("MQTT publish failed on %s: %d", topic, ret);
	}
	else
	{
		LOG_INF("MQTT publish accepted locally: topic=%s, qos=%d", topic, qos);
	}

	return ret;
}

/* Publish one string payload on the default configured MQTT topic (QoS 0). */
int mqtt_transport_publish(const char *payload)
{
	return mqtt_transport_publish_topic(MQTT_TOPIC, payload, MQTT_QOS_0_AT_MOST_ONCE);
}

bool mqtt_transport_is_connected(void)
{
	return mqtt_connected;
}
