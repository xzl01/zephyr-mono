/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_random.h>
#include <esp_wifi.h>

#include <papermono/content_server.h>
#include <papermono/content_store.h>
#include <papermono/ft6336.h>
#include <papermono/http_codec.h>
#include <papermono/organizer.h>
#include <papermono/power.h>
#include <papermono/settings.h>
#include <papermono/status_led.h>
#if defined(CONFIG_PAPERMONO_RSS)
#include <papermono/rss.h>
#endif
#if defined(CONFIG_PAPERMONO_TIME_SYNC)
#include <papermono/time_sync.h>
#endif
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_content_server, LOG_LEVEL_INF);

#define CONTENT_SERVER_PORT 80U
#define AP_ADDRESS "192.168.4.1"
#define AP_NETMASK "255.255.255.0"
#define AP_CHANNEL 6U
#define AP_RETRY_INITIAL_MS 500U
#define AP_RETRY_MAX_MS 30000U
#define AP_RETRY_MAX_SHIFT 6U
/* A configured router may be temporarily unreachable (reboot, channel change,
 * changed password). Keep retrying the saved station with backoff while the
 * provisioning AP stays available, instead of leaving the device off the
 * network with no way back in.
 */
#define STA_RETRY_INITIAL_MS 5000U
#define STA_RETRY_MAX_MS 300000U
#define STA_RETRY_MAX_SHIFT 6U
#define AP_HANDOFF_DELAY_MS 20000U
#define AP_HEALTH_DELAY_MS 750U
#define AP_HEALTH_MAX_FAILURES 3U
#define AP_MAX_TX_POWER_QDBM 78
#define RESPONSE_BUFFER_SIZE 12288U
#define WIFI_REQUEST_LIMIT 256U
/* Reject reason used when the request declared no usable Content-Length. The
 * captured headers are gone by the final callback, so the reason has to carry
 * that fact instead of re-reading the header there.
 */
#define UPLOAD_REJECT_NO_LENGTH (-ENODATA)
/* Reject reason for a request that was not paired. Recorded at the first
 * callback, where the session header still exists.
 */
#define UPLOAD_REJECT_UNAUTHORIZED (-EACCES)

/* Pairing-code throttle. A wrong code rotates the secret, so an attacker who
 * trips the lockout loses every code already tried and has to start over.
 */
#define PAIRING_MAX_ATTEMPTS   5U
#define PAIRING_LOCKOUT_MS     (30U * MSEC_PER_SEC)

/* One-time tickets: the pairing response returns a short-lived ticket rather
 * than the session token, so the long-lived secret never travels in a URL
 * where browser history and sync would retain it.
 */
#define TICKET_SLOT_COUNT      4U
#define TICKET_LENGTH          16U
#define TICKET_TTL_MS          (60U * MSEC_PER_SEC)

enum content_server_busy_bit {
	CONTENT_SERVER_BUSY_UPLOAD,
	CONTENT_SERVER_BUSY_WIFI_REQUEST,
	CONTENT_SERVER_BUSY_RSS_REQUEST,
};

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_SERVER_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_SERVER_BSS
#endif

#if defined(CONFIG_PAPERMONO_RSS)
#define RSS_REQUEST_LIMIT 4096U
/* The Zephyr decoder bounds the escaped JSON token before unescaping it.
 * After Unicode normalization, every stored byte needs at most two wire bytes.
 */
struct rss_source_wire {
	char id[PAPERMONO_RSS_SOURCE_ID_MAX * 2U + 1U];
	char name[PAPERMONO_RSS_SOURCE_NAME_MAX * 2U + 1U];
	char url[PAPERMONO_RSS_SOURCE_URL_MAX * 2U + 1U];
};
struct rss_sources_wire {
	struct rss_source_wire sources[PAPERMONO_RSS_SOURCE_MAX];
	size_t count;
};
static const struct json_obj_descr rss_source_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct rss_source_wire, id, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct rss_source_wire, name, JSON_TOK_STRING_BUF),
	JSON_OBJ_DESCR_PRIM(struct rss_source_wire, url, JSON_TOK_STRING_BUF),
};
static const struct json_obj_descr rss_sources_descr[] = {
	JSON_OBJ_DESCR_OBJ_ARRAY(struct rss_sources_wire, sources,
		PAPERMONO_RSS_SOURCE_MAX, count, rss_source_descr,
		ARRAY_SIZE(rss_source_descr)),
};
/* URLs can carry private feed tokens. Only the paired request owner may
 * retain them, and only until the transaction finishes. Large state is PSRAM.
 */
static struct {
	struct http_client_ctx *owner;
	int fd;
	bool authorized;
	int error;
	int64_t started_ms;
	size_t length;
	char body[RSS_REQUEST_LIMIT + 1U];
	struct rss_sources_wire decoded;
	struct papermono_rss_source sources[PAPERMONO_RSS_SOURCE_MAX];
} rss_request PAPERMONO_SERVER_BSS;
static K_MUTEX_DEFINE(rss_request_lock);
static void rss_request_timeout_worker(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(rss_request_timeout, rss_request_timeout_worker);
#endif

struct wifi_request {
	char *ssid;
	char *psk;
};

static const struct json_obj_descr wifi_request_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct wifi_request, ssid, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct wifi_request, psk, JSON_TOK_STRING),
};

static struct papermono_content_server_state server_state;
static struct net_mgmt_event_callback wifi_callback;
static struct k_work_delayable apply_wifi_work;
static struct k_work_delayable enable_ap_work;
static struct k_work_delayable disable_ap_work;
static struct k_work_delayable ap_health_work;
static struct papermono_content_transaction upload_transaction;
static struct http_client_ctx *upload_owner;
static struct http_client_ctx *upload_rejected_client;
static int upload_reject_reason;
static int64_t upload_started_ms;
static char response_buffer[RESPONSE_BUFFER_SIZE] PAPERMONO_SERVER_BSS;
static uint8_t wifi_request_buffer[WIFI_REQUEST_LIMIT + 1U] PAPERMONO_SERVER_BSS;
static size_t wifi_request_length;
static struct http_client_ctx *wifi_request_owner;
static struct http_client_ctx *wifi_request_rejected_client;
static bool wifi_request_authorized;
static int64_t wifi_started_ms;
/* Absolute deadline for a streamed request body. The HTTP server only closes a
 * connection after it has been idle for the inactivity timeout, and any single
 * received byte restarts that timer, so a client that drips its body can hold
 * the one import slot forever. Our own deadline bounds that.
 */
#define STREAMING_REQUEST_DEADLINE_MS 30000
static char session_token[17];
static bool initialized;
static bool ap_enabling;
static atomic_t wifi_paused;

static bool wifi_is_paused(void)
{
	return atomic_get(&wifi_paused) != 0;
}
static uint8_t ap_retry_shift;
static uint8_t ap_health_failures;
static uint8_t station_retry_shift;
static void station_retry(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(station_retry_work, station_retry);
static atomic_t server_busy;
K_MUTEX_DEFINE(server_lock);

/* ESP32 scan/connect requests may call back synchronously. Never hold
 * server_lock across net_mgmt(), NVS writes, or a driver call. All potentially
 * blocking Wi-Fi operations run here, never on LVGL or the system workqueue.
 */
K_THREAD_STACK_DEFINE(wifi_work_stack, 4096);
static struct k_work_q wifi_work_queue;
static struct papermono_wifi_state wifi_state;
static struct papermono_wifi_network scan_networks[PAPERMONO_WIFI_NETWORK_MAX];
static uint8_t scan_count;
static bool scan_inflight;
static struct k_work scan_work;
static struct k_work_delayable scan_timeout_work;
static struct k_work_delayable wifi_command_work;

enum wifi_command {
	WIFI_COMMAND_NONE,
	WIFI_COMMAND_CONNECT,
	WIFI_COMMAND_SAVED,
	WIFI_COMMAND_FORGET,
	WIFI_COMMAND_RESET,
};

static enum wifi_command pending_wifi_command;
static uint8_t wifi_command_phase;
static int64_t wifi_command_deadline;
static char pending_wifi_ssid[PAPERMONO_WIFI_SSID_MAX + 1U];
static char pending_wifi_psk[PAPERMONO_WIFI_PSK_MAX + 1U];
static void wifi_command_worker(struct k_work *work);
static void scan_wifi_worker(struct k_work *work);
static void scan_timeout_worker(struct k_work *work);

static int wifi_reschedule(struct k_work_delayable *work, k_timeout_t delay)
{
	return k_work_reschedule_for_queue(&wifi_work_queue, work, delay);
}

/* Volatile stores prevent optimization from retaining transient credentials. */
static void wipe_wifi_secret(void *memory, size_t length)
{
	volatile unsigned char *cursor = memory;

	while (length-- > 0U) {
		*cursor++ = 0U;
	}
}

static void content_server_set_busy(enum content_server_busy_bit bit,
	bool busy)
{
	if (busy) {
		atomic_set_bit(&server_busy, bit);
	} else {
		atomic_clear_bit(&server_busy, bit);
	}
	papermono_status_led_request_refresh();
}

/* Pairing throttle state, guarded by server_lock. */
static uint8_t pairing_failures;
static int64_t pairing_lockout_until_ms;

struct ticket_slot {
	char value[TICKET_LENGTH + 1U];
	int64_t expiry_ms;
	bool in_use;
};

static struct ticket_slot ticket_slots[TICKET_SLOT_COUNT];

/* Defined near the end of the file, but referenced by the handlers above. */
static void generate_credentials(void);
static bool secure_equal(const char *left, const char *right, size_t length);
static void random_hex(char *output, size_t length);

HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_session, "X-PaperMono-Session");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_code, "X-PaperMono-Code");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_ticket, "X-PaperMono-Ticket");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_name, "X-PaperMono-Name");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_kind, "X-PaperMono-Kind");
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_crc, "X-PaperMono-CRC32");
/* Zephyr only fills http_client_ctx.content_len on the HTTP/2 path, and this
 * build is HTTP/1 only, so the body length has to be read from the header the
 * client actually sent. Without it the store would be told every upload is
 * zero bytes long.
 */
HTTP_SERVER_REGISTER_HEADER_CAPTURE(capture_length, "Content-Length");

static const uint8_t index_html[] = {
#include "papermono_content_center.html.inc"
};

static struct http_resource_detail_static index_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "text/html; charset=utf-8",
	},
	.static_data = index_html,
	.static_data_len = sizeof(index_html),
};

static const char *header_value(const struct http_request_ctx *request,
	const char *name)
{
	for (size_t index = 0U; index < request->header_count; ++index) {
		if (strlen(request->headers[index].name) == strlen(name) &&
		    strncasecmp(request->headers[index].name, name,
			strlen(name)) == 0) {
			return request->headers[index].value;
		}
	}
	return NULL;
}

/* Ticket helpers. Callers must hold server_lock. */

static void ticket_drop_expired_locked(int64_t now_ms)
{
	for (size_t index = 0U; index < TICKET_SLOT_COUNT; ++index) {
		if (ticket_slots[index].in_use &&
		    now_ms >= ticket_slots[index].expiry_ms) {
			ticket_slots[index].in_use = false;
		}
	}
}

static const char *ticket_issue_locked(int64_t now_ms)
{
	ticket_drop_expired_locked(now_ms);

	struct ticket_slot *slot = NULL;

	for (size_t index = 0U; index < TICKET_SLOT_COUNT; ++index) {
		if (!ticket_slots[index].in_use) {
			slot = &ticket_slots[index];
			break;
		}
	}
	if (slot == NULL) {
		/* All slots busy: recycle the one closest to expiring. */
		slot = &ticket_slots[0];
		for (size_t index = 1U; index < TICKET_SLOT_COUNT; ++index) {
			if (ticket_slots[index].expiry_ms < slot->expiry_ms) {
				slot = &ticket_slots[index];
			}
		}
	}

	random_hex(slot->value, TICKET_LENGTH);
	slot->expiry_ms = now_ms + TICKET_TTL_MS;
	slot->in_use = true;
	return slot->value;
}

/* Redeeming is atomic: a ticket is valid exactly once. */
static bool ticket_redeem_locked(const char *ticket, int64_t now_ms)
{
	if (ticket == NULL || strlen(ticket) != TICKET_LENGTH) {
		return false;
	}
	ticket_drop_expired_locked(now_ms);

	for (size_t index = 0U; index < TICKET_SLOT_COUNT; ++index) {
		struct ticket_slot *slot = &ticket_slots[index];

		if (slot->in_use &&
		    secure_equal(slot->value, ticket, TICKET_LENGTH)) {
			slot->in_use = false;
			return true;
		}
	}
	return false;
}

