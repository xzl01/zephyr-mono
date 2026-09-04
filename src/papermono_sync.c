/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <papermono/content_store.h>
#include <papermono/organizer.h>
#include <papermono/status_led.h>
#include <papermono/sync.h>
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_sync, LOG_LEVEL_INF);

#define SYNC_OP_START 1U
#define SYNC_OP_COMMIT 2U
#define SYNC_OP_ABORT 3U
#define SYNC_KIND_ICS 1U
#define SYNC_KIND_TASKS 2U
#define SYNC_KIND_NOTE 3U
#define SYNC_KIND_STATE 4U
#define SYNC_STATE_LIMIT 8192U
#define SYNC_MESSAGE_DATA_MAX 220U
#define SYNC_NAME_MAX 80U

enum sync_message_type {
	SYNC_MESSAGE_CONTROL,
	SYNC_MESSAGE_DATA,
	SYNC_MESSAGE_DISCONNECT,
};

struct sync_message {
	struct bt_conn *connection;
	uint16_t length;
	uint8_t type;
	uint8_t data[SYNC_MESSAGE_DATA_MAX];
};

static struct papermono_content_transaction transaction;
static char state_buffer[SYNC_STATE_LIMIT + 1U]
#if defined(CONFIG_ESP_SPIRAM)
	__attribute__((section(".ext_ram.bss")))
#endif
;
static bool state_transfer;
static bool state_verify_crc;
static uint32_t state_expected_crc;
static uint32_t state_crc;
static struct papermono_sync_state sync_state;
K_MUTEX_DEFINE(ble_sync_lock);
K_MSGQ_DEFINE(sync_messages, sizeof(struct sync_message), 8, 4);

#define BT_UUID_PAPERMONO_SYNC_SERVICE_VAL \
	BT_UUID_128_ENCODE(0xc1530001, 0x6f4d, 0x4f6e, 0x6f53, 0x796e63000001)
#define BT_UUID_PAPERMONO_SYNC_CONTROL_VAL \
	BT_UUID_128_ENCODE(0xc1530002, 0x6f4d, 0x4f6e, 0x6f53, 0x796e63000001)
#define BT_UUID_PAPERMONO_SYNC_DATA_VAL \
	BT_UUID_128_ENCODE(0xc1530003, 0x6f4d, 0x4f6e, 0x6f53, 0x796e63000001)
#define BT_UUID_PAPERMONO_SYNC_STATUS_VAL \
	BT_UUID_128_ENCODE(0xc1530004, 0x6f4d, 0x4f6e, 0x6f53, 0x796e63000001)

static struct bt_uuid_128 sync_service_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_SYNC_SERVICE_VAL);
static struct bt_uuid_128 sync_control_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_SYNC_CONTROL_VAL);
static struct bt_uuid_128 sync_data_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_SYNC_DATA_VAL);
static struct bt_uuid_128 sync_status_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_SYNC_STATUS_VAL);

static bool status_subscribed;

static int enqueue_message(struct bt_conn *connection,
	enum sync_message_type type, const void *data, uint16_t length)
{
	if (length > SYNC_MESSAGE_DATA_MAX) {
		return -EMSGSIZE;
	}
	struct sync_message message = {
		.connection = connection == NULL ? NULL : bt_conn_ref(connection),
		.length = length,
		.type = (uint8_t)type,
	};
	if (length > 0U) {
		memcpy(message.data, data, length);
	}
	int ret = k_msgq_put(&sync_messages, &message, K_NO_WAIT);
	if (ret < 0 && message.connection != NULL) {
		bt_conn_unref(message.connection);
	}
	return ret;
}

static ssize_t write_control(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, const void *buffer, uint16_t length,
	uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attribute);
	ARG_UNUSED(flags);
	if (offset != 0U || length == 0U ||
	    enqueue_message(connection, SYNC_MESSAGE_CONTROL, buffer, length) < 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}
	return length;
}

static ssize_t write_data(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, const void *buffer, uint16_t length,
	uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attribute);
	ARG_UNUSED(flags);
	if (offset != 0U || length == 0U ||
	    enqueue_message(connection, SYNC_MESSAGE_DATA, buffer, length) < 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}
	return length;
}

