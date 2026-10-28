
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/tls_credentials.h>
#if IS_ENABLED(CONFIG_NETWORK_CELLULAR)
#include <zephyr/device.h>
#include <zephyr/drivers/cellular.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/pm/device.h>
#else
#include <zephyr/net/wifi_mgmt.h>
#endif
#include <zephyr/sys/clock.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/poll.h>

#include <errno.h>
#include <string.h>
#include <time.h>

#include "modbus_reader.h"
#include "tls_credentials.h"

LOG_MODULE_REGISTER(MAIN);

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"

#if !IS_ENABLED(CONFIG_NETWORK_CELLULAR)
#define NET_EVENT_WIFI_MASK \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT)

static struct net_if *sta_iface;

static struct wifi_connect_req_params sta_config;

static struct net_mgmt_event_callback cb;
static bool sta_connected;
#endif

/* Replace the broker placeholder before running an actual MQTT test. */
#define MQTT_BROKER_ADDRESS CONFIG_MQTT_BROKER_ADDRESS
#define MQTT_BROKER_PORT 8883
#define MQTT_CLIENT_ID CONFIG_MQTT_CLIENT_ID
#define MQTT_TOPIC CONFIG_MQTT_TOPIC
#define MQTT_PAYLOAD "{\"status\":\"hello\"}"
#define MQTT_BUFFER_SIZE 256
#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MQTT_CA_CERT_TAG 1
#define MQTT_SNTP_SERVER CONFIG_MQTT_SNTP_SERVER
#define MODBUS_EXPECTED_TEST_VALUE 1234
#define MODBUS_PUBLISH_INTERVAL_MS 10000

static uint8_t mqtt_rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t mqtt_tx_buffer[MQTT_BUFFER_SIZE];
static struct mqtt_client mqtt_client;
static struct sockaddr_storage mqtt_broker;
static struct pollfd mqtt_pollfd;
static bool mqtt_connected;
static int mqtt_connect_result;
/* False after a failed pre-TLS network check, so no TLS or MQTT is attempted on that link. */
static bool mqtt_allowed = true;
/* True once SNTP has set the clock; certificate dates are checked against it. */
static bool clock_synchronized;

static sec_tag_t mqtt_sec_tags[] = {MQTT_CA_CERT_TAG};

static int mqtt_tls_init(void)
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

#if IS_ENABLED(CONFIG_NETWORK_CELLULAR)

/* Devicetree is read before Kconfig, so the modem overlay must be passed at build time. */
#if !DT_NODE_EXISTS(DT_ALIAS(modem))
#error "Cellular build needs the modem alias; pass -DEXTRA_DTC_OVERLAY_FILE with modem-uart1.overlay"
#endif

#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define L4_CONNECTED_FLAG 1
#define NETWORK_POLL_SECONDS 5
#define MODEM_INFO_BUFFER_SIZE 64
/* Onomondo ICCIDs start with this prefix; only the prefix is logged. */
#define ICCID_PREFIX_LENGTH 6

static const struct device *const modem = DEVICE_DT_GET(DT_ALIAS(modem));
static const struct device *const modem_uart = DEVICE_DT_GET(DT_PARENT(DT_ALIAS(modem)));
/* 3GPP 27.010 basic-mode CLD (close multiplexer) on DLCI 0. */
static const uint8_t cmux_close_frame[] = {0xF9, 0x03, 0xEF, 0x05, 0xC3, 0x01, 0xF2, 0xF9};
#define CMUX_CLOSE_SETTLE_MS 500
/* Fixed wait for the modem to restart; the driver's init script sends its first AT 11.5 s after boot. */
#define MODEM_RESTART_WAIT_MS 4000
static struct net_if *ppp_iface;
K_EVENT_DEFINE(l4_event);