/* Caller must hold server_lock. */
static bool session_authorized_locked(const struct http_request_ctx *request)
{
	const char *token = header_value(request, "X-PaperMono-Session");

	return server_state.client_paired && token != NULL &&
		strlen(token) == sizeof(session_token) - 1U &&
		secure_equal(token, session_token,
			sizeof(session_token) - 1U);
}

static bool session_authorized(const struct http_request_ctx *request)
{
	bool result;

	k_mutex_lock(&server_lock, K_FOREVER);
	result = session_authorized_locked(request);
	k_mutex_unlock(&server_lock);
	return result;
}

static void response_json(struct http_response_ctx *response,
	enum http_status status, const char *json)
{
	response->status = status;
	response->body = (const uint8_t *)json;
	response->body_len = strlen(json);
	response->final_chunk = true;
}

static int append_json_string(char *buffer, size_t capacity, size_t *length,
	const char *text)
{
	if (*length + 2U >= capacity) {
		return -ENOSPC;
	}
	buffer[(*length)++] = '"';
	for (const unsigned char *cursor = (const unsigned char *)text;
	     *cursor != '\0'; ++cursor) {
		if (*cursor < 0x20U) {
			continue;
		}
		/* Every branch reserves room for the closing quote and the NUL as
		 * well as the bytes it is about to write; the terminator below is
		 * otherwise written one past the end of the buffer.
		 */
		if (*cursor == '"' || *cursor == '\\') {
			if (*length + 4U > capacity) {
				return -ENOSPC;
			}
			buffer[(*length)++] = '\\';
		} else if (*length + 3U > capacity) {
			return -ENOSPC;
		}
		buffer[(*length)++] = (char)*cursor;
	}
	buffer[(*length)++] = '"';
	buffer[*length] = '\0';
	return 0;
}

struct content_list_context {
	size_t length;
	bool first;
};

static int list_entry(const struct papermono_content_entry *entry,
	void *context)
{
	struct content_list_context *list = context;
	const char *kind = entry->kind == PAPERMONO_CONTENT_TXT ? "txt" :
		entry->kind == PAPERMONO_CONTENT_EPUB ? "epub" :
		entry->kind == PAPERMONO_CONTENT_ICS ? "ics" :
		entry->kind == PAPERMONO_CONTENT_TASKS ? "tasks" :
		entry->kind == PAPERMONO_CONTENT_NOTE ? "note" :
		entry->kind == PAPERMONO_CONTENT_WEREAD ? "weread" : "unknown";
	int ret;

	ret = snprintf(response_buffer + list->length,
		sizeof(response_buffer) - list->length,
		"%s{\"id\":\"%s\",\"title\":", list->first ? "" : ",",
		entry->id);
	if (ret < 0 || ret >= (int)(sizeof(response_buffer) - list->length)) {
		return -ENOSPC;
	}
	list->length += (size_t)ret;
	ret = append_json_string(response_buffer, sizeof(response_buffer),
		&list->length, entry->title);
	if (ret < 0) {
		return ret;
	}
	ret = snprintf(response_buffer + list->length,
		sizeof(response_buffer) - list->length,
		",\"kind\":\"%s\",\"size\":%llu,\"modified\":%u}", kind,
		(unsigned long long)entry->size, entry->modified);
	if (ret < 0 || ret >= (int)(sizeof(response_buffer) - list->length)) {
		return -ENOSPC;
	}
	list->length += (size_t)ret;
	list->first = false;
	return 0;
}

static int status_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	struct papermono_content_server_state state;
	struct papermono_ft6336_status touch = { 0 };
	const int touch_ret = papermono_ft6336_get_status(&touch);
	enum papermono_wakeup_cause wake_cause = PAPERMONO_WAKE_OTHER;
	const int wake_ret = papermono_power_get_wakeup_cause(&wake_cause);
	ARG_UNUSED(client);
	ARG_UNUSED(request);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	papermono_content_server_get_state(&state);
	(void)snprintf(response_buffer, sizeof(response_buffer),
		"{\"api\":\"v1\",\"uptime_ms\":%lld,\"media\":%u,"
		"\"wifi\":{\"ap\":%s,\"ap_verified\":%s,\"connected\":%s,"
		"\"station_ipv4\":\"%s\"},"
		"\"touch\":{\"online\":%s,\"pressed\":%s,\"frames\":%u,"
		"\"press_events\":%u,\"release_events\":%u,\"max_poll_gap_ms\":%u,"
		"\"errors\":%u,\"invalid_frames\":%u,\"recoveries\":%u,"
		"\"x\":%d,\"y\":%d,"
		"\"last_sample_ms\":%u},"
		"\"power\":{\"wake_cause\":\"%s\"},"
		"\"content_generation\":%u}", (long long)k_uptime_get(),
		papermono_content_store_media_state(),
		state.ap_enabled ? "true" : "false",
		state.ap_verified ? "true" : "false",
		state.station_connected ? "true" : "false",
		state.station_ipv4,
		touch_ret == 0 && touch.online ? "true" : "false",
		touch_ret == 0 && touch.pressed ? "true" : "false",
		touch.frames, touch.press_events, touch.release_events,
		touch.max_poll_gap_ms, touch.read_errors, touch.invalid_frames,
		touch.recoveries,
		touch.x, touch.y, touch.last_sample_ms,
		wake_ret == 0 ? papermono_wakeup_cause_name(wake_cause) :
			"unavailable",
		state.content_generation);
	response_json(response, HTTP_200_OK, response_buffer);
	return 0;
}

static int state_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	const char *code;
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (session_authorized(request)) {
		const int prefix = snprintf(response_buffer, sizeof(response_buffer),
			"{\"paired\":true,\"state_version\":1,\"overlay\":");
		int state_length = prefix < 0 ? -EINVAL :
			papermono_organizer_export_state_json(response_buffer + prefix,
				sizeof(response_buffer) - (size_t)prefix - 2U);
		if (state_length < 0 || (size_t)prefix + (size_t)state_length + 2U >
		    sizeof(response_buffer)) {
			response_json(response, HTTP_507_INSUFFICIENT_STORAGE,
				"{\"error\":\"state export too large\"}");
			return 0;
		}
		response_buffer[prefix + state_length] = '}';
		response_buffer[prefix + state_length + 1] = '\0';
		response_json(response, HTTP_200_OK, response_buffer);
		return 0;
	}
	code = header_value(request, "X-PaperMono-Code");
	k_mutex_lock(&server_lock, K_FOREVER);

	const int64_t now_ms = k_uptime_get();

	/* Reject everything while locked out, then let the window expire. */
	if (pairing_lockout_until_ms != 0 && now_ms < pairing_lockout_until_ms) {
		const int64_t remaining_ms =
			pairing_lockout_until_ms - now_ms;

		k_mutex_unlock(&server_lock);
		(void)snprintf(response_buffer, sizeof(response_buffer),
			"{\"error\":\"too many attempts\","
			"\"retry_after_ms\":%lld}",
			(long long)remaining_ms);
		response_json(response, HTTP_429_TOO_MANY_REQUESTS,
			response_buffer);
		return 0;
	}
	if (pairing_lockout_until_ms != 0 &&
	    now_ms >= pairing_lockout_until_ms) {
		pairing_lockout_until_ms = 0;
		pairing_failures = 0U;
	}

	const size_t code_length = strlen(server_state.pairing_code);
	const bool matches = code != NULL && !server_state.client_paired &&
		strlen(code) == code_length &&
		secure_equal(code, server_state.pairing_code, code_length);

	if (!matches) {
		++pairing_failures;
		if (pairing_failures >= PAIRING_MAX_ATTEMPTS) {
			/* Rotate the secret so attempts made before the
			 * lockout are worthless, and drop any issued ticket.
			 */
			generate_credentials();
			pairing_failures = 0U;
			pairing_lockout_until_ms = now_ms + PAIRING_LOCKOUT_MS;
			(void)memset(ticket_slots, 0, sizeof(ticket_slots));
			LOG_WRN("Pairing locked out; code rotated");
			k_mutex_unlock(&server_lock);
			/* The e-paper screen still shows the invalidated code. Without
			 * this redraw the operator cannot pair at all until something
			 * else triggers a repaint, while an attacker keeps the lockout
			 * alive by sending bad codes every window.
			 */
			papermono_ui_request_background_refresh();
			(void)snprintf(response_buffer,
				sizeof(response_buffer),
				"{\"error\":\"too many attempts\","
				"\"code_rotated\":true,"
				"\"retry_after_ms\":%d}",
				(int)PAIRING_LOCKOUT_MS);
			response_json(response, HTTP_429_TOO_MANY_REQUESTS,
				response_buffer);
			return 0;
		}
		k_mutex_unlock(&server_lock);
		response_json(response, HTTP_401_UNAUTHORIZED,
			"{\"error\":\"invalid or consumed pairing code\"}");
		return 0;
	}

	/* Correct code: hand back a one-time ticket, never the session token.
	 * The operator redeems it over a POST body instead of a URL.
	 *
	 * client_paired is deliberately NOT set here. Pairing used to be latched
	 * at this point, before the ticket was redeemed, so if the browser never
	 * completed POST /api/v1/session (tab closed, Wi-Fi drop, TTL expiry) the
	 * code path was closed, the token had never been disclosed, and every
	 * authenticated endpoint returned 401 until a reboot or a manual Wi-Fi
	 * reset. The code stays usable until a session actually exists; tickets
	 * are still single-use with a short TTL, so this grants nothing extra.
	 */
	pairing_failures = 0U;
	pairing_lockout_until_ms = 0;
	const char *ticket = ticket_issue_locked(now_ms);

	(void)snprintf(response_buffer, sizeof(response_buffer),
		"{\"paired\":true,\"ticket\":\"%s\",\"state_version\":1}",
		ticket);
	k_mutex_unlock(&server_lock);
	response_json(response, HTTP_200_OK, response_buffer);
	papermono_ui_request_background_refresh();
	return 0;
}

static int content_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (!session_authorized(request)) {
		response_json(response, HTTP_401_UNAUTHORIZED,
			"{\"error\":\"pairing required\"}");
		return 0;
	}
	struct content_list_context list = { .length = 1U, .first = true };
	response_buffer[0] = '[';
	response_buffer[1] = '\0';
	int ret = papermono_content_store_scan_all(list_entry, &list, NULL);
	if (ret == -ENOSPC) {
		response_json(response, HTTP_507_INSUFFICIENT_STORAGE,
			"{\"error\":\"content index too large\"}");
		return 0;
	}
	if (ret < 0) {
		(void)snprintf(response_buffer, sizeof(response_buffer),
			"{\"error\":\"content unavailable\",\"code\":%d}", ret);
		response_json(response, HTTP_503_SERVICE_UNAVAILABLE,
			response_buffer);
		return 0;
	}
	if (list.length + 2U > sizeof(response_buffer)) {
		response_json(response, HTTP_507_INSUFFICIENT_STORAGE,
			"{\"error\":\"content index too large\"}");
		return 0;
	}
	response_buffer[list.length++] = ']';
	response_buffer[list.length] = '\0';
	response_json(response, HTTP_200_OK, response_buffer);
	return 0;
}

static enum papermono_content_kind parse_kind(const char *kind)
{
	if (kind == NULL) {
		return PAPERMONO_CONTENT_UNKNOWN;
	}
	if (strcmp(kind, "txt") == 0) {
		return PAPERMONO_CONTENT_TXT;
	}
	if (strcmp(kind, "epub") == 0) {
		return PAPERMONO_CONTENT_EPUB;
	}
	if (strcmp(kind, "ics") == 0) {
		return PAPERMONO_CONTENT_ICS;
	}
	if (strcmp(kind, "tasks") == 0) {
		return PAPERMONO_CONTENT_TASKS;
	}
	if (strcmp(kind, "note") == 0) {
		return PAPERMONO_CONTENT_NOTE;
	}
	if (strcmp(kind, "weread") == 0) {
		return PAPERMONO_CONTENT_WEREAD;
	}
	return PAPERMONO_CONTENT_UNKNOWN;
}

static int decode_content_name(const char *encoded, char *name,
	size_t capacity)
{
	const int ret = papermono_http_decode_name(encoded, name, capacity);

	return ret < 0 ? ret :
		papermono_content_store_safe_name(name) ? 0 : -EINVAL;
}

