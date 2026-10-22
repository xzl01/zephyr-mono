/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <papermono/adpcm.h>
#include <papermono/ble_mic.h>
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
#include <papermono/content_server.h>
#endif
#include <papermono/pdm_mic.h>
#include <papermono/ui.h>

#if defined(CONFIG_ESP32_SW_COEXIST_ENABLE)
#include <esp_coexist.h>
#include <private/esp_coexist_internal.h>
#endif

LOG_MODULE_REGISTER(papermono_ble_mic, CONFIG_PAPERMONO_BLE_MIC_LOG_LEVEL);

/* ATVV v1.0 carries 240 samples (15 ms at 16 kHz) as 120 ADPCM bytes. */
#define MIC_FRAME_SAMPLES 240U
#define MIC_FRAME_BYTES (MIC_FRAME_SAMPLES / 2U)
#define MIC_SYNC_INTERVAL_FRAMES 50U
#define MIC_BLOCK_SAMPLES 256U
#define MIC_ATT_OVERHEAD 3U
#define MIC_MIN_CHUNK 20U
#define MIC_SESSION_LIMIT_MS CONFIG_PAPERMONO_BLE_MIC_SESSION_LIMIT_MS
#define MIC_ABORT_TIMEOUT_MS 1000

#define MIC_OP_GET_CAPS 0x0AU
#define MIC_OP_MIC_OPEN 0x0CU
#define MIC_OP_MIC_CLOSE 0x0DU
#define MIC_OP_MIC_EXTEND 0x0EU

#define MIC_EVT_AUDIO_STOP 0x00U
#define MIC_EVT_AUDIO_START 0x04U
#define MIC_EVT_AUDIO_SYNC 0x0AU
#define MIC_EVT_CAPS 0x0BU
#define MIC_EVT_MIC_OPEN_ERROR 0x0CU

#define MIC_CODEC_ADPCM_16K 0x02U
#define MIC_INTERACTION_HOLD 0x03U

#define MIC_ERROR_ENCRYPTION 0x0001U
#define MIC_ERROR_SUBSCRIPTION 0x0002U
#define MIC_ERROR_DEVICE 0x0003U

#define BT_UUID_PAPERMONO_MIC_SERVICE_VAL \
	BT_UUID_128_ENCODE(0xab5e0001, 0x5a21, 0x4f05, 0xbc7d, 0xaf01f617b664)
#define BT_UUID_PAPERMONO_MIC_TX_VAL \
	BT_UUID_128_ENCODE(0xab5e0002, 0x5a21, 0x4f05, 0xbc7d, 0xaf01f617b664)
#define BT_UUID_PAPERMONO_MIC_AUDIO_VAL \
	BT_UUID_128_ENCODE(0xab5e0003, 0x5a21, 0x4f05, 0xbc7d, 0xaf01f617b664)
#define BT_UUID_PAPERMONO_MIC_CONTROL_VAL \
	BT_UUID_128_ENCODE(0xab5e0004, 0x5a21, 0x4f05, 0xbc7d, 0xaf01f617b664)

static struct bt_uuid_128 mic_service_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_MIC_SERVICE_VAL);
static struct bt_uuid_128 mic_tx_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_MIC_TX_VAL);
static struct bt_uuid_128 mic_audio_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_MIC_AUDIO_VAL);
static struct bt_uuid_128 mic_control_uuid =
	BT_UUID_INIT_128(BT_UUID_PAPERMONO_MIC_CONTROL_VAL);

enum mic_attribute_index {
	MIC_ATTR_AUDIO_VALUE = 4U,
	MIC_ATTR_CONTROL_VALUE = 7U,
};

static const struct device *const microphone =
	DEVICE_DT_GET(DT_ALIAS(microphone0));

K_MUTEX_DEFINE(mic_lock);
K_SEM_DEFINE(mic_wake, 0, 1);
K_SEM_DEFINE(mic_idle, 0, 1);
K_THREAD_STACK_DEFINE(mic_stack, CONFIG_PAPERMONO_BLE_MIC_THREAD_STACK_SIZE);
static struct k_thread mic_thread;