static void status_ccc_changed(const struct bt_gatt_attr *attribute,
	uint16_t value)
{
	ARG_UNUSED(attribute);
	status_subscribed = value == BT_GATT_CCC_NOTIFY;
}

BT_GATT_SERVICE_DEFINE(papermono_sync_service,
	BT_GATT_PRIMARY_SERVICE(&sync_service_uuid),
	BT_GATT_CHARACTERISTIC(&sync_control_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE_AUTHEN, NULL, write_control, NULL),
	BT_GATT_CHARACTERISTIC(&sync_data_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE_AUTHEN, NULL, write_data, NULL),
	BT_GATT_CHARACTERISTIC(&sync_status_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed,
		BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_WRITE_AUTHEN));

static void notify_status(struct bt_conn *connection, uint8_t code, int error)
{
	uint8_t report[11];
	uint64_t received;
	uint64_t expected;

	k_mutex_lock(&ble_sync_lock, K_FOREVER);
	received = sync_state.received;
	expected = sync_state.expected;
	k_mutex_unlock(&ble_sync_lock);
	report[0] = code;
	sys_put_le16((uint16_t)(int16_t)error, &report[1]);
	sys_put_le32((uint32_t)received, &report[3]);
	sys_put_le32((uint32_t)expected, &report[7]);
	if (connection != NULL && status_subscribed) {
		(void)bt_gatt_notify(connection,
			&papermono_sync_service.attrs[6], report, sizeof(report));
	}
}

static enum papermono_content_kind content_kind(uint8_t kind)
{
	return kind == SYNC_KIND_ICS ? PAPERMONO_CONTENT_ICS :
		kind == SYNC_KIND_TASKS ? PAPERMONO_CONTENT_TASKS :
		kind == SYNC_KIND_NOTE ? PAPERMONO_CONTENT_NOTE :
		PAPERMONO_CONTENT_UNKNOWN;
}

static int start_transfer(const struct sync_message *message)
{
	if (message->length < 11U) {
		return -EBADMSG;
	}
	const enum papermono_content_kind kind = content_kind(message->data[1]);
	const bool is_state = message->data[1] == SYNC_KIND_STATE;
	const uint32_t size = sys_get_le32(&message->data[2]);
	const uint32_t crc = sys_get_le32(&message->data[6]);
	const uint8_t name_length = message->data[10];
	char name[SYNC_NAME_MAX + 1U];

	if ((!is_state && kind == PAPERMONO_CONTENT_UNKNOWN) ||
	    (is_state && size > SYNC_STATE_LIMIT) || name_length == 0U ||
	    name_length > SYNC_NAME_MAX || 11U + name_length != message->length) {
		return -EINVAL;
	}
	memcpy(name, &message->data[11], name_length);
	name[name_length] = '\0';
	if (transaction.active) {
		(void)papermono_content_store_import_abort(&transaction);
	}
	state_transfer = is_state;
	state_verify_crc = is_state && crc != 0U;
	state_expected_crc = crc;
	state_crc = 0U;
	int ret = is_state ? 0 : papermono_content_store_import_begin(
		&transaction, kind, name, size, crc != 0U, crc);
	k_mutex_lock(&ble_sync_lock, K_FOREVER);
	sync_state.expected = size;
	sync_state.received = 0U;
	sync_state.active = ret == 0;
	sync_state.last_error = ret;
	k_mutex_unlock(&ble_sync_lock);
	papermono_status_led_request_refresh();
	return ret;
}