static int upload_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	ARG_UNUSED(user_data);
	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		if (client == upload_owner && upload_transaction.active) {
			(void)papermono_content_store_import_abort(&upload_transaction);
		}
		if (client == upload_owner) {
			upload_owner = NULL;
			content_server_set_busy(CONTENT_SERVER_BUSY_UPLOAD, false);
		}
		if (client == upload_rejected_client) {
			upload_rejected_client = NULL;
			upload_reject_reason = 0;
		}
		return 0;
	}
	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (client == upload_owner) {
			upload_owner = NULL;
			content_server_set_busy(CONTENT_SERVER_BUSY_UPLOAD, false);
		}
		if (client == upload_rejected_client) {
			upload_rejected_client = NULL;
			upload_reject_reason = 0;
		}
		return 0;
	}
	if (upload_owner != NULL && upload_owner != client) {
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_409_CONFLICT,
				"{\"error\":\"another upload is active\"}");
		}
		return 0;
	}
	if (upload_owner == client && upload_transaction.active &&
	    k_uptime_get() - upload_started_ms > STREAMING_REQUEST_DEADLINE_MS) {
		LOG_WRN("Upload exceeded %d ms; aborting the transaction",
			STREAMING_REQUEST_DEADLINE_MS);
		(void)papermono_content_store_import_abort(&upload_transaction);
		upload_owner = NULL;
		content_server_set_busy(CONTENT_SERVER_BUSY_UPLOAD, false);
		upload_rejected_client = client;
		upload_reject_reason = -ETIMEDOUT;
	}
	if (!upload_transaction.active && upload_owner == NULL &&
	    upload_rejected_client != client) {
		const char *encoded_name = header_value(request, "X-PaperMono-Name");
		const char *kind_text = header_value(request, "X-PaperMono-Kind");
		const char *crc_text = header_value(request, "X-PaperMono-CRC32");
		const char *length_text = header_value(request, "Content-Length");
		uint32_t crc = 0U;
		size_t declared_length = 0U;
		bool verify_crc = false;
		char name[MAX_FILE_NAME + 1U];

		if (!session_authorized(request)) {
			upload_rejected_client = client;
			upload_reject_reason = UPLOAD_REJECT_UNAUTHORIZED;
		} else {
			/* The declared length is the only size the store will ever
			 * see, so an unreadable or absent one must not be treated as
			 * "empty but acceptable": that is what let a zero-length body
			 * overwrite an existing file with nothing.
			 */
			const int length_ret =
				papermono_http_parse_content_length(length_text,
					&declared_length);
			int name_ret = decode_content_name(encoded_name, name,
				sizeof(name));
			if (crc_text != NULL) {
				verify_crc = papermono_http_parse_crc32(crc_text, &crc);
			}
			/* Every importable kind carries content. Committing a zero-byte
			 * body would replace a real file with an empty one, so an empty
			 * body is refused rather than passed to the text validators,
			 * which accept it (papermono_text_detect() and
			 * papermono_parse_ics() both treat "" as valid).
			 */
			int ret = length_ret == -ERANGE ? length_ret :
				length_ret < 0 ? UPLOAD_REJECT_NO_LENGTH :
				declared_length == 0U ? UPLOAD_REJECT_NO_LENGTH :
				name_ret < 0 ? name_ret :
				crc_text != NULL && !verify_crc ? -EINVAL :
				papermono_content_store_import_begin(
					&upload_transaction, parse_kind(kind_text), name,
					declared_length, verify_crc, crc);
			if (ret < 0) {
				upload_rejected_client = client;
				upload_reject_reason = ret;
			} else {
				upload_owner = client;
				upload_started_ms = k_uptime_get();
				content_server_set_busy(CONTENT_SERVER_BUSY_UPLOAD, true);
			}
		}
	}
	if (upload_owner == client && upload_transaction.active &&
	    request->data_len > 0U) {
		int ret = papermono_content_store_import_write(&upload_transaction,
			request->data, request->data_len);
		if (ret < 0) {
			(void)papermono_content_store_import_abort(&upload_transaction);
			upload_owner = NULL;
			content_server_set_busy(CONTENT_SERVER_BUSY_UPLOAD, false);
			upload_rejected_client = client;
			upload_reject_reason = ret;
		}
	}
	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		if (upload_rejected_client == client || upload_owner != client ||
		    !upload_transaction.active) {
			/* session_authorized() cannot be asked here: the captured
			 * headers are gone by the final callback, so it would report a
			 * paired client as unpaired. The decision was made when the
			 * request started and is reflected in upload_rejected_client.
			 */
			if (upload_reject_reason == UPLOAD_REJECT_UNAUTHORIZED) {
				response_json(response, HTTP_401_UNAUTHORIZED,
					"{\"error\":\"pairing required\"}");
			} else if (upload_reject_reason == -ERANGE) {
				response_json(response, HTTP_413_PAYLOAD_TOO_LARGE,
					"{\"error\":\"body too large\"}");
			} else if (upload_reject_reason == UPLOAD_REJECT_NO_LENGTH) {
				response_json(response, HTTP_411_LENGTH_REQUIRED,
					"{\"error\":\"content-length required\"}");
			} else if (upload_reject_reason == -ETIMEDOUT) {
				response_json(response, HTTP_408_REQUEST_TIMEOUT,
					"{\"error\":\"upload timed out\"}");
			} else {
				(void)snprintf(response_buffer, sizeof(response_buffer),
					"{\"error\":\"upload rejected\",\"code\":%d}",
					upload_reject_reason);
				response_json(response, HTTP_400_BAD_REQUEST,
					response_buffer);
			}
			return 0;
		}
		int ret = papermono_content_store_import_commit(&upload_transaction);
		upload_owner = NULL;
		content_server_set_busy(CONTENT_SERVER_BUSY_UPLOAD, false);
		if (ret < 0) {
			(void)snprintf(response_buffer, sizeof(response_buffer),
				"{\"error\":\"commit failed\",\"code\":%d}", ret);
			response_json(response, HTTP_400_BAD_REQUEST, response_buffer);
			return 0;
		}
		k_mutex_lock(&server_lock, K_FOREVER);
		++server_state.content_generation;
		k_mutex_unlock(&server_lock);
		response_json(response, HTTP_201_CREATED, "{\"ok\":true}");
		papermono_ui_request_content_refresh();
	}
	return 0;
}

struct find_content_context {
	const char *id;
	struct papermono_content_entry entry;
	bool found;
	/* The content ID is a 64-bit FNV-1a digest, so two different files can
	 * in principle collide. Deleting the first match would then remove the
	 * wrong book, so an ambiguous ID is refused instead.
	 */
	unsigned int matches;
};

static int find_entry(const struct papermono_content_entry *entry,
	void *context)
{
	struct find_content_context *find = context;

	if (strcmp(entry->id, find->id) == 0) {
		if (find->matches == 0U) {
			find->entry = *entry;
			find->found = true;
		}
		++find->matches;
		if (find->matches > 1U) {
			/* Stop scanning: the answer is already "ambiguous". */
			return 1;
		}
	}
	return 0;
}

static int delete_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	ARG_UNUSED(user_data);
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	if (!session_authorized(request)) {
		response_json(response, HTTP_401_UNAUTHORIZED,
			"{\"error\":\"pairing required\"}");
		return 0;
	}
	const char *id = strrchr((const char *)client->url_buffer, '/');
	struct find_content_context find = { .id = id == NULL ? "" : id + 1U };
	(void)papermono_content_store_scan_all(find_entry, &find, NULL);
	if (!find.found) {
		response_json(response, HTTP_404_NOT_FOUND,
			"{\"error\":\"content not found\"}");
		return 0;
	}
	if (find.matches > 1U) {
		LOG_ERR("Refusing DELETE: content id %s matches %u entries",
			find.id, find.matches);
		response_json(response, HTTP_409_CONFLICT,
			"{\"error\":\"ambiguous content id\"}");
		return 0;
	}
	int ret = papermono_content_store_delete(&find.entry);
	if (ret < 0) {
		response_json(response, HTTP_500_INTERNAL_SERVER_ERROR,
			"{\"error\":\"delete failed\"}");
		return 0;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	++server_state.content_generation;
	k_mutex_unlock(&server_lock);
	response_json(response, HTTP_200_OK, "{\"ok\":true}");
	papermono_ui_request_content_refresh();
	return 0;
}

static int connect_station(void);

static void schedule_station_retry(void)
{
	uint8_t shift;

	if (wifi_is_paused()) {
		return;
	}
	uint32_t delay_ms;

	k_mutex_lock(&server_lock, K_FOREVER);
	if (server_state.station_connected || server_state.station_connecting ||
	    wifi_state.request_busy || scan_inflight ||
	    !papermono_settings_get()->wifi_configured) {
		k_mutex_unlock(&server_lock);
		return;
	}
	shift = station_retry_shift;
	if (station_retry_shift < STA_RETRY_MAX_SHIFT) {
		++station_retry_shift;
	}
	k_mutex_unlock(&server_lock);
	delay_ms = MIN(STA_RETRY_INITIAL_MS << shift, STA_RETRY_MAX_MS);
	(void)wifi_reschedule(&station_retry_work, K_MSEC(delay_ms));
	LOG_INF("Wi-Fi station retry scheduled in %u ms", delay_ms);
}

static void station_retry(struct k_work *work)
{
	ARG_UNUSED(work);
	if (wifi_is_paused()) {
		return;
	}

	if (!papermono_settings_get()->wifi_configured) {
		return;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	const bool busy = server_state.station_connected ||
		server_state.station_connecting || wifi_state.request_busy || scan_inflight;
	k_mutex_unlock(&server_lock);
	if (busy) {
		return;
	}
	const int ret = connect_station();

	if (ret < 0) {
		LOG_WRN("Wi-Fi station retry could not start (%d)", ret);
		schedule_station_retry();
	}
}

static void apply_wifi(struct k_work *work);
static void disable_ap(struct k_work *work);

static int wifi_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	ARG_UNUSED(user_data);
	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (wifi_request_owner == client) {
			wipe_wifi_secret(wifi_request_buffer, sizeof(wifi_request_buffer));
			wifi_request_length = 0U;
			wifi_request_owner = NULL;
			wifi_request_authorized = false;
			content_server_set_busy(CONTENT_SERVER_BUSY_WIFI_REQUEST,
				false);
		}
		if (wifi_request_rejected_client == client) {
			/* Only the client pointer is cleared here. The authorization
			 * latch belongs to the request, and a later request re-decides
			 * it on the callback that carries the headers.
			 */
			wifi_request_rejected_client = NULL;
		}
		return 0;
	}
	if (wifi_request_rejected_client == client) {
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_413_PAYLOAD_TOO_LARGE,
				"{\"error\":\"request too large\"}");
		}
		return 0;
	}
	if (wifi_request_owner != NULL && wifi_request_owner != client) {
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_409_CONFLICT,
				"{\"error\":\"another wifi request is active\"}");
		}
		return 0;
	}
	/* Zephyr publishes the captured headers only in the first callback of a
	 * request and then clears them, so session_authorized() cannot be asked
	 * again once the body starts arriving: it would report a legitimate
	 * request as unpaired and drop its body. Decide once, on the callback
	 * that carries the headers, and remember the answer for the request.
	 */
	if (request->headers_status != HTTP_HEADER_STATUS_NONE) {
		wifi_request_authorized = session_authorized(request);
	}
	if (!wifi_request_authorized) {
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_401_UNAUTHORIZED,
				"{\"error\":\"pairing required\"}");
		}
		return 0;
	}
	if (wifi_request_owner == client &&
	    k_uptime_get() - wifi_started_ms > STREAMING_REQUEST_DEADLINE_MS) {
		LOG_WRN("Wi-Fi request exceeded %d ms; aborting",
			STREAMING_REQUEST_DEADLINE_MS);
		wifi_request_length = 0U;
		wifi_request_owner = NULL;
		content_server_set_busy(CONTENT_SERVER_BUSY_WIFI_REQUEST, false);
		wifi_request_rejected_client = client;
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_408_REQUEST_TIMEOUT,
				"{\"error\":\"request timed out\"}");
		}
		return 0;
	}
	if (wifi_request_owner == NULL) {
		wifi_request_owner = client;
		wifi_request_length = 0U;
		wifi_started_ms = k_uptime_get();
		content_server_set_busy(CONTENT_SERVER_BUSY_WIFI_REQUEST, true);
	}
	if (wifi_request_length + request->data_len > WIFI_REQUEST_LIMIT) {
		wifi_request_length = 0U;
		wifi_request_owner = NULL;
		content_server_set_busy(CONTENT_SERVER_BUSY_WIFI_REQUEST, false);
		wifi_request_rejected_client = client;
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_413_PAYLOAD_TOO_LARGE,
				"{\"error\":\"request too large\"}");
		}
		return 0;
	}
	memcpy(wifi_request_buffer + wifi_request_length, request->data,
		request->data_len);
	wifi_request_length += request->data_len;
	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		struct wifi_request request_data = { 0 };
		wifi_request_buffer[wifi_request_length] = '\0';
		int parsed = json_obj_parse(wifi_request_buffer, wifi_request_length,
			wifi_request_descr, ARRAY_SIZE(wifi_request_descr), &request_data);
		struct papermono_wifi_network network = { .supported = true };
		int ret = -EINVAL;

		if (parsed == BIT_MASK(ARRAY_SIZE(wifi_request_descr)) &&
		    request_data.ssid != NULL && request_data.psk != NULL) {
			const size_t ssid_length = strnlen(request_data.ssid,
				sizeof(network.ssid));

			if (ssid_length > 0U && ssid_length <= PAPERMONO_WIFI_SSID_LENGTH) {
				network.ssid_length = ssid_length;
				memcpy(network.ssid, request_data.ssid, ssid_length);
				network.security = request_data.psk[0] == '\0' ?
					PAPERMONO_WIFI_SECURITY_OPEN : PAPERMONO_WIFI_SECURITY_WPA2_PSK;
				ret = papermono_content_server_connect_wifi(&network, request_data.psk);
			}
		}
		wipe_wifi_secret(wifi_request_buffer, sizeof(wifi_request_buffer));
		wifi_request_length = 0U;
		wifi_request_owner = NULL;
		content_server_set_busy(CONTENT_SERVER_BUSY_WIFI_REQUEST, false);
		if (ret < 0) {
			response_json(response, ret == -EBUSY ? HTTP_409_CONFLICT : HTTP_400_BAD_REQUEST,
				ret == -EBUSY ? "{\"error\":\"wifi operation busy\"}" :
				"{\"error\":\"invalid wifi credentials\"}");
			return 0;
		}
		response_json(response, HTTP_202_ACCEPTED, "{\"ok\":true}");
	}
	return 0;
}