static struct papermono_ble_mic_state mic_state;
static struct bt_conn *mic_connection;
static struct bt_conn *caps_connection;
static struct k_work caps_work;

/* Audio and control notifications are handed to the system workqueue. The
 * capture worker must never block in bt_gatt_notify(): that call waits
 * indefinitely for an ATT buffer on a normal thread, and a stalled host would
 * stop PDM reads long enough for the I2S RX queue to overflow (-EIO). The
 * system workqueue path allocates with K_NO_WAIT instead, so a full buffer
 * drops one frame rather than killing the capture stream.
 */
enum mic_tx_kind {
	MIC_TX_AUDIO = 0,
	MIC_TX_CONTROL = 1,
};

struct mic_tx_message {
	uint8_t data[MIC_FRAME_BYTES];
	uint8_t length;
	uint8_t kind;
	uint8_t reserved[2];
};

K_MSGQ_DEFINE(mic_tx_queue, sizeof(struct mic_tx_message), 8, 4);
static struct k_work_delayable mic_tx_work;

static bool initialized;
static bool audio_subscribed;
static bool control_subscribed;
static bool start_pending;
static bool stop_pending;
static bool abort_pending;

static struct papermono_adpcm_state encoder;
static int16_t pcm_block[MIC_BLOCK_SAMPLES];
static int16_t frame_pcm[MIC_FRAME_SAMPLES];
static uint8_t frame_adpcm[MIC_FRAME_BYTES];
static size_t frame_fill;
static uint16_t frame_sequence;
static uint8_t session_sequence;
static uint32_t session_started_ms;
static uint32_t session_dropped_start;
static uint32_t tx_notified;
static uint32_t tx_failed;
static uint32_t discard_blocks;

static void update_subscribed_locked(void)
{
	mic_state.subscribed = audio_subscribed && control_subscribed;
}

static struct bt_conn *connection_ref(void)
{
	struct bt_conn *connection;

	k_mutex_lock(&mic_lock, K_FOREVER);
	connection = mic_connection == NULL ? NULL : bt_conn_ref(mic_connection);
	k_mutex_unlock(&mic_lock);
	return connection;
}

static void request_fast_connection(struct bt_conn *connection);
static ssize_t write_tx(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, const void *buffer, uint16_t length,
	uint16_t offset, uint8_t flags);
static void audio_ccc_changed(const struct bt_gatt_attr *attribute,
	uint16_t value);
static void control_ccc_changed(const struct bt_gatt_attr *attribute,
	uint16_t value);

