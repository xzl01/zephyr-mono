/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>

#include <papermono/ui.h>
#include <papermono/weather.h>

LOG_MODULE_REGISTER(papermono_weather, LOG_LEVEL_INF);

#define WEATHER_HOST "wttr.in"
#define WEATHER_PATH "/Shanghai?format=j1"
#define WEATHER_POLL_MS (30 * 60 * 1000)

extern const uint8_t papermono_ca_isrg_root_x1[];
extern const size_t papermono_ca_isrg_root_x1_len;

static struct papermono_weather snapshot;
static K_MUTEX_DEFINE(weather_lock);
static K_SEM_DEFINE(weather_request, 0, 1);
static atomic_t busy;

static bool weather_network_ready(void)
{
	struct net_if *iface = net_if_get_wifi_sta();

	if (iface == NULL) {
		iface = net_if_get_default();
	}

	if (iface == NULL) {
		return false;
	}
	return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

/* This tree's DHCP client stores the gateway but never installs it as a
 * default router, so off-link traffic fails with ENOENT while the LAN works.
 * Promote the DHCP gateway to a default router once the address is up.
 */
static void ensure_default_router(void)
{
	struct net_if *iface = net_if_get_wifi_sta();
	struct net_in_addr gw;

	if (iface == NULL) {
		return;
	}
	gw = net_if_ipv4_get_gw(iface);
	if (gw.s4_addr[0] == 0U) {
		return;
	}
	if (net_if_ipv4_router_find_default(iface, NULL) == NULL) {
		(void)net_if_ipv4_router_add(iface, &gw, true, 0);
		LOG_INF("Installed default router %u.%u.%u.%u",
			gw.s4_addr[0], gw.s4_addr[1], gw.s4_addr[2], gw.s4_addr[3]);
	}
}

static int http_get(char *response, size_t response_size)
{
	struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *result = NULL;
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(80) };
	int sock;
	int ret;

	int dns_ret = zsock_getaddrinfo(WEATHER_HOST, "80", &hints, &result);

	if (dns_ret == 0 && result != NULL) {
		memcpy(&addr, result->ai_addr, sizeof(addr));
		zsock_freeaddrinfo(result);
	} else {
		LOG_WRN("wttr.in DNS failed (%d), using fallback", dns_ret);
		/* Router DHCP did not yield a usable resolver on every network.
		 * Keep a last-resort address so weather still refreshes; the Host
		 * header keeps the virtual host correct.
		 */
		/* 188.40.99.226 in network byte order; inet_pton is not linked in
		 * every socket configuration.
		 */
		addr.sin_addr.s_addr = htonl(0x05F9F3BBUL);
	}
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -errno;
	}
	if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		LOG_WRN("wttr.in connect failed (%d) to %s", ret,
			net_addr_ntop(AF_INET, &addr.sin_addr, (char[16]){0}, 16));
		zsock_close(sock);
		return ret;
	}
	LOG_INF("wttr.in connected to %s", net_addr_ntop(AF_INET,
		&addr.sin_addr, (char[16]){0}, 16));
	char request[320];
	int length = snprintf(request, sizeof(request),
		"GET " WEATHER_PATH " HTTP/1.1\r\nHost: " WEATHER_HOST
		"\r\nAccept: application/json\r\nConnection: close\r\n\r\n");

	ssize_t sent = zsock_send(sock, request, (size_t)length, 0);

	if (sent != length) {
		LOG_WRN("wttr.in send failed (%d of %d, errno %d)",
			(int)sent, length, errno);
		zsock_close(sock);
		return -EIO;
	}
	size_t used = 0U;

	while (used + 1U < response_size) {
		ssize_t received = zsock_recv(sock, response + used,
			response_size - 1U - used, 0);

		if (received == 0) {
			break;
		}
		if (received < 0) {
			zsock_close(sock);
			return -errno;
		}
		used += (size_t)received;
	}
	response[used] = '\0';
	zsock_close(sock);
	return (int)used;
}

static int parse_int_field(const char *base, const char *field, int *out)
{
	const char *key = strstr(base, field);
	char *end;

	if (key == NULL) {
		return -EBADMSG;
	}
	key = strchr(key, ':');
	if (key == NULL) {
		return -EBADMSG;
	}
	++key;
	while (*key == ' ' || *key == '"') {
		++key;
	}
	*out = (int)strtol(key, &end, 10);
	return end == key ? -EBADMSG : 0;
}

static int parse_response(const char *json)
{
	const char *current = strstr(json, "\"current_condition\"");
	const char *hourly = strstr(json, "\"hourly\"");
	int value;

	if (current == NULL || parse_int_field(current, "\"temp_C\"", &value) < 0) {
		return -EBADMSG;
	}
	k_mutex_lock(&weather_lock, K_FOREVER);
	snapshot.current_temp = (int8_t)value;
	snapshot.weather_code = 0U;
	if (parse_int_field(current, "\"weatherCode\"", &value) == 0) {
		snapshot.weather_code = (uint8_t)value;
	}
	snapshot.hourly_count = 0U;
	if (hourly != NULL) {
		const char *cursor = hourly;

		for (uint8_t index = 0U; index < 8U; ++index) {
			if (parse_int_field(cursor, "\"tempC\"", &value) < 0) {
				break;
			}
			snapshot.hourly_temp[index] = (int8_t)value;
			snapshot.hourly_count = index + 1U;
			cursor = strstr(cursor, "\"time\"");
			if (cursor == NULL) {
				break;
			}
			cursor = strstr(cursor, "\"tempC\"");
			if (cursor == NULL) {
				break;
			}
		}
	}
	snapshot.updated_at = k_uptime_get() / 1000;
	snapshot.valid = true;
	k_mutex_unlock(&weather_lock);
	return 0;
}

static void weather_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
#if defined(CONFIG_ESP_SPIRAM)
	static char response[6144] __attribute__((section(".ext_ram.bss")));
#else
	static char response[6144];
#endif

	LOG_INF("Weather thread started");
	for (;;) {
		(void)k_sem_take(&weather_request, K_MSEC(WEATHER_POLL_MS));
		if (!atomic_cas(&busy, 0, 1)) {
			continue;
		}
		int ret = -EIO;

		/* Boot starts this thread before DHCP completes; an immediate connect
		 * hits an unconfigured net context. Sleep once, then retry.
		 */
		k_msleep(45000);
		ensure_default_router();
		LOG_INF("wttr.in fetch start (ipv4 ready=%d)", weather_network_ready());
		for (uint8_t attempt = 0U; attempt < 4U; ++attempt) {
			ret = http_get(response, sizeof(response));
			if (ret >= 0) {
				break;
			}
			LOG_INF("wttr.in attempt %u failed (%d)", attempt + 1U, ret);
			k_msleep(10000);
		}
		if (ret < 0) {
			LOG_WRN("wttr.in fetch failed (%d)", ret);
		} else {
			LOG_INF("wttr.in response %d bytes", ret);
			(void)parse_response(response);
		}
		atomic_set(&busy, 0);
		papermono_ui_request_background_refresh();
	}
}

K_THREAD_DEFINE(weather_tid, 2560, weather_thread, NULL, NULL, NULL, 9, 0, 0);

int papermono_weather_init(void)
{
	k_sem_give(&weather_request);
	return 0;
}

void papermono_weather_get(struct papermono_weather *out)
{
	if (out == NULL) {
		return;
	}
	k_mutex_lock(&weather_lock, K_FOREVER);
	*out = snapshot;
	k_mutex_unlock(&weather_lock);
}

int papermono_weather_request(void)
{
	k_sem_give(&weather_request);
	return 0;
}