/* An authenticated AP-origin session can request a short-lived handoff ticket.
 * The LAN origin redeems that ticket for the same session token. The long-lived
 * token is returned in a response body and never travels in a URL.
 */
static int session_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) {
		return 0;
	}
	const char *ticket = header_value(request, "X-PaperMono-Ticket");
	const int64_t now_ms = k_uptime_get();

	k_mutex_lock(&server_lock, K_FOREVER);
	if (session_authorized_locked(request)) {
		const char *issued = ticket_issue_locked(now_ms);

		(void)snprintf(response_buffer, sizeof(response_buffer),
			"{\"ticket\":\"%s\",\"expires_in_ms\":%u}",
			issued, (unsigned int)TICKET_TTL_MS);
		k_mutex_unlock(&server_lock);
		response_json(response, HTTP_200_OK, response_buffer);
		return 0;
	}
	if (!ticket_redeem_locked(ticket, now_ms)) {
		k_mutex_unlock(&server_lock);
		response_json(response, HTTP_401_UNAUTHORIZED,
			"{\"error\":\"invalid or expired ticket\"}");
		return 0;
	}
	/* The code is consumed here, at the moment a real session exists, rather
	 * than when the ticket was issued. A ticket can only be obtained by
	 * presenting the pairing code, so redeeming one is itself evidence that
	 * the code was known.
	 */
	server_state.client_paired = true;
	(void)snprintf(response_buffer, sizeof(response_buffer),
		"{\"session\":\"%s\"}", session_token);
	k_mutex_unlock(&server_lock);
	response_json(response, HTTP_200_OK, response_buffer);
	return 0;
}

#if defined(CONFIG_PAPERMONO_RSS)
static void rss_request_clear(void)
{
	(void)k_work_cancel_delayable(&rss_request_timeout);
	memset(&rss_request, 0, sizeof(rss_request));
	content_server_set_busy(CONTENT_SERVER_BUSY_RSS_REQUEST, false);
}

static void rss_request_timeout_worker(struct k_work *work)
{
	ARG_UNUSED(work);
	/* FINAL may hold the request lock while it saves the SD registry. Keep
	 * this short deadline callback from blocking the system work queue.
	 */
	if (k_mutex_lock(&rss_request_lock, K_NO_WAIT) != 0) {
		(void)k_work_reschedule(&rss_request_timeout, K_MSEC(100));
		return;
	}
	if (rss_request.owner != NULL) {
		const int64_t remaining = rss_request.started_ms +
			STREAMING_REQUEST_DEADLINE_MS - k_uptime_get();

		if (remaining > 0) {
			/* A canceled invocation may have been queued before a fresh
			 * owner started. Only its current deadline can close it.
			 */
			(void)k_work_reschedule(&rss_request_timeout, K_MSEC(remaining));
		} else {
			/* Zephyr invokes ABORT before closing/recycling the client
			 * fd. ABORT takes this same lock, so the captured fd cannot
			 * be reused until shutdown finishes. poll then performs the
			 * ordinary transaction cleanup on the HTTP server thread.
			 */
			(void)zsock_shutdown(rss_request.fd, ZSOCK_SHUT_RD);
		}
	}
	k_mutex_unlock(&rss_request_lock);
}

static int rss_json_hex4(const char *text, uint32_t *value)
{
	*value = 0U;
	for (size_t index = 0U; index < 4U; ++index) {
		const unsigned char ch = (unsigned char)text[index];
		const int digit = ch >= '0' && ch <= '9' ? ch - '0' :
			ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
			ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
		if (digit < 0) return -EINVAL;
		*value = (*value << 4) | (uint32_t)digit;
	}
	return 0;
}

/* This Zephyr JSON decoder handles ordinary escapes but leaves \uXXXX
 * literal. Normalize Unicode escapes in-place before the bounded decoder;
 * the result always fits, and source fields cannot contain NUL/control bytes.
 */
static int rss_json_normalize(char *text, size_t *length)
{
	size_t output = 0U;
	bool quoted = false;

	for (size_t input = 0U; input < *length;) {
		const char ch = text[input++];
		if (ch == '\0') return -EINVAL;
		if (ch == '"') quoted = !quoted;
		if (!quoted || ch != '\\') {
			text[output++] = ch;
			continue;
		}
		if (input >= *length) return -EINVAL;
		if (text[input] != 'u') {
			text[output++] = ch;
			text[output++] = text[input++];
			continue;
		}
		if (*length - input < 5U) return -EINVAL;
		uint32_t value;
		if (rss_json_hex4(text + input + 1U, &value) < 0) return -EINVAL;
		input += 5U;
		if (value >= 0xd800U && value <= 0xdbffU) {
			uint32_t low;
			if (*length - input < 6U || text[input] != '\\' ||
			    text[input + 1U] != 'u' ||
			    rss_json_hex4(text + input + 2U, &low) < 0 ||
			    low < 0xdc00U || low > 0xdfffU) return -EINVAL;
			value = 0x10000U + ((value - 0xd800U) << 10) + low - 0xdc00U;
			input += 6U;
		} else if (value >= 0xdc00U && value <= 0xdfffU) {
			return -EINVAL;
		}
		if (value < 0x20U || value == 0x7fU) return -EINVAL;
		if (value == '"' || value == '\\') text[output++] = '\\';
		if (value < 0x80U) {
			text[output++] = (char)value;
		} else if (value < 0x800U) {
			text[output++] = (char)(0xc0U | (value >> 6));
			text[output++] = (char)(0x80U | (value & 0x3fU));
		} else if (value < 0x10000U) {
			text[output++] = (char)(0xe0U | (value >> 12));
			text[output++] = (char)(0x80U | ((value >> 6) & 0x3fU));
			text[output++] = (char)(0x80U | (value & 0x3fU));
		} else {
			text[output++] = (char)(0xf0U | (value >> 18));
			text[output++] = (char)(0x80U | ((value >> 12) & 0x3fU));
			text[output++] = (char)(0x80U | ((value >> 6) & 0x3fU));
			text[output++] = (char)(0x80U | (value & 0x3fU));
		}
	}
	text[output] = '\0';
	*length = output;
	return quoted ? -EINVAL : 0;
}

static int rss_sources_response(struct http_response_ctx *response)
{
	const uint32_t generation = papermono_rss_sources_generation();
	const size_t count = papermono_rss_source_count();
	size_t used = (size_t)snprintf(response_buffer, sizeof(response_buffer),
		"{\"max_sources\":%u,\"generation\":%u,\"sources\":[",
		PAPERMONO_RSS_SOURCE_MAX, generation);

	for (size_t index = 0U; index < count; ++index) {
		struct papermono_rss_source *source = &rss_request.sources[0];
		if (papermono_rss_source_get(index, source) < 0) {
			return -EAGAIN;
		}
		const char *keys[] = {"id", "name", "url"};
		const char *values[] = {source->id, source->name, source->url};

		response_buffer[used++] = index == 0U ? '{' : ',';
		if (index != 0U) response_buffer[used++] = '{';
		for (size_t field = 0U; field < ARRAY_SIZE(keys); ++field) {
			const int prefix = snprintf(response_buffer + used,
				sizeof(response_buffer) - used, "%s\"%s\":",
				field == 0U ? "" : ",", keys[field]);
			if (prefix < 0 || (size_t)prefix >= sizeof(response_buffer) - used) {
				return -ENOSPC;
			}
			used += (size_t)prefix;
			const int ret = append_json_string(response_buffer,
				sizeof(response_buffer), &used, values[field]);
			if (ret < 0) return ret;
		}
		if (used + 4U >= sizeof(response_buffer)) return -ENOSPC;
		response_buffer[used++] = '}';
	}
	if (generation != papermono_rss_sources_generation()) return -EAGAIN;
	response_buffer[used++] = ']';
	response_buffer[used++] = '}';
	response_buffer[used] = '\0';
	response_json(response, HTTP_200_OK, response_buffer);
	return 0;
}

static int rss_sources_decode(void)
{
	for (size_t index = 0U; index < rss_request.decoded.count; ++index) {
		const struct rss_source_wire *wire = &rss_request.decoded.sources[index];
		struct papermono_rss_source *source = &rss_request.sources[index];
		const size_t id_length = strlen(wire->id);
		const size_t name_length = strlen(wire->name);
		const size_t url_length = strlen(wire->url);

		if (id_length >= sizeof(source->id) ||
		    name_length >= sizeof(source->name) ||
		    url_length >= sizeof(source->url)) return -EINVAL;
		memcpy(source->id, wire->id, id_length + 1U);
		memcpy(source->name, wire->name, name_length + 1U);
		memcpy(source->url, wire->url, url_length + 1U);
	}
	return 0;
}

