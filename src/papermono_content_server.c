/* SPDX-License-Identifier: MIT */

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
};

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_SERVER_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_SERVER_BSS
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
static uint8_t wifi_request_buffer[WIFI_REQUEST_LIMIT + 1U];
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
static uint8_t ap_retry_shift;
static uint8_t ap_health_failures;
static uint8_t station_retry_shift;
static void station_retry(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(station_retry_work, station_retry);
static atomic_t server_busy;
K_MUTEX_DEFINE(server_lock);

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
	uint32_t delay_ms;

	k_mutex_lock(&server_lock, K_FOREVER);
	if (server_state.station_connected || server_state.station_connecting ||
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
	(void)k_work_reschedule(&station_retry_work, K_MSEC(delay_ms));
	LOG_INF("Wi-Fi station retry scheduled in %u ms", delay_ms);
}

static void station_retry(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!papermono_settings_get()->wifi_configured) {
		return;
	}
	k_mutex_lock(&server_lock, K_FOREVER);
	const bool busy = server_state.station_connected ||
		server_state.station_connecting;
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
		int ret = parsed == BIT_MASK(ARRAY_SIZE(wifi_request_descr)) ?
			papermono_settings_set_wifi(request_data.ssid, request_data.psk) :
			-EINVAL;
		wifi_request_length = 0U;
		wifi_request_owner = NULL;
		content_server_set_busy(CONTENT_SERVER_BUSY_WIFI_REQUEST, false);
		if (ret < 0) {
			response_json(response, HTTP_400_BAD_REQUEST,
				"{\"error\":\"invalid wifi credentials\"}");
			return 0;
		}
		response_json(response, HTTP_202_ACCEPTED, "{\"ok\":true}");
		(void)k_work_reschedule(&apply_wifi_work, K_MSEC(500));
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
	struct net_if *iface = net_if_get_wifi_sta();
	struct wifi_connect_req_params config = { 0 };

	if (iface == NULL || !prefs->wifi_configured) {
		return -ENODEV;
	}
	config.ssid = (const uint8_t *)prefs->wifi_ssid;
	config.ssid_length = strlen(prefs->wifi_ssid);
	config.psk = (const uint8_t *)prefs->wifi_psk;
	config.psk_length = strlen(prefs->wifi_psk);
	config.security = WIFI_SECURITY_TYPE_PSK;
	config.channel = WIFI_CHANNEL_ANY;
	config.band = WIFI_FREQ_BAND_2_4_GHZ;
	k_mutex_lock(&server_lock, K_FOREVER);
	server_state.station_connecting = true;
	k_mutex_unlock(&server_lock);
	const int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &config,
		sizeof(config));

	if (ret < 0) {
		k_mutex_lock(&server_lock, K_FOREVER);
		server_state.station_connecting = false;
		k_mutex_unlock(&server_lock);
	}
	return ret;
}

static void schedule_recovery_ap(void)
{
	uint8_t shift;
	uint32_t delay_ms;

	k_mutex_lock(&server_lock, K_FOREVER);
	if (server_state.ap_enabled || server_state.station_connected || ap_enabling) {
		k_mutex_unlock(&server_lock);
		return;
	}
	shift = ap_retry_shift;
	if (ap_retry_shift < AP_RETRY_MAX_SHIFT) {
		++ap_retry_shift;
	}
	k_mutex_unlock(&server_lock);
	delay_ms = MIN(AP_RETRY_INITIAL_MS << shift, AP_RETRY_MAX_MS);
	(void)k_work_reschedule(&enable_ap_work, K_MSEC(delay_ms));
	LOG_INF("Recovery SoftAP retry scheduled in %u ms", delay_ms);
}

static void apply_wifi(struct k_work *work)
{
	ARG_UNUSED(work);
	const int ret = connect_station();

	if (ret < 0) {
		LOG_WRN("Unable to start station connection (%d); keeping setup AP",
			ret);
		schedule_recovery_ap();
	}
}

static void enable_ap_worker(struct k_work *work)
{
	ARG_UNUSED(work);
	const int ret = enable_ap();

	if (ret < 0) {
		LOG_ERR("Unable to enable recovery SoftAP (%d)", ret);
		schedule_recovery_ap();
	}
}

static void verify_ap_worker(struct k_work *work)
{
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
		(void)k_work_reschedule(&ap_health_work,
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
	const struct wifi_status *status = callback->info;
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
		request_time_sync = success;
		if (success) {
			ap_retry_shift = 0U;
			station_retry_shift = 0U;
			(void)k_work_cancel_delayable(&station_retry_work);
		}
		request_ap = !success && !server_state.ap_enabled;
		request_station_retry = !success;
		if (success && server_state.ap_enabled) {
			(void)k_work_reschedule(&disable_ap_work,
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
		(void)k_work_reschedule(&ap_health_work,
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
		NET_EVENT_WIFI_AP_DISABLE_RESULT;
	int ret;

	if (initialized) {
		return 0;
	}
	generate_credentials();
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
	if (papermono_settings_get()->wifi_configured) {
		ret = connect_station();
		if (ret < 0) {
			LOG_WRN("Saved Wi-Fi could not start (%d); enabling setup AP", ret);
			ret = enable_ap();
		}
	} else {
		ret = enable_ap();
	}
	if (ret < 0) {
		LOG_WRN("Wi-Fi interface is not ready yet (%d); retrying setup AP", ret);
		schedule_recovery_ap();
	}
	initialized = true;
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
	return atomic_get(&server_busy) != 0;
}

int papermono_content_server_reset_wifi(void)
{
	int ret = papermono_settings_clear_wifi();
	if (ret < 0) {
		return ret;
	}
	struct net_if *station = net_if_get_wifi_sta();
	if (station != NULL) {
		(void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, station, NULL, 0U);
	}
	(void)k_work_cancel_delayable(&disable_ap_work);
	(void)k_work_cancel_delayable(&ap_health_work);
	k_mutex_lock(&server_lock, K_FOREVER);
	server_state.station_connected = false;
	server_state.station_connecting = false;
	server_state.client_paired = false;
	server_state.ap_verified = false;
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
