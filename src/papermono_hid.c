/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <papermono/hid.h>
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_hid, LOG_LEVEL_INF);

#if defined(CONFIG_BT_ESP32) && (defined(CONFIG_SOC_SERIES_ESP32S3) || \
	defined(CONFIG_SOC_SERIES_ESP32C3))
BUILD_ASSERT(IS_ENABLED(CONFIG_ESP32_BT_CTLR_LE_SECURITY_ENABLE),
	"BLE HID pairing requires ESP controller security support");
BUILD_ASSERT(CONFIG_BT_RX_STACK_SIZE >= 2048,
	"BLE HID bonding requires at least 2048 bytes of Bluetooth RX stack");
#endif

enum {
	HIDS_REMOTE_WAKE = BIT(0),
	HIDS_NORMALLY_CONNECTABLE = BIT(1),
	HIDS_INPUT = 0x01,
};

enum hid_attribute_index {
	HID_ATTR_MEDIA_VALUE = 6U,
	HID_ATTR_KEYBOARD_VALUE = 10U,
	HID_ATTR_MOUSE_VALUE = 14U,
};

#define HID_NOTIFY_RETRY_COUNT 5U
#define HID_NOTIFY_RETRY_MS 5U
#define HID_RELEASE_DELAY_MS 8U

struct hids_info {
	uint16_t version;
	uint8_t country;
	uint8_t flags;
} __packed;

struct hids_report_reference {
	uint8_t id;
	uint8_t type;
} __packed;

static const struct hids_info hid_info = {
	.version = sys_cpu_to_le16(0x0111),
	.country = 0,
	.flags = HIDS_REMOTE_WAKE | HIDS_NORMALLY_CONNECTABLE,
};
static const struct hids_report_reference media_reference = {
	.id = PAPERMONO_HID_REPORT_MEDIA, .type = HIDS_INPUT,
};
static const struct hids_report_reference keyboard_reference = {
	.id = PAPERMONO_HID_REPORT_KEYBOARD, .type = HIDS_INPUT,
};
static const struct hids_report_reference mouse_reference = {
	.id = PAPERMONO_HID_REPORT_MOUSE, .type = HIDS_INPUT,
};
static uint8_t control_point;

static struct papermono_hid_state hid_state;
static struct bt_conn *active_connection;
static bool mouse_enabled;
static int16_t mouse_x, mouse_y, mouse_wheel;
static uint32_t mouse_generation, mouse_pending_since;
static uint32_t mouse_reports, mouse_retries;
static uint32_t mouse_last_attempt_ms;
static bool mouse_clock_valid;
#define MOUSE_REPORT_INTERVAL_MS 15U
K_MUTEX_DEFINE(hid_lock);
K_MUTEX_DEFINE(hid_report_lock);

static void clear_mouse_locked(void)
{
	mouse_x = mouse_y = mouse_wheel = 0;
	++mouse_generation;
}

static ssize_t read_info(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, void *buffer, uint16_t length,
	uint16_t offset)
{
	ARG_UNUSED(attribute);
	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
		&hid_info, sizeof(hid_info));
}

static ssize_t read_report_map(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, void *buffer, uint16_t length,
	uint16_t offset)
{
	size_t report_map_length;
	const uint8_t *report_map = papermono_hid_report_map(
		&report_map_length);

	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
		report_map, report_map_length);
}

static ssize_t read_media_report(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, void *buffer, uint16_t length,
	uint16_t offset)
{
	static const uint8_t idle[PAPERMONO_HID_MEDIA_REPORT_SIZE];

	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
		idle, sizeof(idle));
}

static ssize_t read_keyboard_report(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, void *buffer, uint16_t length,
	uint16_t offset)
{
	static const uint8_t idle[PAPERMONO_HID_KEYBOARD_REPORT_SIZE];

	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
		idle, sizeof(idle));
}

static ssize_t read_mouse_report(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, void *buffer, uint16_t length,
	uint16_t offset)
{
	static const uint8_t idle[PAPERMONO_HID_MOUSE_REPORT_SIZE];

	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
		idle, sizeof(idle));
}