static int handle_control(const struct sync_message *message)
{
	if (message->length == 0U) {
		return -EBADMSG;
	}
	if (message->data[0] == SYNC_OP_START) {
		return start_transfer(message);
	}
	if (message->data[0] == SYNC_OP_ABORT) {
		int ret = transaction.active ?
			papermono_content_store_import_abort(&transaction) : 0;
		state_transfer = false;
		state_verify_crc = false;
		k_mutex_lock(&ble_sync_lock, K_FOREVER);
		sync_state.active = false;
		sync_state.last_error = ret;
		k_mutex_unlock(&ble_sync_lock);
		papermono_status_led_request_refresh();
		return ret;
	}
	if (message->data[0] == SYNC_OP_COMMIT) {
		if (!transaction.active && !state_transfer) {
			return -EINVAL;
		}
		int ret;
		if (state_transfer) {
			uint64_t received;
			uint64_t expected;

			k_mutex_lock(&ble_sync_lock, K_FOREVER);
			received = sync_state.received;
			expected = sync_state.expected;
			k_mutex_unlock(&ble_sync_lock);
			state_buffer[received] = '\0';
			ret = received == expected &&
				(!state_verify_crc || state_crc == state_expected_crc) ?
				papermono_organizer_import_state_json(state_buffer,
					(size_t)received) : -EBADMSG;
			state_transfer = false;
			state_verify_crc = false;
		} else {
			ret = papermono_content_store_import_commit(&transaction);
		}
		k_mutex_lock(&ble_sync_lock, K_FOREVER);
		sync_state.active = false;
		sync_state.last_error = ret;
		k_mutex_unlock(&ble_sync_lock);
		papermono_status_led_request_refresh();
		if (ret == 0) {
			papermono_ui_request_content_refresh();
		}
		return ret;
	}
	return -ENOTSUP;
}

static void sync_worker(void *first, void *second, void *third)
{
	ARG_UNUSED(first);
	ARG_UNUSED(second);
	ARG_UNUSED(third);
	for (;;) {
		struct sync_message message;
		int ret = k_msgq_get(&sync_messages, &message, K_FOREVER);

		if (ret < 0) {
			continue;
		}
		uint8_t code = 0U;
		if (message.type == SYNC_MESSAGE_DISCONNECT) {
			ret = transaction.active ?
				papermono_content_store_import_abort(&transaction) : 0;
			state_transfer = false;
			state_verify_crc = false;
			k_mutex_lock(&ble_sync_lock, K_FOREVER);
			sync_state.active = false;
			sync_state.last_error = ret;
			k_mutex_unlock(&ble_sync_lock);
			papermono_status_led_request_refresh();
		} else if (message.type == SYNC_MESSAGE_CONTROL) {
			ret = handle_control(&message);
			code = message.data[0];
		} else if (!transaction.active && !state_transfer) {
			ret = -EINVAL;
		} else if (state_transfer) {
			k_mutex_lock(&ble_sync_lock, K_FOREVER);
			if (sync_state.received + message.length > sync_state.expected ||
			    sync_state.received + message.length > SYNC_STATE_LIMIT) {
				ret = -EFBIG;
			} else {
				memcpy(state_buffer + sync_state.received, message.data,
					message.length);
				state_crc = crc32_ieee_update(state_crc, message.data,
					message.length);
				sync_state.received += message.length;
				ret = 0;
			}
			sync_state.last_error = ret;
			k_mutex_unlock(&ble_sync_lock);
		} else {
			ret = papermono_content_store_import_write(&transaction,
				message.data, message.length);
			k_mutex_lock(&ble_sync_lock, K_FOREVER);
			if (ret == 0) {
				sync_state.received += message.length;
			}
			sync_state.last_error = ret;
			k_mutex_unlock(&ble_sync_lock);
		}
		if (ret < 0 || message.type == SYNC_MESSAGE_CONTROL) {
			notify_status(message.connection, code, ret);
		}
		if (message.connection != NULL) {
			bt_conn_unref(message.connection);
		}
	}
}

K_THREAD_DEFINE(sync_thread, 4096, sync_worker, NULL, NULL, NULL, 7, 0, 0);

static void sync_disconnected(struct bt_conn *connection, uint8_t reason)
{
	ARG_UNUSED(reason);
	(void)enqueue_message(connection, SYNC_MESSAGE_DISCONNECT, NULL, 0U);
}

BT_CONN_CB_DEFINE(sync_connection_callbacks) = {
	.disconnected = sync_disconnected,
};

int papermono_sync_init(void)
{
	k_mutex_lock(&ble_sync_lock, K_FOREVER);
	sync_state.initialized = true;
	k_mutex_unlock(&ble_sync_lock);
	LOG_INF("Authenticated BLE small-data sync ready");
	return 0;
}

void papermono_sync_get_state(struct papermono_sync_state *state)
{
	if (state == NULL) {
		return;
	}
	k_mutex_lock(&ble_sync_lock, K_FOREVER);
	*state = sync_state;
	k_mutex_unlock(&ble_sync_lock);
}