static int rss_sources_request(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	ARG_UNUSED(user_data);
	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		if (rss_request.owner == client) rss_request_clear();
		return 0;
	}
	if (rss_request.owner != NULL &&
	    k_uptime_get() - rss_request.started_ms >= STREAMING_REQUEST_DEADLINE_MS) {
		rss_request.error = -ETIMEDOUT;
	}
	if (rss_request.owner != NULL && rss_request.owner != client) {
		if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
			response_json(response, HTTP_409_CONFLICT,
				"{\"error\":\"subscriptions_busy\",\"message\":\"另一个订阅操作正在进行，请稍后重试\"}");
		}
		return 0;
	}
	if (rss_request.owner == NULL) {
		/* Captured headers exist only on the first callback. Bind this
		 * authorization to its client before accepting subsequent chunks.
		 */
		if (request->headers_status == HTTP_HEADER_STATUS_NONE ||
		    !session_authorized(request)) {
			if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
				response_json(response, HTTP_401_UNAUTHORIZED,
					"{\"error\":\"pairing required\"}");
			}
			return status == HTTP_SERVER_REQUEST_DATA_FINAL ? 0 : -EACCES;
		}
		rss_request.owner = client;
		rss_request.fd = client->fd;
		rss_request.authorized = true;
		rss_request.started_ms = k_uptime_get();
		(void)k_work_reschedule(&rss_request_timeout,
			K_MSEC(STREAMING_REQUEST_DEADLINE_MS));
		content_server_set_busy(CONTENT_SERVER_BUSY_RSS_REQUEST, true);
	}
	if (request->data_len > RSS_REQUEST_LIMIT - rss_request.length) {
		rss_request.error = -EFBIG;
	}
	if (rss_request.error != 0) {
		const int error = rss_request.error;
		/* An incomplete rejected request must close, so Zephyr releases its
		 * holder and delivers ABORT before this client slot can be reused.
		 */
		if (status != HTTP_SERVER_REQUEST_DATA_FINAL) return error;
		response_json(response, error == -EFBIG ?
			HTTP_413_PAYLOAD_TOO_LARGE : HTTP_408_REQUEST_TIMEOUT,
			"{\"error\":\"subscriptions_request_rejected\",\"message\":\"请求过大或已超时，请重新保存\"}");
		rss_request_clear();
		return 0;
	}
	if (rss_request.error == 0 && request->data_len > 0U) {
		memcpy(rss_request.body + rss_request.length, request->data,
			request->data_len);
		rss_request.length += request->data_len;
	}
	if (status != HTTP_SERVER_REQUEST_DATA_FINAL) return 0;
	if (client->method == HTTP_GET) {
		if (rss_sources_response(response) < 0) {
			response_json(response, HTTP_409_CONFLICT,
				"{\"error\":\"subscriptions_changed\",\"message\":\"订阅列表已变化，请重新加载\"}");
		}
	} else {
		rss_request.body[rss_request.length] = '\0';
		memset(&rss_request.decoded, 0, sizeof(rss_request.decoded));
		const int64_t parsed = rss_json_normalize(rss_request.body,
			&rss_request.length) < 0 ? -EINVAL :
			json_obj_parse(rss_request.body, rss_request.length,
				rss_sources_descr, ARRAY_SIZE(rss_sources_descr),
				&rss_request.decoded);
		int ret = -EINVAL;
		if (parsed == BIT_MASK(ARRAY_SIZE(rss_sources_descr)) &&
		    rss_request.decoded.count <= PAPERMONO_RSS_SOURCE_MAX &&
		    rss_sources_decode() == 0) {
			ret = papermono_rss_sources_replace(rss_request.sources,
				rss_request.decoded.count);
		}
		if (ret == 0) {
			response_json(response, HTTP_200_OK, "{\"ok\":true}");
		} else {
			const bool unavailable = ret == -ENODEV || ret == -EIO || ret == -ENOSPC;
			response_json(response, unavailable ? HTTP_503_SERVICE_UNAVAILABLE :
				HTTP_400_BAD_REQUEST, unavailable ?
				"{\"error\":\"subscriptions_storage\",\"message\":\"TF 存储未就绪或无法写入，原订阅已保留\"}" :
				"{\"error\":\"subscriptions_invalid\",\"message\":\"请检查名称、http:// 地址、重复订阅和数量限制\"}");
		}
	}
	/* Do not retain feed tokens in the HTTP request buffer. */
	rss_request_clear();
	return 0;
}

static int rss_sources_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	k_mutex_lock(&rss_request_lock, K_FOREVER);
	const int ret = rss_sources_request(client, status, request, response, user_data);
	k_mutex_unlock(&rss_request_lock);
	return ret;
}
#endif

static struct http_resource_detail_dynamic status_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json" },
	.cb = status_handler,
};
static struct http_resource_detail_dynamic session_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "application/json" },
	.cb = session_handler,
};
static struct http_resource_detail_dynamic state_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json" },
	.cb = state_handler,
};
static int delete_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data);

static int content_endpoint_handler(struct http_client_ctx *client,
	enum http_transaction_status status,
	const struct http_request_ctx *request,
	struct http_response_ctx *response, void *user_data)
{
	if (client->method == HTTP_GET) {
		return content_handler(client, status, request, response, user_data);
	}
	/* The server matches "/api/v1/content" as a leading directory of
	 * "/api/v1/content/<id>", so DELETE requests can arrive on this exact
	 * resource instead of the wildcard one.
	 */
	if (client->method == HTTP_DELETE) {
		return delete_handler(client, status, request, response, user_data);
	}
	return upload_handler(client, status, request, response, user_data);
}

static struct http_resource_detail_dynamic content_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods =
			BIT(HTTP_GET) | BIT(HTTP_POST) | BIT(HTTP_DELETE),
		.content_type = "application/json" },
	.cb = content_endpoint_handler,
};
static struct http_resource_detail_dynamic delete_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_DELETE),
		.content_type = "application/json" },
	.cb = delete_handler,
};
static struct http_resource_detail_dynamic wifi_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "application/json" },
	.cb = wifi_handler,
};
#if defined(CONFIG_PAPERMONO_RSS)
static struct http_resource_detail_dynamic rss_sources_resource_detail = {
	.common = { .type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_PUT),
		.content_type = "application/json" },
	.cb = rss_sources_handler,
};
#endif

static uint16_t content_server_port = CONTENT_SERVER_PORT;
HTTP_SERVICE_DEFINE(papermono_http, NULL, &content_server_port,
	CONFIG_HTTP_SERVER_MAX_CLIENTS, 2, NULL, NULL, NULL);
HTTP_RESOURCE_DEFINE(index_resource, papermono_http, "/",
	&index_resource_detail);
HTTP_RESOURCE_DEFINE(status_resource, papermono_http, "/api/v1/status",
	&status_resource_detail);
HTTP_RESOURCE_DEFINE(state_resource, papermono_http, "/api/v1/state",
	&state_resource_detail);
HTTP_RESOURCE_DEFINE(session_resource, papermono_http, "/api/v1/session",
	&session_resource_detail);
/* The HTTP server matches wildcard resources with FNM_LEADING_DIR, so the
 * exact "/api/v1/content" resource also matches "/api/v1/content/<id>". It
 * must be registered after the delete wildcard, otherwise a DELETE is routed
 * to the GET/POST resource and answered with 405 Method Not Allowed.
 */
HTTP_RESOURCE_DEFINE(delete_resource, papermono_http, "/api/v1/content/*",
	&delete_resource_detail);
HTTP_RESOURCE_DEFINE(content_resource, papermono_http, "/api/v1/content",
	&content_resource_detail);
HTTP_RESOURCE_DEFINE(wifi_resource, papermono_http, "/api/v1/wifi",
	&wifi_resource_detail);
#if defined(CONFIG_PAPERMONO_RSS)
HTTP_RESOURCE_DEFINE(rss_sources_resource, papermono_http, "/api/v1/rss/sources",
	&rss_sources_resource_detail);
#endif

static int configure_ap_address(struct net_if *iface)
{
	struct in_addr address;
	struct in_addr netmask;

	if (net_addr_pton(AF_INET, AP_ADDRESS, &address) < 0 ||
	    net_addr_pton(AF_INET, AP_NETMASK, &netmask) < 0) {
		return -EINVAL;
	}
	net_if_ipv4_set_gw(iface, &address);
	if (net_if_ipv4_addr_add(iface, &address, NET_ADDR_MANUAL, 0) == NULL ||
	    !net_if_ipv4_set_netmask_by_addr(iface, &address, &netmask)) {
		return -EIO;
	}
	return 0;
}

static int start_ap_dhcp(struct net_if *iface)
{
	struct in_addr pool;
	int ret;

	if (net_addr_pton(AF_INET, AP_ADDRESS, &pool) < 0) {
		return -EINVAL;
	}
	pool.s4_addr[3] += 10U;
	ret = net_dhcpv4_server_start(iface, &pool);
	return ret == -EALREADY ? 0 : ret;
}

static void normalize_provisioning_radio(bool ap_only)
{
	static const uint8_t protocols = WIFI_PROTOCOL_11B |
		WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
	esp_err_t ret;
	wifi_mode_t mode = ESP32_WIFI_MODE_NULL;
	wifi_bandwidth_t bandwidth = WIFI_BW20;
	uint8_t active_protocols = 0U;
	int8_t tx_power = 0;

	/*
	 * On the ESP32-S3 APSTA backend, the first AP_START can report success
	 * while no usable beacon leaves the radio.  A deliberate transition to
	 * AP-only produces AP_STOP/AP_START and a discoverable provisioning AP.
	 * DHCP is started from AP_ENABLE_RESULT, so the second AP_START rebuilds
	 * the server after the transition instead of leaving a stale instance.
	 */
	(void)esp_wifi_get_mode(&mode);
	if (ap_only && mode != ESP32_WIFI_MODE_AP) {
		ret = esp_wifi_set_mode(ESP32_WIFI_MODE_AP);
		if (ret != ESP_OK) {
			LOG_WRN("Unable to select provisioning AP-only mode (%d)", ret);
		}
	}
	ret = esp_wifi_set_ps(WIFI_PS_NONE);
	if (ret != ESP_OK) {
		LOG_WRN("Unable to disable Wi-Fi power save for provisioning (%d)", ret);
	}
	ret = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
	if (ret != ESP_OK) {
		LOG_WRN("Unable to select SoftAP HT20 bandwidth (%d)", ret);
	}
	ret = esp_wifi_set_protocol(WIFI_IF_AP, protocols);
	if (ret != ESP_OK) {
		LOG_WRN("Unable to select SoftAP compatibility protocols (%d)", ret);
	}
	ret = esp_wifi_set_max_tx_power(AP_MAX_TX_POWER_QDBM);
	if (ret != ESP_OK) {
		LOG_WRN("Unable to set SoftAP maximum TX power (%d)", ret);
	}

	(void)esp_wifi_get_mode(&mode);
	(void)esp_wifi_get_bandwidth(WIFI_IF_AP, &bandwidth);
	(void)esp_wifi_get_protocol(WIFI_IF_AP, &active_protocols);
	(void)esp_wifi_get_max_tx_power(&tx_power);
	LOG_INF("SoftAP radio normalized: mode=%d bandwidth=%d "
		"protocols=0x%02x tx_power=%d qdBm", mode, bandwidth,
		active_protocols, tx_power);
}

static int enable_ap(void)
{
	struct net_if *iface = net_if_get_wifi_sap();
	struct wifi_connect_req_params config = { 0 };
	const bool ap_only = !papermono_settings_get()->wifi_configured;
	int ret;

	if (iface == NULL) {
		return -ENODEV;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	if (server_state.ap_enabled || ap_enabling) {
		k_mutex_unlock(&server_lock);
		return 0;
	}
	ap_enabling = true;
	k_mutex_unlock(&server_lock);
	ret = configure_ap_address(iface);
	if (ret < 0 && ret != -EALREADY) {
		goto fail;
	}
	config.ssid = (const uint8_t *)server_state.ap_ssid;
	config.ssid_length = strlen(server_state.ap_ssid);
	config.psk = (const uint8_t *)server_state.ap_password;
	config.psk_length = strlen(server_state.ap_password);
	config.security = WIFI_SECURITY_TYPE_PSK;
	/*
	 * The ESP32 Zephyr backend translates WIFI_CHANNEL_ANY to channel zero.
	 * The controller can report AP_START for that configuration without a
	 * reliably discoverable beacon, so use a legal fixed 2.4 GHz channel.
	 */
	config.channel = AP_CHANNEL;
	config.band = WIFI_FREQ_BAND_2_4_GHZ;
	ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &config,
		sizeof(config));
	if (ret < 0) {
		goto fail;
	}
	normalize_provisioning_radio(ap_only);
	return 0;

fail:
	k_mutex_lock(&server_lock, K_FOREVER);
	ap_enabling = false;
	k_mutex_unlock(&server_lock);
	return ret;
}