static ssize_t read_report_reference(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, void *buffer, uint16_t length,
	uint16_t offset)
{
	return bt_gatt_attr_read(connection, attribute, buffer, length, offset,
		attribute->user_data, sizeof(struct hids_report_reference));
}

static ssize_t write_control_point(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, const void *buffer, uint16_t length,
	uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(connection);
	ARG_UNUSED(flags);
	if (offset != 0U || length != sizeof(control_point)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	const uint8_t value = *(const uint8_t *)buffer;

	/* HID Control Point: 0=suspend, 1=exit suspend. */
	if (value > 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	memcpy(attribute->user_data, &value, sizeof(value));
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.suspended = value == 0U;
	k_mutex_unlock(&hid_lock);
	return length;
}

static void report_ccc_changed(const struct bt_gatt_attr *attribute,
	uint16_t value)
{
	ARG_UNUSED(attribute);
	ARG_UNUSED(value);
	/* Subscription state is connection-specific and may be restored from a
	 * bond. Query bt_gatt_is_subscribed() at send time instead of caching a
	 * process-global boolean which becomes stale after reconnect.
	 */
}

#define HID_READ_ENCRYPT BT_GATT_PERM_READ_ENCRYPT
#define HID_WRITE_ENCRYPT BT_GATT_PERM_WRITE_ENCRYPT

BT_GATT_SERVICE_DEFINE(papermono_hid_service,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_info, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ, read_report_map, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, HID_READ_ENCRYPT,
		read_media_report, NULL, NULL),
	BT_GATT_CCC(report_ccc_changed, HID_READ_ENCRYPT | HID_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
		read_report_reference, NULL, (void *)&media_reference),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, HID_READ_ENCRYPT,
		read_keyboard_report, NULL, NULL),
	BT_GATT_CCC(report_ccc_changed, HID_READ_ENCRYPT | HID_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
		read_report_reference, NULL, (void *)&keyboard_reference),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, HID_READ_ENCRYPT,
		read_mouse_report, NULL, NULL),
	BT_GATT_CCC(report_ccc_changed, HID_READ_ENCRYPT | HID_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
		read_report_reference, NULL, (void *)&mouse_reference),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT,
		BT_GATT_CHRC_WRITE_WITHOUT_RESP, HID_WRITE_ENCRYPT, NULL,
		write_control_point, &control_point));

static const struct bt_data advertising_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
		BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};
static const struct bt_data scan_response[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1U),
};

static int start_advertising(void)
{
	int ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, advertising_data,
		ARRAY_SIZE(advertising_data), scan_response,
		ARRAY_SIZE(scan_response));

	k_mutex_lock(&hid_lock, K_FOREVER);
	if (ret == 0 || ret == -EALREADY) {
		hid_state.advertising = true;
		hid_state.last_error = 0;
		ret = 0;
	} else {
		hid_state.advertising = false;
		hid_state.last_error = ret;
	}
	k_mutex_unlock(&hid_lock);
	if (ret < 0) {
		LOG_WRN("BLE HID advertising failed (%d)", ret);
	} else {
		LOG_INF("BLE HID advertising ready");
	}
	return ret;
}

static void restart_advertising(struct k_work *work)
{
	bool needed;

	ARG_UNUSED(work);
	k_mutex_lock(&hid_lock, K_FOREVER);
	needed = hid_state.initialized && !hid_state.connected &&
		!hid_state.advertising;
	k_mutex_unlock(&hid_lock);
	if (needed) {
		(void)start_advertising();
		papermono_ui_request_background_refresh();
	}
}

K_WORK_DEFINE(advertising_work, restart_advertising);

static void connection_recycled(void)
{
	/* disconnected() still runs with a stack-owned connection reference.
	 * With BT_MAX_CONN=1, connectable advertising must wait for this event.
	 * Queue HCI work instead of starting it inside the stack callback.
	 */
	(void)k_work_submit(&advertising_work);
}