/* Track PPP link state so the IP wait and the status check see the same flag. */
static void l4_event_handler(uint64_t event, struct net_if *iface, void *info,
							 size_t info_length, void *user_data)
{
	if (iface != ppp_iface)
	{
		return;
	}

	if (event == NET_EVENT_L4_CONNECTED)
	{
		k_event_post(&l4_event, L4_CONNECTED_FLAG);
	}
	else if (event == NET_EVENT_L4_DISCONNECTED)
	{
		k_event_set(&l4_event, 0);
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(l4_events, L4_EVENT_MASK, l4_event_handler, NULL);

/* Name the registration status values so a rejected SIM is easy to recognise. */
static const char *registration_text(enum cellular_registration_status status)
{
	switch (status)
	{
	case CELLULAR_REGISTRATION_NOT_REGISTERED:
		return "not registered, not searching";
	case CELLULAR_REGISTRATION_REGISTERED_HOME:
		return "registered, home network";
	case CELLULAR_REGISTRATION_SEARCHING:
		return "searching for network";
	case CELLULAR_REGISTRATION_DENIED:
		return "DENIED by network";
	case CELLULAR_REGISTRATION_REGISTERED_ROAMING:
		return "registered, roaming";
	default:
		return "unknown or other";
	}
}

/* Log LTE registration status while waiting, so progress or rejection is visible. */
static void log_registration_progress(void)
{
	enum cellular_registration_status registration = CELLULAR_REGISTRATION_UNKNOWN;

	if (cellular_get_registration_status(modem, CELLULAR_ACCESS_TECHNOLOGY_E_UTRAN,
										 &registration) < 0)
	{
		LOG_WRN("Registration status: unavailable");
		return;
	}

	LOG_INF("Registration status: %d (%s)", registration, registration_text(registration));
}

/* Log one text modem field, marking it unknown when the read fails. */
static void log_modem_info(const char *label, enum cellular_modem_info_type type, size_t length)
{
	char value[MODEM_INFO_BUFFER_SIZE] = "-";

	if (cellular_get_modem_info(modem, type, value, sizeof(value)) < 0)
	{
		LOG_WRN("%s: unknown", label);
		return;
	}

	LOG_INF("%s: %.*s", label, (int)length, value);
}

/* Log facts that show which modem, SIM and radio link are in use. Runs once after IP. */
static void log_modem_facts(void)
{
	int16_t rssi_dbm = 0;

	log_modem_info("Model", CELLULAR_MODEM_INFO_MODEL_ID, MODEM_INFO_BUFFER_SIZE);
	log_modem_info("Firmware", CELLULAR_MODEM_INFO_FW_VERSION, MODEM_INFO_BUFFER_SIZE);
	log_modem_info("ICCID prefix", CELLULAR_MODEM_INFO_SIM_ICCID, ICCID_PREFIX_LENGTH);

	if (cellular_get_signal(modem, CELLULAR_SIGNAL_RSSI, &rssi_dbm) == 0)
	{
		LOG_INF("RSSI: %d dBm", rssi_dbm);
	}
	else
	{
		LOG_WRN("RSSI: unknown");
	}
}

/*
 * Close a CMUX session the modem kept from the previous boot, so the driver's AT script is
 * answered. The driver opens the UART early, but its init script sends the first AT only
 * 11.5 s after boot (1.5 s power pulse and 10 s startup time for the SIM7080).
 */
static void modem_close_stale_cmux(void)
{
	unsigned char stale_byte;

	LOG_INF("Closing any CMUX session left by the previous boot");
	for (size_t i = 0; i < sizeof(cmux_close_frame); i++)
	{
		uart_poll_out(modem_uart, cmux_close_frame[i]);
	}

	/* Let the modem answer, then discard the answer so the driver does not read it. */
	k_msleep(CMUX_CLOSE_SETTLE_MS);
	while (uart_poll_in(modem_uart, &stale_byte) == 0)
	{
	}
}

/* Send one AT command on the modem UART in plain AT mode. */
static void modem_send_at(const char *command)
{
	for (size_t i = 0; command[i] != '\0'; i++)
	{
		uart_poll_out(modem_uart, command[i]);
	}
	uart_poll_out(modem_uart, '\r');
}

/*
 * Reset the module with AT+CFUN=1,1 and wait for it to restart. A modem that is still attached
 * from the previous boot refuses ATD*99***1# (NO CARRIER); a reset gives the same state as a
 * power cycle. The modem's reply was not readable here with uart_poll_in in most measured runs,
 * so the wait is fixed. The restart took 2.5 to 2.8 s in measurements.
 */
static void modem_reset_module(void)
{
	LOG_INF("Resetting the modem module");
	modem_send_at("AT+CFUN=1,1");
	k_msleep(MODEM_RESTART_WAIT_MS);
}

/* Power the modem and bring the PPP interface up. */
static int network_start(void)
{
	int ret;

	modem_close_stale_cmux();
	modem_reset_module();

	ppp_iface = net_if_get_first_by_type(&NET_L2_GET_NAME(PPP));
	if (!ppp_iface)
	{
		LOG_ERR("No PPP interface found");
		return -ENODEV;
	}

	LOG_INF("Powering on modem");
	/* The driver resumes itself at boot, so -EALREADY means it is already running. */
	ret = pm_device_action_run(modem, PM_DEVICE_ACTION_RESUME);
	if (ret < 0 && ret != -EALREADY)
	{
		LOG_ERR("Modem resume failed: %d", ret);
		return ret;
	}

	ret = net_if_up(ppp_iface);
	if (ret < 0)
	{
		LOG_ERR("PPP interface up failed: %d", ret);
	}

	return ret;
}

/* True when the PPP link has an IPv4 address. */
static bool network_has_ipv4(void)
{
	return k_event_test(&l4_event, L4_CONNECTED_FLAG) != 0;
}

/* Wait for PPP to assign an address, logging LTE registration while waiting. */
static int network_wait_ipv4(int timeout_seconds)
{
	for (int waited_seconds = 0; waited_seconds < timeout_seconds;
		 waited_seconds += NETWORK_POLL_SECONDS)
	{
		uint32_t events = k_event_wait(&l4_event, L4_CONNECTED_FLAG, false,
									   K_SECONDS(NETWORK_POLL_SECONDS));

		if (events == L4_CONNECTED_FLAG)
		{
			LOG_INF("PPP link up, IP address assigned");
			log_modem_facts();
			return 0;
		}

		log_registration_progress();
	}

	LOG_ERR("No IP address within %d s", timeout_seconds);
	return -ETIMEDOUT;
}

static void network_log_status(void)
{
	LOG_INF("PPP status: %s", network_has_ipv4() ? "IP assigned" : "no IP");
}

/* Open a plain TCP connection to the broker to prove the link before TLS. */
static int network_precheck(void)
{
	struct sockaddr_in endpoint = {0};
	int sock;
	int ret;
	int connect_errno;

	endpoint.sin_family = AF_INET;
	endpoint.sin_port = htons(MQTT_BROKER_PORT);
	if (inet_pton(AF_INET, MQTT_BROKER_ADDRESS, &endpoint.sin_addr) != 1)
	{
		LOG_ERR("Invalid TCP endpoint address: %s", MQTT_BROKER_ADDRESS);
		return -EINVAL;
	}

	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0)
	{
		LOG_ERR("TCP socket open failed: %d", errno);
		return -errno;
	}

	/* Blocking connect; the TCP stack sets the timeout. */
	ret = zsock_connect(sock, (struct sockaddr *)&endpoint, sizeof(endpoint));
	/* Keep errno before close, because close may overwrite it. */
	connect_errno = errno;
	zsock_close(sock);
	if (ret < 0)
	{
		LOG_ERR("TCP connect to %s:%d failed: %d", MQTT_BROKER_ADDRESS, MQTT_BROKER_PORT,
				connect_errno);
		return -connect_errno;
	}

	LOG_INF("TCP connection to %s:%d succeeded", MQTT_BROKER_ADDRESS, MQTT_BROKER_PORT);
	return 0;
}

#else

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

static void network_log_status(void)
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
		LOG_INF("STA: interface no initialized");
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

static int network_wait_ipv4(int timeout_seconds)
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
static int network_precheck(void)
{
	return 0;
}

/* Start the Wi-Fi station and request connection to the configured AP. */
static int network_start(void)
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
static bool network_has_ipv4(void)
{
	return sta_connected && net_if_ipv4_get_global_addr(sta_iface, NET_ADDR_PREFERRED) != NULL;
}

#endif

/* Log the IPv4 DNS servers the resolver holds, so a missing or wrong server is visible. */
static void log_dns_servers(void)
{
	struct dns_resolve_context *dns = dns_resolve_get_default();
	char text[NET_IPV4_ADDR_LEN];
	int found = 0;

	for (int i = 0; i < DNS_RESOLVER_MAX_POLL; i++)
	{
		const struct net_sockaddr *server = &dns->servers[i].dns_server;

		if (server->sa_family != NET_AF_INET)
		{
			continue;
		}

		net_addr_ntop(NET_AF_INET, &net_sin(server)->sin_addr, text, sizeof(text));
		LOG_INF("DNS server %d: %s", i, text);
		found++;
	}

	if (found == 0)
	{
		LOG_WRN("No IPv4 DNS server configured");
	}
}

static int synchronize_clock(void)
{
	struct sntp_time sntp_time;
	struct timespec realtime;
	int ret;

	int64_t started_ms = k_uptime_get();

	ret = sntp_simple(MQTT_SNTP_SERVER, 5000, &sntp_time);
	LOG_INF("SNTP name lookup and query took %d ms", (int)(k_uptime_get() - started_ms));
	if (ret < 0)
	{
		LOG_ERR("SNTP query failed: %d", ret);
		log_dns_servers();
		return ret;
	}

	realtime.tv_sec = (time_t)sntp_time.seconds;
	realtime.tv_nsec = ((uint64_t)sntp_time.fraction * NSEC_PER_SEC) >> 32;
	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &realtime);
	if (ret < 0)
	{
		LOG_ERR("Setting realtime clock failed: %d", ret);
		return ret;
	}

	LOG_INF("Realtime clock synchronized before TLS");
	clock_synchronized = true;
	return 0;
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
static int mqtt_publish_setup(void)
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

/* Publish one string payload on the established MQTT connection. */
static int mqtt_publish_payload(const char *payload)
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

/* Read and validate the RTU values before publishing the slave's uptime. */
static int mqtt_publish_modbus_uptime(void)
{
	char payload[MQTT_BUFFER_SIZE];
	uint16_t test_value;
	uint16_t uptime_seconds;
	int ret;

	ret = modbus_reader_read(&test_value, &uptime_seconds);
	if (ret < 0)
	{
		LOG_ERR("Modbus RTU read failed: %d", ret);
		return ret;
	}

	if (test_value != MODBUS_EXPECTED_TEST_VALUE)
	{
		LOG_ERR("Modbus validation failed: register 0 = %u", test_value);
		return -EINVAL;
	}

	LOG_INF("Modbus sample: register 0 = %u, uptime = %u s", test_value, uptime_seconds);
	ret = snprintk(payload, sizeof(payload), "payload %u", uptime_seconds);
	if (ret < 0 || ret >= sizeof(payload))
	{
		LOG_ERR("Modbus payload formatting failed: %d", ret);
		return -EMSGSIZE;
	}

	return mqtt_publish_payload(payload);
}

int main(void)
{
	int64_t next_modbus_publish_ms = 0;
	bool modbus_ready = false;
	int ret;

	k_sleep(K_SECONDS(5));

	ret = modbus_reader_init();
	if (ret < 0)
	{
		LOG_ERR("Modbus client initialization failed: %d; telemetry disabled", ret);
	}
	else
	{
		modbus_ready = true;
		LOG_INF("Modbus RTU client ready; sample interval %d ms", MODBUS_PUBLISH_INTERVAL_MS);
	}

	if (mqtt_tls_init() < 0)
	{
		LOG_ERR("TLS initialization failed");
		return 0;
	}

	if (network_start() < 0)
	{
		LOG_ERR("Lower network layer failed to start");
	}
	else if (network_wait_ipv4(CONFIG_NETWORK_IPV4_TIMEOUT_SECONDS) == 0)
	{
		LOG_INF("ipv4 recieved");
		if (network_precheck() < 0)
		{
			LOG_ERR("Stopping before TLS: plain TCP to broker failed");
			mqtt_allowed = false;
		}
		else if (synchronize_clock() == 0 && mqtt_publish_setup() == 0)
		{
			next_modbus_publish_ms = k_uptime_get();
		}
	}

	while (true)
	{
		network_log_status();

		if (mqtt_connected)
		{
			int ret = poll(&mqtt_pollfd, 1, 1000);

			if (ret < 0)
			{
				LOG_ERR("MQTT poll failed: %d", errno);
				mqtt_abort(&mqtt_client);
				mqtt_connected = false;
			}
			else if (ret > 0 && (mqtt_pollfd.revents & POLLIN))
			{
				ret = mqtt_input(&mqtt_client);
				if (ret < 0)
				{
					LOG_ERR("MQTT input failed: %d", ret);
					mqtt_abort(&mqtt_client);
					mqtt_connected = false;
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
				}
			}
		}
		else
		{
			k_sleep(K_SECONDS(5));
			/* Retry the clock first: a wrong clock makes TLS reject the certificate. */
			if (mqtt_allowed && network_has_ipv4() &&
			    (clock_synchronized || synchronize_clock() == 0))
			{
				if (mqtt_publish_setup() == 0)
				{
					next_modbus_publish_ms = k_uptime_get();
				}
			}
		}

		if (mqtt_connected && modbus_ready && k_uptime_get() >= next_modbus_publish_ms)
		{
			mqtt_publish_modbus_uptime();
			next_modbus_publish_ms = k_uptime_get() + MODBUS_PUBLISH_INTERVAL_MS;
		}
	}
}