static int connect_station(void)
{
	const struct papermono_preferences *prefs = papermono_settings_get();

	if (wifi_is_paused()) {
		return -EBUSY;
	}
	struct net_if *iface = net_if_get_wifi_sta();
	struct wifi_connect_req_params config = { 0 };
	/* A private copy remains valid for the entire synchronous driver call;
	 * a UI preference write must not alter the memory underneath net_mgmt.
	 */
	char ssid[PAPERMONO_WIFI_SSID_MAX + 1U];
	char psk[PAPERMONO_WIFI_PSK_MAX + 1U];
	int ret;

	if (iface == NULL || !prefs->wifi_configured) {
		return -ENODEV;
	}
	memcpy(ssid, prefs->wifi_ssid, sizeof(ssid));
	memcpy(psk, prefs->wifi_psk, sizeof(psk));
	config.ssid_length = strnlen(ssid, sizeof(ssid));
	config.psk_length = strnlen(psk, sizeof(psk));
	/* The current ESP32 driver writes a terminator at password[length];
	 * reject 64-byte legacy PSKs rather than overflowing its 64-byte array.
	 */
	if (config.ssid_length == 0U || config.ssid_length > 32U ||
	    (config.psk_length != 0U &&
	     (config.psk_length < 8U || config.psk_length > 63U))) {
		wipe_wifi_secret(psk, sizeof(psk));
		return -EINVAL;
	}
	config.ssid = (const uint8_t *)ssid;
	config.psk = (const uint8_t *)psk;
	config.security = config.psk_length == 0U ? WIFI_SECURITY_TYPE_NONE :
		WIFI_SECURITY_TYPE_PSK;
	config.mfp = config.psk_length == 0U ? WIFI_MFP_DISABLE : WIFI_MFP_OPTIONAL;
	config.channel = WIFI_CHANNEL_ANY;
	config.band = WIFI_FREQ_BAND_2_4_GHZ;
	config.timeout = 30;
	k_mutex_lock(&server_lock, K_FOREVER);
	server_state.station_connecting = true;
	wifi_state.connection = PAPERMONO_WIFI_CONNECTING;
	wifi_state.last_connection_error = 0;
	k_mutex_unlock(&server_lock);
	ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &config, sizeof(config));
	wipe_wifi_secret(psk, sizeof(psk));
	if (ret != 0) {
		ret = ret < 0 ? ret : -EIO;
		k_mutex_lock(&server_lock, K_FOREVER);
		server_state.station_connecting = false;
		wifi_state.connection = PAPERMONO_WIFI_CONNECTION_ERROR;
		wifi_state.last_connection_error = ret;
		k_mutex_unlock(&server_lock);
	}
	return ret;
}

static void schedule_recovery_ap(void)
{
	uint8_t shift;

	if (wifi_is_paused()) {
		return;
	}
	uint32_t delay_ms;

	k_mutex_lock(&server_lock, K_FOREVER);
	if (server_state.ap_enabled || server_state.station_connected || ap_enabling ||
	    wifi_state.request_busy) {
		k_mutex_unlock(&server_lock);
		return;
	}
	shift = ap_retry_shift;
	if (ap_retry_shift < AP_RETRY_MAX_SHIFT) {
		++ap_retry_shift;
	}
	k_mutex_unlock(&server_lock);
	delay_ms = MIN(AP_RETRY_INITIAL_MS << shift, AP_RETRY_MAX_MS);
	(void)wifi_reschedule(&enable_ap_work, K_MSEC(delay_ms));
	LOG_INF("Recovery SoftAP retry scheduled in %u ms", delay_ms);
}

static enum papermono_wifi_security scan_security(enum wifi_security_type type)
{
	if (type == WIFI_SECURITY_TYPE_NONE) {
		return PAPERMONO_WIFI_SECURITY_OPEN;
	}
	if (type == WIFI_SECURITY_TYPE_PSK) {
		return PAPERMONO_WIFI_SECURITY_WPA2_PSK;
	}
	return PAPERMONO_WIFI_SECURITY_UNSUPPORTED;
}

/* Called only with server_lock held. Keep strongest BSS for each SSID/security,
 * then keep the strongest bounded set. No driver or UI calls from this helper.
 */
static void scan_add_locked(const struct wifi_scan_result *result)
{
	struct papermono_wifi_network network = { 0 };
	uint8_t index;

	if (result->ssid_length > PAPERMONO_WIFI_SSID_LENGTH) {
		return;
	}
	network.ssid_length = result->ssid_length;
	memcpy(network.ssid, result->ssid, network.ssid_length);
	network.ssid[network.ssid_length] = '\0';
	network.rssi = result->rssi;
	network.channel = result->channel;
	network.security = scan_security(result->security);
	network.hidden = network.ssid_length == 0U;
	network.supported = !network.hidden &&
		memchr(network.ssid, '\0', network.ssid_length) == NULL &&
		result->band == WIFI_FREQ_BAND_2_4_GHZ &&
		network.security != PAPERMONO_WIFI_SECURITY_UNSUPPORTED;
	for (index = 0U; index < scan_count; ++index) {
		if (scan_networks[index].ssid_length == network.ssid_length &&
		    scan_networks[index].security == network.security &&
		    scan_networks[index].supported == network.supported &&
		    memcmp(scan_networks[index].ssid, network.ssid,
			 network.ssid_length) == 0) {
			if (scan_networks[index].rssi >= network.rssi) {
				return;
			}
			break;
		}
	}
	if (index == scan_count) {
		if (scan_count < PAPERMONO_WIFI_NETWORK_MAX) {
			++scan_count;
		} else {
			index = scan_count - 1U;
			if (scan_networks[index].rssi >= network.rssi) {
				return;
			}
		}
	}
	scan_networks[index] = network;
	while (index > 0U && scan_networks[index].rssi >
	       scan_networks[index - 1U].rssi) {
		const struct papermono_wifi_network previous = scan_networks[index - 1U];

		scan_networks[index - 1U] = scan_networks[index];
		scan_networks[index--] = previous;
	}
}

static void finish_scan_locked(int error)
{
	wifi_state.scan_busy = false;
	wifi_state.scan_error = error;
	wifi_state.count = scan_count;
	memcpy(wifi_state.networks, scan_networks, sizeof(scan_networks));
	++wifi_state.scan_generation;
}

static void scan_wifi_worker(struct k_work *work)
{
	if (wifi_is_paused()) {
		return;
	}
	struct net_if *iface = net_if_get_wifi_sta();
	struct wifi_scan_params params = { .scan_type = WIFI_SCAN_TYPE_ACTIVE };
	int ret;

	ARG_UNUSED(work);
	/* Arm before issuing: the driver may report results and DONE before
	 * net_mgmt returns. A nonzero return is failure, not a pending scan.
	 */
	(void)wifi_reschedule(&scan_timeout_work, K_SECONDS(30));
	ret = iface == NULL ? -ENODEV :
		net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
	if (ret != 0) {
		k_mutex_lock(&server_lock, K_FOREVER);
		if (scan_inflight) {
			scan_inflight = false;
			finish_scan_locked(ret < 0 ? ret : -EIO);
		}
		k_mutex_unlock(&server_lock);
		(void)k_work_cancel_delayable(&scan_timeout_work);
		schedule_station_retry();
		papermono_ui_request_background_refresh();
	}
}

static void scan_timeout_worker(struct k_work *work)
{
	if (wifi_is_paused()) {
		return;
	}
	bool changed = false;

	ARG_UNUSED(work);
	k_mutex_lock(&server_lock, K_FOREVER);
	if (scan_inflight && wifi_state.scan_busy) {
		finish_scan_locked(-ETIMEDOUT);
		/* Do not reuse this slot until the old DONE arrives: Zephyr has no
		 * scan transaction ID/cancel API to distinguish late results.
		 */
		changed = true;
	}
	k_mutex_unlock(&server_lock);
	if (changed) {
		papermono_ui_request_background_refresh();
	}
}

static int reset_wifi_now(void);

static void wifi_command_complete(int error)
{
	k_mutex_lock(&server_lock, K_FOREVER);
	pending_wifi_command = WIFI_COMMAND_NONE;
	wifi_command_phase = 0U;
	wifi_state.request_busy = false;
	if (error != 0) {
		wifi_state.last_connection_error = error < 0 ? error : -EIO;
		wifi_state.connection = PAPERMONO_WIFI_CONNECTION_ERROR;
	}
	wipe_wifi_secret(pending_wifi_psk, sizeof(pending_wifi_psk));
	memset(pending_wifi_ssid, 0, sizeof(pending_wifi_ssid));
	k_mutex_unlock(&server_lock);
	schedule_station_retry();
	schedule_recovery_ap();
	papermono_ui_request_background_refresh();
}

static void wifi_command_worker(struct k_work *work)
{
	if (wifi_is_paused()) {
		return;
	}
	struct net_if *iface = net_if_get_wifi_sta();
	enum wifi_command command;
	bool linking;
	int ret = 0;

	ARG_UNUSED(work);
	k_mutex_lock(&server_lock, K_FOREVER);
	command = pending_wifi_command;
	linking = server_state.station_connected || server_state.station_connecting;
	k_mutex_unlock(&server_lock);
	if (command == WIFI_COMMAND_NONE) {
		return;
	}
	if (wifi_command_phase == 0U) {
		(void)k_work_cancel_delayable(&station_retry_work);
		(void)k_work_cancel_delayable(&apply_wifi_work);
		(void)k_work_cancel_delayable(&disable_ap_work);
		(void)k_work_cancel_delayable(&enable_ap_work);
		/* Persist before disturbing a working station. NVS failure leaves
		 * the previous production connection and preferences untouched.
		 */
		if (command == WIFI_COMMAND_CONNECT) {
			ret = papermono_settings_set_wifi(pending_wifi_ssid, pending_wifi_psk);
		} else if (command == WIFI_COMMAND_FORGET || command == WIFI_COMMAND_RESET) {
			ret = papermono_settings_clear_wifi();
		}
		wipe_wifi_secret(pending_wifi_psk, sizeof(pending_wifi_psk));
		if (ret != 0) {
			wifi_command_complete(ret);
			return;
		}
		k_mutex_lock(&server_lock, K_FOREVER);
		wifi_state.configured = papermono_settings_get()->wifi_configured;
		memcpy(wifi_state.saved_ssid, papermono_settings_get()->wifi_ssid,
			sizeof(wifi_state.saved_ssid));
		wifi_state.saved_ssid[PAPERMONO_WIFI_SSID_LENGTH] = '\0';
		k_mutex_unlock(&server_lock);
		k_mutex_lock(&server_lock, K_FOREVER);
		wifi_command_phase = 1U;
		k_mutex_unlock(&server_lock);
		wifi_command_deadline = k_uptime_get() + 5000;
		if (linking) {
			ret = iface == NULL ? -ENODEV :
				net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0U);
			if (ret != 0) {
				wifi_command_complete(ret);
				return;
			}
			(void)wifi_reschedule(&wifi_command_work, K_MSEC(50));
			return;
		}
	}
	if (wifi_command_phase == 1U) {
		if (linking) {
			if (k_uptime_get() >= wifi_command_deadline) {
				wifi_command_complete(-ETIMEDOUT);
			} else {
				(void)wifi_reschedule(&wifi_command_work, K_MSEC(50));
			}
			return;
		}
		if (command == WIFI_COMMAND_FORGET || command == WIFI_COMMAND_RESET) {
			struct net_if *ap = net_if_get_wifi_sap();
			k_mutex_lock(&server_lock, K_FOREVER);
			const bool ap_enabled = server_state.ap_enabled;
			const bool ap_starting = ap_enabling;
			k_mutex_unlock(&server_lock);
			/* AP_ENABLE may still be in flight when forget is pressed.
			 * Wait for that result before stopping/rotating its credentials.
			 */
			if (ap_starting) {
				if (k_uptime_get() >= wifi_command_deadline) {
					wifi_command_complete(-ETIMEDOUT);
				} else {
					(void)wifi_reschedule(&wifi_command_work, K_MSEC(50));
				}
				return;
			}
			if (ap_enabled) {
				ret = ap == NULL ? -ENODEV :
					net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, ap, NULL, 0U);
				if (ret != 0) {
					wifi_command_complete(ret);
					return;
				}
				k_mutex_lock(&server_lock, K_FOREVER);
				wifi_command_phase = 3U;
				k_mutex_unlock(&server_lock);
				wifi_command_deadline = k_uptime_get() + 5000;
				(void)wifi_reschedule(&wifi_command_work, K_MSEC(50));
			} else {
				ret = reset_wifi_now();
				wifi_command_complete(ret);
			}
			return;
		}
		k_mutex_lock(&server_lock, K_FOREVER);
		wifi_command_phase = 2U;
		k_mutex_unlock(&server_lock);
		wifi_command_deadline = k_uptime_get() + 35000;
		ret = connect_station();
		if (ret != 0) {
			wifi_command_complete(ret);
			schedule_station_retry();
			return;
		}
	}
	if (wifi_command_phase == 3U) {
		k_mutex_lock(&server_lock, K_FOREVER);
		const bool ap_enabled = server_state.ap_enabled;
		k_mutex_unlock(&server_lock);
		if (!ap_enabled) {
			ret = reset_wifi_now();
			wifi_command_complete(ret);
		} else if (k_uptime_get() >= wifi_command_deadline) {
			wifi_command_complete(-ETIMEDOUT);
		} else {
			(void)wifi_reschedule(&wifi_command_work, K_MSEC(50));
		}
		return;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	const bool connecting = server_state.station_connecting;
	const int connection_error = wifi_state.last_connection_error;
	k_mutex_unlock(&server_lock);
	if (!connecting) {
		wifi_command_complete(connection_error);
	} else if (k_uptime_get() >= wifi_command_deadline) {
		/* Terminate a lost callback, but do not sleep/wait in this queue. */
		if (iface != NULL) {
			(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0U);
		}
		k_mutex_lock(&server_lock, K_FOREVER);
		server_state.station_connecting = false;
		k_mutex_unlock(&server_lock);
		wifi_command_complete(-ETIMEDOUT);
		schedule_station_retry();
	} else {
		(void)wifi_reschedule(&wifi_command_work, K_MSEC(100));
	}
}