static void connected(struct bt_conn *connection, uint8_t error)
{
	if (error != 0U) {
		LOG_WRN("BLE HID connection failed (0x%02x)", error);
		k_mutex_lock(&hid_lock, K_FOREVER);
		hid_state.last_error = -EIO;
		hid_state.advertising = false;
		k_mutex_unlock(&hid_lock);
		return;
	}
	k_mutex_lock(&hid_lock, K_FOREVER);
	if (active_connection != NULL) {
		bt_conn_unref(active_connection);
	}
	active_connection = bt_conn_ref(connection);
	hid_state.connected = true;
	hid_state.secured = false;
	hid_state.advertising = false;
	hid_state.suspended = false;
	hid_state.passkey_visible = false;
	clear_mouse_locked();
	mouse_clock_valid = false;
	k_mutex_unlock(&hid_lock);
	/* Force authenticated pairing so the passkey callback can put the code on
	 * the e-paper display. Bonds are restored by Settings on later boots.
	 */
	const int security_ret = bt_conn_set_security(connection, BT_SECURITY_L3);
	LOG_INF("BLE HID connected; security request result=%d", security_ret);
	if (security_ret < 0) {
		k_mutex_lock(&hid_lock, K_FOREVER);
		hid_state.last_error = security_ret;
		k_mutex_unlock(&hid_lock);
	}
	papermono_ui_request_background_refresh();
}

static void disconnected(struct bt_conn *connection, uint8_t reason)
{
	ARG_UNUSED(connection);
	LOG_INF("BLE HID disconnected (0x%02x)", reason);
	k_mutex_lock(&hid_lock, K_FOREVER);
	if (active_connection != NULL) {
		bt_conn_unref(active_connection);
		active_connection = NULL;
	}
	hid_state.connected = false;
	hid_state.secured = false;
	hid_state.advertising = false;
	hid_state.suspended = false;
	hid_state.passkey_visible = false;
	clear_mouse_locked();
	mouse_clock_valid = false;
	k_mutex_unlock(&hid_lock);
	papermono_ui_request_background_refresh();
}

static void security_changed(struct bt_conn *connection, bt_security_t level,
	enum bt_security_err error)
{
	bool active;

	k_mutex_lock(&hid_lock, K_FOREVER);
	active = active_connection == connection;
	if (active) {
		hid_state.secured = error == BT_SECURITY_ERR_SUCCESS &&
			level >= BT_SECURITY_L2;
		hid_state.last_error = error == BT_SECURITY_ERR_SUCCESS ? 0 :
			-(int)error;
	}
	k_mutex_unlock(&hid_lock);
	if (active) {
		LOG_INF("BLE HID security changed: level=%u error=%u",
			(unsigned)level, (unsigned)error);
		papermono_ui_request_background_refresh();
	}
}

static void connection_parameters_updated(struct bt_conn *connection,
	uint16_t interval, uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(connection);
	ARG_UNUSED(timeout);
	LOG_INF("BLE HID interval=%u us latency=%u", interval * 1250U, latency);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = connection_recycled,
	.security_changed = security_changed,
	.le_param_updated = connection_parameters_updated,
};

static void passkey_display(struct bt_conn *connection, unsigned int passkey)
{
	ARG_UNUSED(connection);
	LOG_INF("BLE HID pairing code displayed");
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.passkey = passkey;
	hid_state.passkey_visible = true;
	k_mutex_unlock(&hid_lock);
	papermono_ui_request_background_refresh();
}

static void pairing_cancelled(struct bt_conn *connection)
{
	ARG_UNUSED(connection);
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.passkey_visible = false;
	k_mutex_unlock(&hid_lock);
	papermono_ui_request_background_refresh();
}

static struct bt_conn_auth_cb authentication_callbacks = {
	.passkey_display = passkey_display,
	.cancel = pairing_cancelled,
};

static void pairing_complete(struct bt_conn *connection, bool bonded)
{
	ARG_UNUSED(connection);
	LOG_INF("BLE HID pairing complete; bonded=%d", bonded);
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.bonded = bonded;
	hid_state.passkey_visible = false;
	k_mutex_unlock(&hid_lock);
	papermono_ui_request_background_refresh();
}

