/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_transport.h"
#include "ppk_markers.h"
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
/* tx holds the CONNECT packet with the will; publish payloads are sent separately (iovec). */
#define MQTT_BUFFER_SIZE 256
#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MQTT_CA_CERT_TAG 1
/* Largest incoming payload (NCMD/DCMD) and topic we keep; longer ones are dropped. */
#define MQTT_RX_PAYLOAD_SIZE 512
#define MQTT_RX_TOPIC_SIZE 128

static uint8_t mqtt_rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t mqtt_tx_buffer[MQTT_BUFFER_SIZE];
static struct mqtt_client mqtt_client;
static struct sockaddr_storage mqtt_broker;
static struct pollfd mqtt_pollfd;
static bool mqtt_connected;
static int mqtt_connect_result;

static struct mqtt_topic mqtt_will_topic;
static struct mqtt_utf8 mqtt_will_message;
static bool mqtt_will_set;

static mqtt_transport_rx_cb_t mqtt_rx_callback;
static uint8_t mqtt_rx_payload[MQTT_RX_PAYLOAD_SIZE];
static char mqtt_rx_topic[MQTT_RX_TOPIC_SIZE];
static uint16_t mqtt_next_message_id = 1;

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

void mqtt_transport_set_rx_callback(mqtt_transport_rx_cb_t callback)
{
	mqtt_rx_callback = callback;
}

void mqtt_transport_set_will(const char *topic, const uint8_t *payload, size_t length)
{
	if (topic == NULL)
	{
		mqtt_will_set = false;
		return;
	}

	mqtt_will_topic.topic.utf8 = (const uint8_t *)topic;
	mqtt_will_topic.topic.size = strlen(topic);
	mqtt_will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	mqtt_will_message.utf8 = payload;
	mqtt_will_message.size = length;
	mqtt_will_set = true;
}

static uint16_t mqtt_message_id(void)
{
	uint16_t id = mqtt_next_message_id++;

	if (mqtt_next_message_id == 0)
	{
		mqtt_next_message_id = 1;
	}

	return id;
}

/* Read a received PUBLISH, acknowledge QoS 1, and hand it to the callback. */
static void mqtt_handle_publish(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	const struct mqtt_publish_param *publish = &evt->param.publish;
	size_t payload_length = publish->message.payload.len;
	size_t topic_length = publish->message.topic.topic.size;
	bool fits = payload_length <= sizeof(mqtt_rx_payload) &&
		    topic_length < sizeof(mqtt_rx_topic);
	int ret;

	/* The topic points into the rx buffer and is overwritten when the payload is read. */
	if (fits)
	{
		memcpy(mqtt_rx_topic, publish->message.topic.topic.utf8, topic_length);
		mqtt_rx_topic[topic_length] = '\0';
	}

	/* The payload must always be read off the socket, also when it is dropped. */
	for (size_t done = 0; done < payload_length;)
	{
		size_t chunk = MIN(payload_length - done, sizeof(mqtt_rx_payload));
		uint8_t *dest = fits ? &mqtt_rx_payload[done] : mqtt_rx_payload;

		ret = mqtt_readall_publish_payload(client, dest, chunk);
		if (ret < 0)
		{
			LOG_ERR("MQTT payload read failed: %d", ret);
			return;
		}
		done += chunk;
	}

	if (publish->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE)
	{
		struct mqtt_puback_param ack = {.message_id = publish->message_id};

		ret = mqtt_publish_qos1_ack(client, &ack);
		if (ret < 0)
		{
			LOG_ERR("MQTT PUBACK failed: %d", ret);
		}
	}

	if (!fits)
	{
		LOG_WRN("Dropped incoming MQTT message: topic %zu bytes, payload %zu bytes",
			topic_length, payload_length);
		return;
	}

	if (mqtt_rx_callback != NULL)
	{
		mqtt_rx_callback(mqtt_rx_topic, mqtt_rx_payload, payload_length);
	}
}

static void mqtt_event_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
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
	case MQTT_EVT_PUBLISH:
		if (evt->result != 0)
		{
			LOG_ERR("MQTT PUBLISH receive error: %d", evt->result);
			break;
		}
		mqtt_handle_publish(client, evt);
		break;
	case MQTT_EVT_SUBACK:
		LOG_INF("MQTT SUBACK received for message id %u", evt->param.suback.message_id);
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
	if (mqtt_will_set)
	{
		mqtt_client.will_topic = &mqtt_will_topic;
		mqtt_client.will_message = &mqtt_will_message;
		mqtt_client.will_retain = 0U;
	}
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

int mqtt_transport_subscribe(const char *topic)
{
	struct mqtt_topic subscription = {
		.topic = {.utf8 = (const uint8_t *)topic, .size = strlen(topic)},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	struct mqtt_subscription_list list = {
		.list = &subscription,
		.list_count = 1,
		.message_id = mqtt_message_id(),
	};
	int ret = mqtt_subscribe(&mqtt_client, &list);

	if (ret < 0)
	{
		LOG_ERR("MQTT subscribe to %s failed: %d", topic, ret);
	}
	else
	{
		LOG_INF("MQTT subscribe sent: %s", topic);
	}

	return ret;
}

/* Publish one binary payload on the established MQTT connection. */
int mqtt_transport_publish(const char *topic, const uint8_t *payload, size_t length)
{
	struct mqtt_publish_param publish_param = {0};
	int ret;

	if (topic == NULL || (payload == NULL && length > 0))
	{
		return -EINVAL;
	}

	publish_param.message.topic.topic.utf8 = (const uint8_t *)topic;
	publish_param.message.topic.topic.size = strlen(topic);
	publish_param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
	publish_param.message.payload.data = (uint8_t *)payload;
	publish_param.message.payload.len = length;
	/* QoS 0 has no packet identifier. */
	publish_param.message_id = 0;

	ppk_mark_publish(true);
	ret = mqtt_publish(&mqtt_client, &publish_param);
	ppk_mark_publish(false);
	if (ret < 0)
	{
		LOG_ERR("MQTT publish to %s failed: %d", topic, ret);
	}
	else
	{
		LOG_INF("MQTT QoS 0 publish accepted locally: %s (%zu bytes); broker delivery unverified",
			topic, length);
	}

	return ret;
}

int mqtt_transport_close(void)
{
	int ret = mqtt_disconnect(&mqtt_client, NULL);

	if (ret < 0)
	{
		LOG_ERR("MQTT disconnect failed: %d", ret);
		mqtt_abort(&mqtt_client);
	}

	mqtt_connected = false;
	return ret;
}

bool mqtt_transport_is_connected(void)
{
	return mqtt_connected;
}