static void apply_wifi(struct k_work *work)
{
	ARG_UNUSED(work);
	if (wifi_is_paused()) {
		return;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	const bool defer = scan_inflight;
	const bool command_busy = wifi_state.request_busy;
	k_mutex_unlock(&server_lock);
	if (command_busy) {
		return;
	}
	if (defer) {
		(void)wifi_reschedule(&apply_wifi_work, K_MSEC(200));
		return;
	}
	const int ret = connect_station();

	if (ret < 0) {
		LOG_WRN("Unable to start station connection (%d); keeping setup AP",
			ret);
		schedule_recovery_ap();
		schedule_station_retry();
	}
	papermono_ui_request_background_refresh();
}

static void enable_ap_worker(struct k_work *work)
{
	ARG_UNUSED(work);
	if (wifi_is_paused()) {
		return;
	}
	const int ret = enable_ap();

	if (ret < 0) {
		LOG_ERR("Unable to enable recovery SoftAP (%d)", ret);
		schedule_recovery_ap();
	}
}

static void verify_ap_worker(struct k_work *work)
{
	if (wifi_is_paused()) {
		return;
	}
	struct wifi_iface_status status = { 0 };
	struct net_if *iface = net_if_get_wifi_sap();
	char expected_ssid[sizeof(server_state.ap_ssid)];
	bool enabled;
	bool healthy;
	bool retry = false;
	bool restart = false;
	bool state_changed = false;
	uint8_t failure_count = 0U;
	int ret;

	ARG_UNUSED(work);
	k_mutex_lock(&server_lock, K_FOREVER);
	enabled = server_state.ap_enabled;
	(void)snprintf(expected_ssid, sizeof(expected_ssid), "%s",
		server_state.ap_ssid);
	k_mutex_unlock(&server_lock);
	if (!enabled) {
		return;
	}

	ret = iface == NULL ? -ENODEV :
		net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status,
			sizeof(status));
	healthy = ret == 0 && status.iface_mode == WIFI_MODE_AP &&
		status.channel == AP_CHANNEL &&
		status.ssid_len == strlen(expected_ssid) &&
		memcmp(status.ssid, expected_ssid, status.ssid_len) == 0;

	k_mutex_lock(&server_lock, K_FOREVER);
	if (!server_state.ap_enabled ||
	    strcmp(server_state.ap_ssid, expected_ssid) != 0) {
		k_mutex_unlock(&server_lock);
		return;
	}
	state_changed = server_state.ap_verified != healthy;
	server_state.ap_verified = healthy;
	if (healthy) {
		ap_health_failures = 0U;
	} else {
		failure_count = ++ap_health_failures;
		if (failure_count >= AP_HEALTH_MAX_FAILURES) {
			ap_health_failures = 0U;
			restart = true;
		} else {
			retry = true;
		}
	}
	k_mutex_unlock(&server_lock);

	if (healthy) {
		LOG_INF("SoftAP interface verified: SSID %s, channel %u",
			expected_ssid, status.channel);
	} else {
		LOG_WRN("SoftAP interface verification failed (%u/%u, ret=%d, "
			"mode=%d, channel=%u, ssid=%.*s)", failure_count,
			AP_HEALTH_MAX_FAILURES, ret, status.iface_mode, status.channel,
			(int)MIN(status.ssid_len, sizeof(status.ssid)), status.ssid);
	}
	if (retry) {
		(void)wifi_reschedule(&ap_health_work,
			K_MSEC(AP_HEALTH_DELAY_MS));
	} else if (restart) {
		LOG_ERR("SoftAP reported enabled but failed interface verification; "
			"restarting provisioning AP");
		ret = iface == NULL ? -ENODEV :
			net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0U);
		if (ret < 0) {
			k_mutex_lock(&server_lock, K_FOREVER);
			server_state.ap_enabled = false;
			server_state.ap_verified = false;
			k_mutex_unlock(&server_lock);
			schedule_recovery_ap();
		}
	}
	if (state_changed || restart) {
		papermono_ui_request_background_refresh();
	}
}

static void disable_ap(struct k_work *work)
{
	if (wifi_is_paused()) {
		return;
	}
	struct net_if *iface = net_if_get_wifi_sap();

	ARG_UNUSED(work);
	if (iface == NULL) {
		return;
	}
	int ret = net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, iface, NULL, 0U);

	if (ret < 0 && ret != -EALREADY) {
		LOG_WRN("Unable to disable provisioning SoftAP (%d)", ret);
	}
}

static void wifi_event(struct net_mgmt_event_callback *callback,
	uint64_t event, struct net_if *iface)
{
	if (event == NET_EVENT_WIFI_SCAN_RESULT || event == NET_EVENT_WIFI_SCAN_DONE) {
		bool changed = false;

		if (iface != net_if_get_wifi_sta()) {
			return;
		}
		k_mutex_lock(&server_lock, K_FOREVER);
		if (scan_inflight && event == NET_EVENT_WIFI_SCAN_RESULT &&
		    wifi_state.scan_busy && callback->info != NULL &&
		    callback->info_length >= sizeof(struct wifi_scan_result)) {
			scan_add_locked(callback->info);
		} else if (scan_inflight && event == NET_EVENT_WIFI_SCAN_DONE) {
			const struct wifi_status *done = callback->info;
			int error = done != NULL && callback->info_length >= sizeof(*done) ?
				done->status : -ENODATA;

			scan_inflight = false;
			if (wifi_state.scan_busy) {
				finish_scan_locked(error == 0 ? 0 : error < 0 ? error : -EIO);
			}
			changed = true;
		}
		k_mutex_unlock(&server_lock);
		if (changed) {
			(void)k_work_cancel_delayable(&scan_timeout_work);
			schedule_station_retry();
			papermono_ui_request_background_refresh();
		}
		return;
	}
	if ((event == NET_EVENT_WIFI_CONNECT_RESULT ||
	     event == NET_EVENT_WIFI_DISCONNECT_RESULT) && iface != net_if_get_wifi_sta()) {
		return;
	}
	const struct wifi_status *status = callback->info != NULL &&
		callback->info_length >= sizeof(struct wifi_status) ? callback->info : NULL;
	const bool success = status != NULL && status->status == 0;
	bool request_time_sync = false;
	bool request_ap = false;
	bool request_ap_health = false;
	bool request_dhcp = false;
	bool cancel_ap_health = false;
	bool request_station_retry = false;
	k_mutex_lock(&server_lock, K_FOREVER);
	if (event == NET_EVENT_WIFI_AP_ENABLE_RESULT) {
		ap_enabling = false;
		server_state.ap_enabled = success;
		server_state.ap_verified = false;
		ap_health_failures = 0U;
		if (success) {
			ap_retry_shift = 0U;
		}
		request_dhcp = success;
		request_ap_health = success;
		request_ap = !success && !server_state.station_connected;
		LOG_INF("SoftAP enable %s (status=%d, channel=%u)",
			success ? "complete" : "failed",
			status == NULL ? -ENODATA : status->status, AP_CHANNEL);
	} else if (event == NET_EVENT_WIFI_AP_DISABLE_RESULT) {
		if (success) {
			server_state.ap_enabled = false;
			server_state.ap_verified = false;
			(void)net_dhcpv4_server_stop(iface);
		}
		cancel_ap_health = success;
		request_ap = success && !server_state.station_connected;
		LOG_INF("SoftAP disable %s (status=%d)",
			success ? "complete" : "failed",
			status == NULL ? -ENODATA : status->status);
	} else if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		server_state.station_connecting = false;
		server_state.station_connected = success;
		wifi_state.connection = success ? PAPERMONO_WIFI_CONNECTED :
			PAPERMONO_WIFI_CONNECTION_ERROR;
		wifi_state.last_connection_error = success ? 0 :
			status == NULL ? -ENODATA : status->status < 0 ? status->status : -EIO;
		request_time_sync = success;
		if (success) {
			ap_retry_shift = 0U;
			station_retry_shift = 0U;
			(void)k_work_cancel_delayable(&station_retry_work);
		}
		request_ap = !success && !server_state.ap_enabled;
		request_station_retry = !success;
		if (success && server_state.ap_enabled) {
			(void)wifi_reschedule(&disable_ap_work,
				K_MSEC(AP_HANDOFF_DELAY_MS));
		}
		LOG_INF("Wi-Fi station connect %s (status=%d)",
			success ? "complete" : "failed",
			status == NULL ? -ENODATA : status->status);
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		/* The status may be non-zero (peer teardown, driver reset), but the
		 * station link is gone in every case: restore the provisioning AP and
		 * retry the saved network so the device never becomes unreachable.
		 */
		server_state.station_connecting = false;
		server_state.station_connected = false;
		/* A disconnect in command phase 1 is intentional. A disconnect while
		 * authenticating (ESP32 reports failures via this event) is an error.
		 */
		const bool intentional = wifi_state.request_busy && wifi_command_phase == 1U;
		wifi_state.connection = intentional ? PAPERMONO_WIFI_DISCONNECTED :
			PAPERMONO_WIFI_CONNECTION_ERROR;
		wifi_state.last_connection_error = intentional ? 0 :
			status == NULL ? -ENODATA : status->status < 0 ? status->status : -ENOTCONN;
		request_ap = !server_state.ap_enabled;
		request_station_retry = true;
		LOG_INF("Wi-Fi station disconnect event (status=%d)",
			status == NULL ? -ENODATA : status->status);
	}
	k_mutex_unlock(&server_lock);
	if (request_dhcp) {
		const int ret = start_ap_dhcp(iface);

		if (ret < 0) {
			LOG_ERR("Unable to start SoftAP DHCP server (%d)", ret);
		} else {
			LOG_INF("SoftAP DHCP server ready at " AP_ADDRESS);
		}
	}
	if (request_ap_health) {
		(void)wifi_reschedule(&ap_health_work,
			K_MSEC(AP_HEALTH_DELAY_MS));
	} else if (cancel_ap_health) {
		(void)k_work_cancel_delayable(&ap_health_work);
	}
	if (request_ap) {
		schedule_recovery_ap();
	}
	if (request_station_retry) {
		schedule_station_retry();
	}
#if defined(CONFIG_PAPERMONO_TIME_SYNC)
	if (request_time_sync) {
		papermono_time_sync_request();
	}
#else
	ARG_UNUSED(request_time_sync);
#endif
	papermono_ui_request_background_refresh();
}

/* Crockford base32 without I, L, O and U, so hand-copied codes stay readable
 * and no code doubles as an accidental word.
 */
static const char base32_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* Rejection sampling: 2^32 is not a multiple of 32, so masking alone would
 * bias the first characters. Values in the final partial block are redrawn.
 */
static uint32_t uniform_below(uint32_t bound)
{
	const uint32_t limit = UINT32_MAX - (UINT32_MAX % bound);
	uint32_t value;

	do {
		value = esp_random();
	} while (value >= limit);

	return value % bound;
}

static void random_base32(char *output, size_t length)
{
	for (size_t index = 0U; index < length; ++index) {
		output[index] = base32_alphabet[uniform_below(
			sizeof(base32_alphabet) - 1U)];
	}
	output[length] = '\0';
}

static void random_hex(char *output, size_t length)
{
	static const char hex[] = "0123456789ABCDEF";

	for (size_t index = 0U; index < length; ++index) {
		output[index] = hex[uniform_below(sizeof(hex) - 1U)];
	}
	output[length] = '\0';
}

/* Constant-time comparison. strcmp exits on the first differing byte, which
 * leaks the length of the matching prefix.
 */
static bool secure_equal(const char *left, const char *right, size_t length)
{
	unsigned int diff = 0U;

	for (size_t index = 0U; index < length; ++index) {
		diff |= (unsigned char)left[index] ^
			(unsigned char)right[index];
	}
	return diff == 0U;
}