static void pairing_failed(struct bt_conn *connection,
	enum bt_security_err reason)
{
	ARG_UNUSED(connection);
	LOG_WRN("BLE HID pairing failed (%u)", (unsigned)reason);
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.last_error = -(int)reason;
	hid_state.passkey_visible = false;
	k_mutex_unlock(&hid_lock);
	papermono_ui_request_background_refresh();
}

static void bond_deleted(uint8_t identity, const bt_addr_le_t *peer)
{
	ARG_UNUSED(identity);
	ARG_UNUSED(peer);
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.bonded = false;
	k_mutex_unlock(&hid_lock);
	papermono_ui_request_background_refresh();
}

static struct bt_conn_auth_info_cb authentication_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
	.bond_deleted = bond_deleted,
};

static void count_bond(const struct bt_bond_info *information, void *user_data)
{
	ARG_UNUSED(information);
	*(bool *)user_data = true;
}

int papermono_hid_init(void)
{
	bool bonded = false;
	int ret;

	k_mutex_lock(&hid_lock, K_FOREVER);
	if (hid_state.initialized) {
		k_mutex_unlock(&hid_lock);
		return 0;
	}
	k_mutex_unlock(&hid_lock);
	ret = bt_conn_auth_cb_register(&authentication_callbacks);
	ret = ret ?: bt_conn_auth_info_cb_register(
		&authentication_info_callbacks);
	ret = ret ?: bt_enable(NULL);
	ret = ret ?: settings_load_subtree("bt");
	if (ret < 0) {
		k_mutex_lock(&hid_lock, K_FOREVER);
		hid_state.last_error = ret;
		k_mutex_unlock(&hid_lock);
		return ret;
	}
	bt_foreach_bond(BT_ID_DEFAULT, count_bond, &bonded);
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.initialized = true;
	hid_state.bonded = bonded;
	k_mutex_unlock(&hid_lock);
	ret = start_advertising();
	LOG_INF("BLE HID ready, bonded=%d", bonded);
	papermono_ui_request_background_refresh();
	return ret;
}

static int notify_report(size_t attribute_index, const void *report,
	size_t length)
{
	struct bt_conn *connection;
	bool suspended;
	int ret;

	k_mutex_lock(&hid_lock, K_FOREVER);
	connection = active_connection == NULL ? NULL :
		bt_conn_ref(active_connection);
	suspended = hid_state.suspended;
	k_mutex_unlock(&hid_lock);
	if (connection == NULL) {
		return -ENOTCONN;
	}
	if (suspended) {
		bt_conn_unref(connection);
		return -EHOSTDOWN;
	}
	const struct bt_gatt_attr *attribute =
		&papermono_hid_service.attrs[attribute_index];
	if (!bt_gatt_is_subscribed(connection, attribute,
		BT_GATT_CCC_NOTIFY)) {
		bt_conn_unref(connection);
		return -EACCES;
	}
	for (uint8_t attempt = 0U; attempt < HID_NOTIFY_RETRY_COUNT; ++attempt) {
		ret = bt_gatt_notify(connection, attribute, report, length);
		if (ret != -ENOMEM && ret != -EAGAIN) {
			break;
		}
		k_msleep(HID_NOTIFY_RETRY_MS);
	}
	bt_conn_unref(connection);
	return ret;
}

static int notify_encoded_report(const struct papermono_hid_report *report)
{
	uint8_t released[PAPERMONO_HID_REPORT_MAX_SIZE] = { 0 };
	size_t attribute_index;
	int ret;

	switch (report->id) {
	case PAPERMONO_HID_REPORT_MEDIA:
		attribute_index = HID_ATTR_MEDIA_VALUE;
		break;
	case PAPERMONO_HID_REPORT_KEYBOARD:
		attribute_index = HID_ATTR_KEYBOARD_VALUE;
		break;
	case PAPERMONO_HID_REPORT_MOUSE:
		attribute_index = HID_ATTR_MOUSE_VALUE;
		break;
	default:
		return -EINVAL;
	}
	if (report->length == 0U || report->length > sizeof(released)) {
		return -EMSGSIZE;
	}
	ret = notify_report(attribute_index, report->data, report->length);
	if (ret == 0 && report->release_required) {
		/* Space the release from the press and retry temporary ATT buffer
		 * exhaustion. Losing this packet leaves a key/button held on the host.
		 */
		k_msleep(HID_RELEASE_DELAY_MS);
		ret = notify_report(attribute_index, released, report->length);
	}
	return ret;
}