BT_GATT_SERVICE_DEFINE(papermono_ble_mic_service,
	BT_GATT_PRIMARY_SERVICE(&mic_service_uuid),
	BT_GATT_CHARACTERISTIC(&mic_tx_uuid.uuid,
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_tx, NULL),
	BT_GATT_CHARACTERISTIC(&mic_audio_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(audio_ccc_changed,
		BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_CHARACTERISTIC(&mic_control_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(control_ccc_changed,
		BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
);

static int notify_control(struct bt_conn *connection, const uint8_t *data,
	size_t length)
{
	if (connection == NULL) {
		return -ENOTCONN;
	}
	if (!bt_gatt_is_subscribed(connection,
		&papermono_ble_mic_service.attrs[MIC_ATTR_CONTROL_VALUE],
		BT_GATT_CCC_NOTIFY)) {
		return -EACCES;
	}
	return bt_gatt_notify(connection,
		&papermono_ble_mic_service.attrs[MIC_ATTR_CONTROL_VALUE],
		data, length);
}

static int notify_audio(const uint8_t *data, size_t length)
{
	struct bt_conn *connection = connection_ref();
	uint16_t mtu;
	size_t chunk;
	int ret = 0;

	if (connection == NULL) {
		return -ENOTCONN;
	}
	if (!bt_gatt_is_subscribed(connection,
		&papermono_ble_mic_service.attrs[MIC_ATTR_AUDIO_VALUE],
		BT_GATT_CCC_NOTIFY)) {
		bt_conn_unref(connection);
		return -EACCES;
	}

	/* A host that negotiated a small ATT MTU still receives the same byte
	 * stream; the receiver's frame accumulator reassembles 120-byte frames.
	 */
	mtu = bt_gatt_get_mtu(connection);
	chunk = mtu > MIC_ATT_OVERHEAD ? (size_t)mtu - MIC_ATT_OVERHEAD :
		MIC_MIN_CHUNK;
	if (chunk > length) {
		chunk = length;
	}
	for (size_t offset = 0U; offset < length; offset += chunk) {
		const size_t part = MIN(chunk, length - offset);

		ret = bt_gatt_notify(connection,
			&papermono_ble_mic_service.attrs[MIC_ATTR_AUDIO_VALUE],
			data + offset, part);
		if (ret < 0) {
			break;
		}
	}
	bt_conn_unref(connection);
	if (ret == 0) {
		tx_notified++;
	} else {
		tx_failed++;
	}
	return ret;
}

static void mic_tx_work_handler(struct k_work *work)
{
	struct mic_tx_message message;

	ARG_UNUSED(work);
	/* One notification per 15 ms connection interval. Sending a whole burst
	 * fills the shared ATT pool instantly and the tail is dropped; pacing
	 * matches the link and keeps one frame per event instead of losing most
	 * of a burst when the host or Wi-Fi coexistence delays an event.
	 */
	if (k_msgq_get(&mic_tx_queue, &message, K_NO_WAIT) == 0) {
		if (message.kind == MIC_TX_AUDIO) {
			(void)notify_audio(message.data, message.length);
		} else {
			struct bt_conn *connection = connection_ref();

			if (connection != NULL) {
				(void)notify_control(connection, message.data,
					message.length);
				bt_conn_unref(connection);
			}
		}
	}
	if (k_msgq_num_used_get(&mic_tx_queue) > 0U) {
		(void)k_work_reschedule(&mic_tx_work, K_MSEC(15));
	}
}

static int mic_tx_enqueue(enum mic_tx_kind kind, const uint8_t *data,
	size_t length)
{
	struct mic_tx_message message;
	int ret;

	if (length > sizeof(message.data)) {
		return -EMSGSIZE;
	}
	message.kind = (uint8_t)kind;
	message.length = (uint8_t)length;
	memcpy(message.data, data, length);
	ret = k_msgq_put(&mic_tx_queue, &message, K_NO_WAIT);
	if (ret == 0) {
		(void)k_work_reschedule(&mic_tx_work, K_NO_WAIT);
	}
	return ret;
}

static int send_caps(struct bt_conn *connection)
{
	static const uint8_t caps[9] = {
		MIC_EVT_CAPS,
		0x01U, 0x00U, /* ATVV 1.0 */
		MIC_CODEC_ADPCM_16K,
		MIC_INTERACTION_HOLD,
		0x00U, MIC_FRAME_BYTES,
		0x00U, 0x00U,
	};

	ARG_UNUSED(connection);
	return mic_tx_enqueue(MIC_TX_CONTROL, caps, sizeof(caps));
}

static int send_audio_start(struct bt_conn *connection, uint8_t session)
{
	const uint8_t event[4] = {
		MIC_EVT_AUDIO_START,
		0x00U,
		MIC_CODEC_ADPCM_16K,
		session,
	};

	ARG_UNUSED(connection);
	return mic_tx_enqueue(MIC_TX_CONTROL, event, sizeof(event));
}

static int send_audio_sync(struct bt_conn *connection)
{
	uint8_t event[7] = {
		MIC_EVT_AUDIO_SYNC,
		MIC_CODEC_ADPCM_16K,
		0x00U, 0x00U,
		0x00U, 0x00U,
		0x00U,
	};

	sys_put_be16(frame_sequence, &event[2]);
	sys_put_be16((uint16_t)(int16_t)encoder.predictor, &event[4]);
	event[6] = encoder.step_index;
	ARG_UNUSED(connection);
	return mic_tx_enqueue(MIC_TX_CONTROL, event, sizeof(event));
}

static int send_audio_stop(struct bt_conn *connection)
{
	const uint8_t event[2] = { MIC_EVT_AUDIO_STOP, 0x00U };

	ARG_UNUSED(connection);
	return mic_tx_enqueue(MIC_TX_CONTROL, event, sizeof(event));
}

static int send_mic_open_error(struct bt_conn *connection, uint16_t code)
{
	uint8_t event[3] = {
		MIC_EVT_MIC_OPEN_ERROR,
		(uint8_t)(code >> 8),
		(uint8_t)(code & 0xFFU),
	};

	ARG_UNUSED(connection);
	return mic_tx_enqueue(MIC_TX_CONTROL, event, sizeof(event));
}

static uint16_t current_open_error_code(void)
{
	uint16_t code;

	k_mutex_lock(&mic_lock, K_FOREVER);
	if (mic_connection == NULL) {
		code = MIC_ERROR_DEVICE;
	} else if (!mic_state.secured) {
		code = MIC_ERROR_ENCRYPTION;
	} else if (!audio_subscribed || !control_subscribed) {
		code = MIC_ERROR_SUBSCRIPTION;
	} else {
		code = MIC_ERROR_DEVICE;
	}
	k_mutex_unlock(&mic_lock);
	return code;
}

static void caps_work_handler(struct k_work *work)
{
	struct bt_conn *connection;

	ARG_UNUSED(work);
	k_mutex_lock(&mic_lock, K_FOREVER);
	connection = caps_connection;
	caps_connection = NULL;
	k_mutex_unlock(&mic_lock);
	if (connection != NULL) {
		(void)send_caps(connection);
		bt_conn_unref(connection);
	}
}

static void schedule_caps(struct bt_conn *connection)
{
	k_mutex_lock(&mic_lock, K_FOREVER);
	if (caps_connection != NULL) {
		bt_conn_unref(caps_connection);
	}
	caps_connection = bt_conn_ref(connection);
	k_mutex_unlock(&mic_lock);
	(void)k_work_submit(&caps_work);
}

static void request_stop_locked(void)
{
	start_pending = false;
	stop_pending = true;
}

static int request_start_internal(void)
{
	int ret = 0;

	k_mutex_lock(&mic_lock, K_FOREVER);
	if (!initialized) {
		ret = -ENODEV;
	} else if (mic_connection == NULL) {
		ret = -ENOTCONN;
	} else if (!mic_state.secured) {
		ret = -EACCES;
	} else if (!audio_subscribed || !control_subscribed) {
		ret = -EACCES;
	} else if (!mic_state.streaming && !start_pending) {
		start_pending = true;
		stop_pending = false;
	}
	if (ret < 0) {
		mic_state.last_error = ret;
	}
	k_mutex_unlock(&mic_lock);
	if (ret == 0) {
		k_sem_give(&mic_wake);
	}
	return ret;
}

static void request_stop_internal(void)
{
	k_mutex_lock(&mic_lock, K_FOREVER);
	request_stop_locked();
	k_mutex_unlock(&mic_lock);
	k_sem_give(&mic_wake);
}

static bool stop_requested(void)
{
	bool requested;

	k_mutex_lock(&mic_lock, K_FOREVER);
	requested = stop_pending || !mic_state.streaming;
	k_mutex_unlock(&mic_lock);
	return requested;
}

static void start_session(void)
{
	struct bt_conn *connection;
	uint8_t session;
	int ret;

	k_mutex_lock(&mic_lock, K_FOREVER);
	if (!initialized || mic_connection == NULL || !mic_state.secured ||
	    !audio_subscribed || !control_subscribed) {
		mic_state.last_error = mic_connection == NULL ? -ENOTCONN : -EACCES;
		mic_state.phase = PAPERMONO_BLE_MIC_IDLE;
		mic_state.streaming = false;
		k_mutex_unlock(&mic_lock);
		papermono_ui_request_background_refresh();
		return;
	}
	connection = bt_conn_ref(mic_connection);
	mic_state.phase = PAPERMONO_BLE_MIC_STARTING;
	k_mutex_unlock(&mic_lock);

#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	/* The C153 shares one 2.4 GHz radio between Wi-Fi and BLE. Measured
	 * 16 kHz ADPCM throughput falls from ~60 kbps to ~40 kbps while the
	 * content-center SoftAP/STA is active, so suspend Wi-Fi for the session.
	 */
	(void)papermono_content_server_pause_wifi();
#endif
	ret = papermono_pdm_mic_start(microphone);
	if (ret < 0) {
		k_mutex_lock(&mic_lock, K_FOREVER);
		mic_state.last_error = ret;
		mic_state.phase = PAPERMONO_BLE_MIC_IDLE;
		mic_state.streaming = false;
		k_mutex_unlock(&mic_lock);
		LOG_ERR("PDM microphone start failed (%d)", ret);
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
		(void)papermono_content_server_resume_wifi();
#endif
		bt_conn_unref(connection);
		papermono_ui_request_background_refresh();
		return;
	}

	papermono_adpcm_reset(&encoder);
	frame_fill = 0U;
	frame_sequence = 0U;
	discard_blocks = 1U;
	session_started_ms = k_uptime_get_32();
	session = session_sequence++;

	k_mutex_lock(&mic_lock, K_FOREVER);
	mic_state.phase = PAPERMONO_BLE_MIC_STREAMING;
	mic_state.streaming = true;
	mic_state.sessions++;
	mic_state.session_ms = 0U;
	mic_state.session_bytes = 0U;
	mic_state.last_error = 0;
	session_dropped_start = mic_state.frames_dropped;
	k_mutex_unlock(&mic_lock);

	request_fast_connection(connection);
#if defined(CONFIG_ESP32_SW_COEXIST_ENABLE)
	/* Give BLE priority over Wi-Fi for the duration of the voice stream. The
	 * shared 2.4 GHz radio otherwise delivers only about 40 kbps while the
	 * content-center SoftAP/STA is active, which is below 16 kHz ADPCM.
	 */
	(void)coex_preference_set((coex_prefer_t)ESP_COEX_PREFER_BT);
#endif
	k_msgq_purge(&mic_tx_queue);
	(void)send_caps(connection);
	(void)send_audio_start(connection, session);
	/* The first AUDIO_SYNC is emitted by send_frame() immediately before the
	 * first frame (sequence 0), so the host receives exactly one seq=0 sync.
	 */
	LOG_INF("BLE voice session %u started", session);
	bt_conn_unref(connection);
	papermono_ui_request_background_refresh();
}

static void stop_session(void)
{
	struct bt_conn *connection = NULL;
	bool was_streaming;
	uint32_t session_ms;
	uint32_t session_bytes;
	uint32_t dropped;

	k_mutex_lock(&mic_lock, K_FOREVER);
	was_streaming = mic_state.streaming;
	if (was_streaming) {
		mic_state.streaming = false;
		mic_state.phase = PAPERMONO_BLE_MIC_STOPPING;
		connection = mic_connection == NULL ? NULL :
			bt_conn_ref(mic_connection);
	}
	k_mutex_unlock(&mic_lock);

	if (!was_streaming) {
		(void)papermono_pdm_mic_stop(microphone);
		k_mutex_lock(&mic_lock, K_FOREVER);
		mic_state.phase = PAPERMONO_BLE_MIC_IDLE;
		k_mutex_unlock(&mic_lock);
		return;
	}

	if (connection != NULL) {
		(void)send_audio_stop(connection);
		bt_conn_unref(connection);
	}
	(void)papermono_pdm_mic_stop(microphone);
#if defined(CONFIG_ESP32_SW_COEXIST_ENABLE)
	(void)coex_preference_set((coex_prefer_t)ESP_COEX_PREFER_BALANCE);
#endif
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	(void)papermono_content_server_resume_wifi();
#endif

	k_mutex_lock(&mic_lock, K_FOREVER);
	mic_state.session_ms = k_uptime_get_32() - session_started_ms;
	mic_state.phase = PAPERMONO_BLE_MIC_IDLE;
	session_ms = mic_state.session_ms;
	session_bytes = mic_state.session_bytes;
	dropped = mic_state.frames_dropped - session_dropped_start;
	k_mutex_unlock(&mic_lock);
	LOG_INF("BLE voice stopped after %u ms, %u bytes, %u enqueue drops, "
		"%u notified, %u notify failures",
		session_ms, session_bytes, dropped, tx_notified, tx_failed);
	tx_notified = 0U;
	tx_failed = 0U;
	papermono_ui_request_background_refresh();
}

static bool send_frame(void)
{
	bool stop;
	bool too_long;
	int ret;

	k_mutex_lock(&mic_lock, K_FOREVER);
	stop = stop_pending || !mic_state.streaming;
	k_mutex_unlock(&mic_lock);
	if (stop) {
		return false;
	}

	if ((frame_sequence % MIC_SYNC_INTERVAL_FRAMES) == 0U) {
		(void)send_audio_sync(NULL);
	}

	papermono_adpcm_encode(&encoder, frame_pcm, MIC_FRAME_SAMPLES,
		frame_adpcm);
	ret = mic_tx_enqueue(MIC_TX_AUDIO, frame_adpcm, sizeof(frame_adpcm));

	k_mutex_lock(&mic_lock, K_FOREVER);
	if (ret == 0) {
		mic_state.frames_sent++;
		mic_state.session_bytes += MIC_FRAME_BYTES;
		mic_state.last_error = 0;
	} else {
		/* A full TX queue or a busy host is recoverable: the next periodic
		 * AUDIO_SYNC resynchronises the decoder. Never turn one dropped frame
		 * into a stopped session, and never block the PDM reader on BLE.
		 */
		mic_state.frames_dropped++;
	}
	frame_sequence++;
	mic_state.session_ms = k_uptime_get_32() - session_started_ms;
	too_long = mic_state.session_ms >= MIC_SESSION_LIMIT_MS;
	k_mutex_unlock(&mic_lock);

	if (too_long) {
		LOG_INF("BLE voice session reached the %u ms limit",
			MIC_SESSION_LIMIT_MS);
		stop_session();
		return false;
	}
	return true;
}

static void capture_once(void)
{
	size_t count = 0U;
	int ret;

	if (stop_requested()) {
		return;
	}
	ret = papermono_pdm_mic_read(microphone, pcm_block,
		ARRAY_SIZE(pcm_block), &count);
	if (ret < 0) {
		if (ret != -EAGAIN && ret != -EWOULDBLOCK) {
			LOG_WRN("PDM microphone read failed (%d)", ret);
			k_mutex_lock(&mic_lock, K_FOREVER);
			mic_state.last_error = ret;
			k_mutex_unlock(&mic_lock);
			stop_session();
		}
		return;
	}
	if (discard_blocks > 0U) {
		discard_blocks--;
		return;
	}
	if (stop_requested()) {
		return;
	}

	for (size_t index = 0U; index < count; ++index) {
		frame_pcm[frame_fill++] = pcm_block[index];
		if (frame_fill == MIC_FRAME_SAMPLES) {
			frame_fill = 0U;
			if (!send_frame()) {
				return;
			}
		}
	}

	k_mutex_lock(&mic_lock, K_FOREVER);
	mic_state.session_ms = k_uptime_get_32() - session_started_ms;
	k_mutex_unlock(&mic_lock);
}

static void mic_worker(void *first, void *second, void *third)
{
	ARG_UNUSED(first);
	ARG_UNUSED(second);
	ARG_UNUSED(third);

	for (;;) {
		bool start;
		bool stop;
		bool abort;
		bool streaming;

		k_mutex_lock(&mic_lock, K_FOREVER);
		start = start_pending;
		start_pending = false;
		stop = stop_pending;
		stop_pending = false;
		abort = abort_pending;
		abort_pending = false;
		k_mutex_unlock(&mic_lock);

		if (abort) {
			start = false;
		}
		if (stop) {
			stop_session();
		}
		if (start) {
			start_session();
		}

		k_mutex_lock(&mic_lock, K_FOREVER);
		streaming = mic_state.streaming;
		k_mutex_unlock(&mic_lock);
		if (streaming) {
			capture_once();
		} else {
			k_sem_give(&mic_idle);
			(void)k_sem_take(&mic_wake, K_MSEC(500));
		}
	}
}

static void audio_ccc_changed(const struct bt_gatt_attr *attribute,
	uint16_t value)
{
	ARG_UNUSED(attribute);
	k_mutex_lock(&mic_lock, K_FOREVER);
	audio_subscribed = value == BT_GATT_CCC_NOTIFY;
	update_subscribed_locked();
	if (!audio_subscribed) {
		request_stop_locked();
	}
	k_mutex_unlock(&mic_lock);
	k_sem_give(&mic_wake);
	papermono_ui_request_background_refresh();
}

static void control_ccc_changed(const struct bt_gatt_attr *attribute,
	uint16_t value)
{
	ARG_UNUSED(attribute);
	k_mutex_lock(&mic_lock, K_FOREVER);
	control_subscribed = value == BT_GATT_CCC_NOTIFY;
	update_subscribed_locked();
	if (!control_subscribed) {
		request_stop_locked();
	}
	k_mutex_unlock(&mic_lock);
	k_sem_give(&mic_wake);
	papermono_ui_request_background_refresh();
}

static ssize_t write_tx(struct bt_conn *connection,
	const struct bt_gatt_attr *attribute, const void *buffer, uint16_t length,
	uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buffer;
	int ret;

	ARG_UNUSED(attribute);
	ARG_UNUSED(flags);
	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (length == 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (data[0]) {
	case MIC_OP_GET_CAPS:
		schedule_caps(connection);
		break;
	case MIC_OP_MIC_OPEN:
		ret = request_start_internal();
		if (ret < 0) {
			(void)send_mic_open_error(connection,
				current_open_error_code());
		}
		break;
	case MIC_OP_MIC_CLOSE:
		request_stop_internal();
		break;
	case MIC_OP_MIC_EXTEND:
		/* The 60 s device limit still applies to host-requested streams. */
		break;
	default:
		break;
	}
	return length;
}

static void request_fast_connection(struct bt_conn *connection)
{
	static const struct bt_le_conn_param param = {
		.interval_min = 12U, /* 15 ms */
		.interval_max = 12U,
		.latency = 0U,
		.timeout = 400U, /* 4 s supervision timeout */
	};
	int ret = bt_conn_le_param_update(connection, &param);

	LOG_INF("BLE voice requested 15 ms connection interval (%d)", ret);
}

static void mic_connected(struct bt_conn *connection, uint8_t error)
{
	if (error != 0U) {
		return;
	}
	k_mutex_lock(&mic_lock, K_FOREVER);
	if (mic_connection != NULL) {
		bt_conn_unref(mic_connection);
	}
	mic_connection = bt_conn_ref(connection);
	mic_state.connected = true;
	mic_state.secured = bt_conn_get_security(connection) >= BT_SECURITY_L2;
	{
		struct bt_conn_info info;

		if (bt_conn_get_info(connection, &info) == 0) {
			LOG_INF("BLE voice link interval=%u us latency=%u",
				info.le.interval_us, info.le.latency);
		}
	}
	audio_subscribed = false;
	control_subscribed = false;
	update_subscribed_locked();
	k_mutex_unlock(&mic_lock);
	papermono_ui_request_background_refresh();
}

static void mic_disconnected(struct bt_conn *connection, uint8_t reason)
{
	ARG_UNUSED(reason);
	k_mutex_lock(&mic_lock, K_FOREVER);
	if (mic_connection == connection) {
		bt_conn_unref(mic_connection);
		mic_connection = NULL;
	}
	if (caps_connection == connection) {
		bt_conn_unref(caps_connection);
		caps_connection = NULL;
	}
	mic_state.connected = false;
	mic_state.secured = false;
	mic_state.subscribed = false;
	mic_state.streaming = false;
	mic_state.phase = PAPERMONO_BLE_MIC_IDLE;
	audio_subscribed = false;
	control_subscribed = false;
	start_pending = false;
	stop_pending = true;
	k_mutex_unlock(&mic_lock);
	k_msgq_purge(&mic_tx_queue);
	k_sem_give(&mic_wake);
	papermono_ui_request_background_refresh();
}

static void mic_security_changed(struct bt_conn *connection,
	bt_security_t level, enum bt_security_err error)
{
	k_mutex_lock(&mic_lock, K_FOREVER);
	const bool secured = error == BT_SECURITY_ERR_SUCCESS &&
		level >= BT_SECURITY_L2;

	mic_state.secured = secured;
	if (!secured) {
		request_stop_locked();
	}
	k_mutex_unlock(&mic_lock);
	if (secured) {
		request_fast_connection(connection);
	}
	k_sem_give(&mic_wake);
	papermono_ui_request_background_refresh();
}

BT_CONN_CB_DEFINE(papermono_ble_mic_connection_callbacks) = {
	.connected = mic_connected,
	.disconnected = mic_disconnected,
	.security_changed = mic_security_changed,
};

int papermono_ble_mic_init(void)
{
	if (!device_is_ready(microphone)) {
		return -ENODEV;
	}

	k_mutex_lock(&mic_lock, K_FOREVER);
	if (initialized) {
		k_mutex_unlock(&mic_lock);
		return 0;
	}
	initialized = true;
	mic_state.initialized = true;
	mic_state.phase = PAPERMONO_BLE_MIC_IDLE;
	k_mutex_unlock(&mic_lock);

	k_work_init(&caps_work, caps_work_handler);
	k_work_init_delayable(&mic_tx_work, mic_tx_work_handler);
	k_thread_create(&mic_thread, mic_stack,
		K_THREAD_STACK_SIZEOF(mic_stack), mic_worker, NULL, NULL, NULL,
		CONFIG_PAPERMONO_BLE_MIC_THREAD_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&mic_thread, "ble_mic");
	LOG_INF("BLE voice microphone ready: 16 kHz IMA ADPCM, 120-byte frames");
	return 0;
}

int papermono_ble_mic_request_start(void)
{
	return request_start_internal();
}

int papermono_ble_mic_request_stop(void)
{
	request_stop_internal();
	return 0;
}

void papermono_ble_mic_abort(void)
{
	bool active;

	k_mutex_lock(&mic_lock, K_FOREVER);
	start_pending = false;
	stop_pending = true;
	abort_pending = true;
	active = mic_state.streaming ||
		mic_state.phase != PAPERMONO_BLE_MIC_IDLE;
	k_mutex_unlock(&mic_lock);

	k_sem_reset(&mic_idle);
	k_sem_give(&mic_wake);
	if (!active) {
		return;
	}
	if (k_sem_take(&mic_idle, K_MSEC(MIC_ABORT_TIMEOUT_MS)) != 0) {
		LOG_WRN("BLE voice stop timed out; forcing microphone off");
		(void)papermono_pdm_mic_stop(microphone);
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
		(void)papermono_content_server_resume_wifi();
#endif
	}
}

void papermono_ble_mic_get_state(struct papermono_ble_mic_state *state)
{
	if (state == NULL) {
		return;
	}
	k_mutex_lock(&mic_lock, K_FOREVER);
	*state = mic_state;
	k_mutex_unlock(&mic_lock);
}