static void generate_credentials(void)
{
	/* 32 bits of SSID suffix and 96 bits of AP password. The password is
	 * the only thing protecting the SoftAP, so it must not be guessable
	 * from the broadcast SSID.
	 */
	(void)snprintf(server_state.ap_ssid, sizeof(server_state.ap_ssid),
		"PaperMono-");
	random_hex(server_state.ap_ssid + strlen(server_state.ap_ssid), 8U);

	(void)snprintf(server_state.ap_password, sizeof(server_state.ap_password),
		"PM-");
	random_base32(server_state.ap_password + strlen(server_state.ap_password),
		sizeof(server_state.ap_password) - 4U);

	/* 8 base32 characters = 40 bits. Short enough to type, wide enough that
	 * online guessing is impractical once the throttle below applies.
	 */
	random_base32(server_state.pairing_code,
		PAPERMONO_PAIRING_CODE_LENGTH);
	random_hex(session_token, sizeof(session_token) - 1U);
}

int papermono_content_server_init(void)
{
	const uint64_t event_mask = NET_EVENT_WIFI_CONNECT_RESULT |
		NET_EVENT_WIFI_DISCONNECT_RESULT | NET_EVENT_WIFI_AP_ENABLE_RESULT |
		NET_EVENT_WIFI_AP_DISABLE_RESULT | NET_EVENT_WIFI_SCAN_RESULT |
		NET_EVENT_WIFI_SCAN_DONE;
	int ret;

	if (initialized) {
		return 0;
	}
	generate_credentials();
	k_work_queue_start(&wifi_work_queue, wifi_work_stack,
		K_THREAD_STACK_SIZEOF(wifi_work_stack), 7, NULL);
	k_work_init(&scan_work, scan_wifi_worker);
	k_work_init_delayable(&scan_timeout_work, scan_timeout_worker);
	k_work_init_delayable(&wifi_command_work, wifi_command_worker);
	wifi_state.configured = papermono_settings_get()->wifi_configured;
	memcpy(wifi_state.saved_ssid, papermono_settings_get()->wifi_ssid,
		sizeof(wifi_state.saved_ssid));
	wifi_state.saved_ssid[PAPERMONO_WIFI_SSID_LENGTH] = '\0';
	k_work_init_delayable(&apply_wifi_work, apply_wifi);
	k_work_init_delayable(&enable_ap_work, enable_ap_worker);
	k_work_init_delayable(&disable_ap_work, disable_ap);
	k_work_init_delayable(&ap_health_work, verify_ap_worker);
#if defined(CONFIG_PAPERMONO_TIME_SYNC)
	ret = papermono_time_sync_init();
	if (ret < 0) {
		LOG_WRN("Time synchronization unavailable (%d)", ret);
	}
#endif
	net_mgmt_init_event_callback(&wifi_callback, wifi_event, event_mask);
	net_mgmt_add_event_callback(&wifi_callback);
	ret = http_server_start();
	if (ret < 0) {
		return ret;
	}
	/* Boot restoration follows the same private queue as UI and retries;
	 * saved production NVS credentials are neither rewritten nor logged.
	 */
	initialized = true;
	if (papermono_settings_get()->wifi_configured) {
		(void)wifi_reschedule(&apply_wifi_work, K_NO_WAIT);
	} else {
		(void)wifi_reschedule(&enable_ap_work, K_NO_WAIT);
	}
	/* Log the SSID only. The AP password and the pairing code are live
	 * credentials: this firmware's only diagnostic channel is the USB
	 * serial/JTAG console, and capture logs are routinely pasted into
	 * issues, so emitting them here hands out full pairing access. Both
	 * values are already shown on the e-paper screen for the operator.
	 */
	LOG_INF("Content center ready: SoftAP \"%s\" (credentials shown on screen)",
		server_state.ap_ssid);
	papermono_ui_request_background_refresh();
	return 0;
}

void papermono_content_server_get_state(
	struct papermono_content_server_state *state)
{
	if (state == NULL) {
		return;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	*state = server_state;
	k_mutex_unlock(&server_lock);
	state->station_ipv4[0] = '\0';
	if (state->station_connected) {
		struct net_if *iface = net_if_get_wifi_sta();
		struct in_addr *address = iface == NULL ? NULL :
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (address != NULL) {
			(void)net_addr_ntop(AF_INET, address, state->station_ipv4,
				sizeof(state->station_ipv4));
		}
	}
}

bool papermono_content_server_busy(void)
{
	bool wifi_busy;

	k_mutex_lock(&server_lock, K_FOREVER);
	wifi_busy = wifi_state.request_busy || wifi_state.scan_busy;
	k_mutex_unlock(&server_lock);
	return atomic_get(&server_busy) != 0 || wifi_busy;
}

void papermono_content_server_get_wifi_state(struct papermono_wifi_state *state)
{
	if (state == NULL) {
		return;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	*state = wifi_state;
	k_mutex_unlock(&server_lock);
}

int papermono_content_server_scan_wifi(void)
{
	int ret;

	k_mutex_lock(&server_lock, K_FOREVER);
	if (!initialized) {
		ret = -EAGAIN;
	} else if (scan_inflight || wifi_state.request_busy ||
		   server_state.station_connecting) {
		ret = -EBUSY;
	} else {
		scan_inflight = true;
		scan_count = 0U;
		memset(scan_networks, 0, sizeof(scan_networks));
		wifi_state.scan_busy = true;
		wifi_state.scan_error = 0;
		ret = k_work_submit_to_queue(&wifi_work_queue, &scan_work);
		if (ret < 0) {
			scan_inflight = false;
			finish_scan_locked(ret);
		} else {
			ret = 0;
		}
	}
	k_mutex_unlock(&server_lock);
	return ret;
}

/* Caller holds server_lock; submit itself never invokes the handler inline. */
static int submit_wifi_command_locked(enum wifi_command command)
{
	if (!initialized) {
		return -EAGAIN;
	}
	if (wifi_state.request_busy || scan_inflight) {
		return -EBUSY;
	}
	pending_wifi_command = command;
	wifi_command_phase = 0U;
	wifi_state.request_busy = true;
	wifi_state.last_connection_error = 0;
	const int ret = wifi_reschedule(&wifi_command_work, K_NO_WAIT);

	if (ret < 0) {
		pending_wifi_command = WIFI_COMMAND_NONE;
		wifi_state.request_busy = false;
		return ret;
	}
	return 0;
}

int papermono_content_server_connect_wifi(
	const struct papermono_wifi_network *network, const char *password)
{
	if (network == NULL || password == NULL || network->ssid_length == 0U ||
	    network->ssid_length > PAPERMONO_WIFI_SSID_LENGTH ||
	    memchr(network->ssid, '\0', network->ssid_length) != NULL) {
		return -EINVAL;
	}
	if (network->hidden || !network->supported ||
	    (network->security != PAPERMONO_WIFI_SECURITY_OPEN &&
	     network->security != PAPERMONO_WIFI_SECURITY_WPA2_PSK)) {
		return -ENOTSUP;
	}
	const size_t length = strnlen(password, PAPERMONO_WIFI_PSK_MAX + 1U);

	if ((network->security == PAPERMONO_WIFI_SECURITY_OPEN && length != 0U) ||
	    (network->security == PAPERMONO_WIFI_SECURITY_WPA2_PSK &&
	     (length < 8U || length > 63U))) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < length; ++index) {
		if ((unsigned char)password[index] < 0x20U ||
		    (unsigned char)password[index] > 0x7eU) {
			return -EINVAL;
		}
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	/* Reserve and copy under the same lock so another UI/HTTP caller cannot
	 * overwrite a command's credentials while its NVS write is running.
	 */
	int ret;

	if (!initialized || wifi_state.request_busy || scan_inflight) {
		ret = !initialized ? -EAGAIN : -EBUSY;
	} else {
		memset(pending_wifi_ssid, 0, sizeof(pending_wifi_ssid));
		memcpy(pending_wifi_ssid, network->ssid, network->ssid_length);
		memset(pending_wifi_psk, 0, sizeof(pending_wifi_psk));
		memcpy(pending_wifi_psk, password, length);
		ret = submit_wifi_command_locked(WIFI_COMMAND_CONNECT);
		if (ret != 0) {
			wipe_wifi_secret(pending_wifi_psk, sizeof(pending_wifi_psk));
		}
	}
	k_mutex_unlock(&server_lock);
	return ret;
}

int papermono_content_server_connect_saved(void)
{
	k_mutex_lock(&server_lock, K_FOREVER);
	const int ret = !wifi_state.configured ? -ENOENT :
		submit_wifi_command_locked(WIFI_COMMAND_SAVED);
	k_mutex_unlock(&server_lock);
	return ret;
}

int papermono_content_server_forget_wifi(void)
{
	k_mutex_lock(&server_lock, K_FOREVER);
	const int ret = submit_wifi_command_locked(WIFI_COMMAND_FORGET);
	k_mutex_unlock(&server_lock);
	return ret;
}

int papermono_content_server_reset_wifi(void)
{
	k_mutex_lock(&server_lock, K_FOREVER);
	const int ret = submit_wifi_command_locked(WIFI_COMMAND_RESET);
	k_mutex_unlock(&server_lock);
	return ret;
}

int papermono_content_server_pause_wifi(void)
{
	if (!initialized) {
		return 0;
	}
	/* Stop every Wi-Fi worker before bringing the radio down, so a retry or
	 * AP-enable work cannot immediately undo the pause. The BLE voice stream
	 * shares the same 2.4 GHz radio and needs the full connection interval.
	 */
	atomic_set(&wifi_paused, 1);
	(void)k_work_cancel_delayable(&station_retry_work);
	(void)k_work_cancel_delayable(&apply_wifi_work);
	(void)k_work_cancel_delayable(&enable_ap_work);
	(void)k_work_cancel_delayable(&disable_ap_work);
	(void)k_work_cancel_delayable(&ap_health_work);
	(void)k_work_cancel_delayable(&wifi_command_work);
	(void)k_work_cancel_delayable(&scan_timeout_work);
	(void)k_work_cancel(&scan_work);

	k_mutex_lock(&server_lock, K_FOREVER);
	server_state.ap_enabled = false;
	server_state.ap_verified = false;
	server_state.station_connected = false;
	server_state.station_connecting = false;
	scan_inflight = false;
	ap_enabling = false;
	wifi_state.request_busy = false;
	station_retry_shift = 0U;
	ap_retry_shift = 0U;
	k_mutex_unlock(&server_lock);

	struct net_if *sta = net_if_get_wifi_sta();
	struct net_if *sap = net_if_get_wifi_sap();

	if (sta != NULL) {
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, sta, NULL, 0U);
	}
	if (sap != NULL) {
		(void)net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, sap, NULL, 0U);
	}
	/* Let the driver stop beacons/connection events before audio starts. */
	k_msleep(60);
	papermono_ui_request_background_refresh();
	LOG_INF("Wi-Fi paused for BLE voice session");
	return 0;
}

int papermono_content_server_resume_wifi(void)
{
	if (!initialized) {
		atomic_clear(&wifi_paused);
		return 0;
	}
	atomic_clear(&wifi_paused);
	if (papermono_settings_get()->wifi_configured) {
		(void)wifi_reschedule(&apply_wifi_work, K_NO_WAIT);
	} else {
		(void)wifi_reschedule(&enable_ap_work, K_NO_WAIT);
	}
	papermono_ui_request_background_refresh();
	LOG_INF("Wi-Fi resumed after BLE voice session");
	return 0;
}

static int reset_wifi_now(void)
{
	int ret;
	/* NVS erase, station disconnect and AP stop completed in the worker.
	 * Only now may the displayed and broadcast AP credentials rotate.
	 */
	(void)k_work_cancel_delayable(&disable_ap_work);
	(void)k_work_cancel_delayable(&ap_health_work);
	k_mutex_lock(&server_lock, K_FOREVER);
	server_state.station_connected = false;
	server_state.station_connecting = false;
	server_state.client_paired = false;
	server_state.ap_verified = false;
	wifi_state.connection = PAPERMONO_WIFI_DISCONNECTED;
	wifi_state.last_connection_error = 0;
	ap_health_failures = 0U;
	ap_retry_shift = 0U;
	station_retry_shift = 0U;
	/* A freshly generated code must be usable straight away: leaving the
	 * previous lockout armed would reject the new code for up to 30 s, and a
	 * stale failure count would rotate it again on the first typo.
	 */
	pairing_failures = 0U;
	pairing_lockout_until_ms = 0;
	(void)memset(ticket_slots, 0, sizeof(ticket_slots));
	generate_credentials();
	k_mutex_unlock(&server_lock);
	papermono_ui_request_background_refresh();
	ret = enable_ap();
	if (ret < 0) {
		schedule_recovery_ap();
	}
	return ret;
}