int papermono_hid_send_action(const struct papermono_hid_action *action)
{
	struct papermono_hid_report report;
	int ret = papermono_hid_encode_action(action, &report);

	if (ret == 0) {
		k_mutex_lock(&hid_report_lock, K_FOREVER);
		ret = notify_encoded_report(&report);
		k_mutex_unlock(&hid_report_lock);
	}
	k_mutex_lock(&hid_lock, K_FOREVER);
	hid_state.last_error = ret;
	k_mutex_unlock(&hid_lock);
	return ret;
}

static void mouse_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(mouse_work, mouse_work_handler);

static void schedule_mouse(uint32_t delay)
{
	if (delay == 0U) {
		/* A previous mode may have left a delayed wakeup pending. Pull only
		 * immediate work forward; normal samples must not postpone a slot.
		 */
		(void)k_work_reschedule(&mouse_work, K_NO_WAIT);
	} else {
		(void)k_work_schedule(&mouse_work, K_MSEC(delay));
	}
}

static void accumulate_mouse(int16_t *pending, int16_t delta)
{
	/* Bound congestion to four reports instead of building a cursor backlog. */
	*pending = CLAMP((int)*pending + delta, -508, 508);
}

static uint32_t mouse_delay_locked(uint32_t now)
{
	const uint32_t elapsed = now - mouse_last_attempt_ms;

	return !mouse_clock_valid || elapsed >= MOUSE_REPORT_INTERVAL_MS ? 0U :
		MOUSE_REPORT_INTERVAL_MS - elapsed;
}

static void mouse_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	struct bt_conn *connection = NULL;
	uint8_t report[4];
	uint32_t generation, since, delay;
	bool pending;
	int ret = 0;

	k_mutex_lock(&hid_report_lock, K_FOREVER);
	k_mutex_lock(&hid_lock, K_FOREVER);
	if (!mouse_enabled || !hid_state.connected || !hid_state.secured ||
	    hid_state.suspended || hid_state.passkey_visible ||
	    k_uptime_get_32() - mouse_pending_since > 100U) {
		clear_mouse_locked();
	}
	pending = mouse_x != 0 || mouse_y != 0 || mouse_wheel != 0;
	delay = mouse_delay_locked(k_uptime_get_32());
	if (pending && delay > 0U) {
		/* An input sample may have queued this work just before the previous
		 * attempt reserved its time slot. Enforce spacing in the worker too.
		 */
		k_mutex_unlock(&hid_lock);
		k_mutex_unlock(&hid_report_lock);
		schedule_mouse(delay);
		return;
	}
	if (pending && active_connection != NULL) {
		connection = bt_conn_ref(active_connection);
		/* Reserve before dropping hid_lock: an input callback can preempt
		 * bt_gatt_notify(), and must see this slot as already occupied.
		 */
		mouse_last_attempt_ms = k_uptime_get_32();
		mouse_clock_valid = true;
	}
	generation = mouse_generation;
	since = mouse_pending_since;
	report[0] = 0;
	report[1] = (uint8_t)(int8_t)CLAMP(mouse_x, -127, 127);
	report[2] = (uint8_t)(int8_t)CLAMP(mouse_y, -127, 127);
	report[3] = (uint8_t)(int8_t)CLAMP(mouse_wheel, -127, 127);
	mouse_x -= (int8_t)report[1];
	mouse_y -= (int8_t)report[2];
	mouse_wheel -= (int8_t)report[3];
	k_mutex_unlock(&hid_lock);
	if (connection != NULL) {
		const struct bt_gatt_attr *attribute =
			&papermono_hid_service.attrs[HID_ATTR_MOUSE_VALUE];
		/* Keep this connection referenced through submission. A queued move
		 * must never be redirected to a newly connected host.
		 */
		ret = bt_gatt_is_subscribed(connection, attribute,
			BT_GATT_CCC_NOTIFY) ? bt_gatt_notify(connection, attribute,
				report, sizeof(report)) : -EACCES;
	}
	k_mutex_lock(&hid_lock, K_FOREVER);
	if (connection != NULL && generation == mouse_generation) {
		if (ret == -ENOMEM || ret == -EAGAIN) {
			accumulate_mouse(&mouse_x, (int8_t)report[1]);
			accumulate_mouse(&mouse_y, (int8_t)report[2]);
			accumulate_mouse(&mouse_wheel, (int8_t)report[3]);
			mouse_pending_since = since;
			++mouse_retries;
		} else if (ret == 0) {
			++mouse_reports;
			hid_state.last_error = 0;
			mouse_pending_since = k_uptime_get_32();
		} else {
			clear_mouse_locked();
			hid_state.last_error = ret;
		}
	}
	pending = mouse_x != 0 || mouse_y != 0 || mouse_wheel != 0;
	delay = mouse_delay_locked(k_uptime_get_32());
	k_mutex_unlock(&hid_lock);
	if (connection != NULL) {
		bt_conn_unref(connection);
	}
	k_mutex_unlock(&hid_report_lock);
	if (pending) {
		schedule_mouse(delay);
	}
}

void papermono_hid_mouse_enable(bool enabled)
{
	k_mutex_lock(&hid_lock, K_FOREVER);
	if (mouse_enabled != enabled) {
		mouse_enabled = enabled;
		clear_mouse_locked();
		mouse_clock_valid = false;
		LOG_INF("BLE touchpad stream %s; reports=%u retries=%u",
			enabled ? "enabled" : "disabled", mouse_reports, mouse_retries);
	}
	k_mutex_unlock(&hid_lock);
}

void papermono_hid_mouse_move(int16_t x, int16_t y, int16_t wheel)
{
	bool pending = false;
	uint32_t delay = 0U;

	k_mutex_lock(&hid_lock, K_FOREVER);
	if (mouse_enabled && hid_state.connected && hid_state.secured &&
	    !hid_state.suspended && !hid_state.passkey_visible) {
		if (mouse_x == 0 && mouse_y == 0 && mouse_wheel == 0) {
			mouse_pending_since = k_uptime_get_32();
		}
		accumulate_mouse(&mouse_x, x);
		accumulate_mouse(&mouse_y, y);
		accumulate_mouse(&mouse_wheel, wheel);
		pending = mouse_x != 0 || mouse_y != 0 || mouse_wheel != 0;
		delay = mouse_delay_locked(k_uptime_get_32());
	}
	k_mutex_unlock(&hid_lock);
	if (pending) {
		/* Idle first motion has no coalescing delay. Later samples share the
		 * next 15 ms slot; schedule() never postpones an existing deadline.
		 */
		schedule_mouse(delay);
	}
}

int papermono_hid_clear_bonds(void)
{
	struct bt_conn *connection;

	k_mutex_lock(&hid_lock, K_FOREVER);
	connection = active_connection == NULL ? NULL :
		bt_conn_ref(active_connection);
	k_mutex_unlock(&hid_lock);
	if (connection != NULL) {
		(void)bt_conn_disconnect(connection,
			BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(connection);
	}
	int ret = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	if (ret == 0) {
		k_mutex_lock(&hid_lock, K_FOREVER);
		hid_state.bonded = false;
		hid_state.secured = false;
		hid_state.suspended = false;
		hid_state.passkey_visible = false;
		k_mutex_unlock(&hid_lock);
		(void)k_work_submit(&advertising_work);
		papermono_ui_request_background_refresh();
	}
	return ret;
}

int papermono_hid_set_battery_level(uint8_t percent)
{
	return bt_bas_set_battery_level(MIN(percent, 100U));
}

void papermono_hid_get_state(struct papermono_hid_state *state)
{
	if (state == NULL) {
		return;
	}
	k_mutex_lock(&hid_lock, K_FOREVER);
	*state = hid_state;
	k_mutex_unlock(&hid_lock);
}
