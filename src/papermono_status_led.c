/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stddef.h>

#include <papermono/status_led.h>

enum papermono_status_led_state papermono_status_led_resolve(
	const struct papermono_status_led_snapshot *snapshot)
{
	if (snapshot == NULL || snapshot->sleeping) {
		return PAPERMONO_STATUS_LED_OFF;
	}
	if (snapshot->startup_fault || snapshot->runtime_fault ||
	    snapshot->hard_error) {
		return PAPERMONO_STATUS_LED_ERROR;
	}
	if (snapshot->warning) {
		return PAPERMONO_STATUS_LED_WARNING;
	}
	if (snapshot->busy) {
		return PAPERMONO_STATUS_LED_BUSY;
	}
	if (snapshot->pairing) {
		return PAPERMONO_STATUS_LED_PAIRING;
	}
	if (snapshot->wifi_connected || snapshot->ble_connected) {
		return PAPERMONO_STATUS_LED_CONNECTED;
	}
	if (snapshot->provisioning) {
		return PAPERMONO_STATUS_LED_CONNECTING;
	}
	return snapshot->booting ? PAPERMONO_STATUS_LED_BOOTING :
		PAPERMONO_STATUS_LED_READY;
}

int papermono_status_led_color(enum papermono_status_led_state state,
	struct papermono_status_led_color *color)
{
	if (color == NULL || state < PAPERMONO_STATUS_LED_OFF ||
	    state > PAPERMONO_STATUS_LED_ERROR) {
		return -EINVAL;
	}
	/* Red is a binary M5PM1 channel; only exceptional or short-lived states
	 * use it. Green and blue are PWM channels and stay dim during normal use.
	 */
	static const struct papermono_status_led_color colors[] = {
		[PAPERMONO_STATUS_LED_OFF] = { 0U, 0U, 0U, 0U },
		[PAPERMONO_STATUS_LED_BOOTING] = { 255U, 255U, 255U, 100U },
		[PAPERMONO_STATUS_LED_READY] = { 0U, 255U, 0U, 10U },
		[PAPERMONO_STATUS_LED_CONNECTING] = { 0U, 0U, 255U, 30U },
		[PAPERMONO_STATUS_LED_CONNECTED] = { 0U, 255U, 255U, 20U },
		[PAPERMONO_STATUS_LED_BUSY] = { 255U, 255U, 0U, 100U },
		[PAPERMONO_STATUS_LED_PAIRING] = { 255U, 0U, 255U, 100U },
		[PAPERMONO_STATUS_LED_WARNING] = { 255U, 64U, 0U, 100U },
		[PAPERMONO_STATUS_LED_ERROR] = { 255U, 0U, 0U, 100U },
	};

	*color = colors[state];
	return 0;
}

const char *papermono_status_led_state_name(
	enum papermono_status_led_state state)
{
	switch (state) {
	case PAPERMONO_STATUS_LED_OFF:
		return "off";
	case PAPERMONO_STATUS_LED_BOOTING:
		return "booting";
	case PAPERMONO_STATUS_LED_READY:
		return "ready";
	case PAPERMONO_STATUS_LED_CONNECTING:
		return "connecting";
	case PAPERMONO_STATUS_LED_CONNECTED:
		return "connected";
	case PAPERMONO_STATUS_LED_BUSY:
		return "busy";
	case PAPERMONO_STATUS_LED_PAIRING:
		return "pairing";
	case PAPERMONO_STATUS_LED_WARNING:
		return "warning";
	case PAPERMONO_STATUS_LED_ERROR:
		return "error";
	default:
		return "invalid";
	}
}

#if defined(__ZEPHYR__)

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <papermono/content_store.h>
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
#include <papermono/content_server.h>
#endif
#if defined(CONFIG_PAPERMONO_BLE_HID)
#include <papermono/hid.h>
#endif
#if defined(CONFIG_PAPERMONO_BLE_SYNC)
#include <papermono/sync.h>
#endif

LOG_MODULE_REGISTER(papermono_status_led, LOG_LEVEL_INF);

/* LED updates execute on sysworkq and enter the blocking PMIC/I2C path.
 * Catch profile overrides that reintroduce the measured 1 KiB overflow.
 */
BUILD_ASSERT(CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE >= 4096,
	"PaperMono PMIC/LED work requires at least a 4 KiB system workqueue stack");

static const struct device *const status_led = DEVICE_DT_GET(DT_ALIAS(led0));
K_MUTEX_DEFINE(status_led_lock);
static atomic_t initialized;
static atomic_t ready;
static atomic_t startup_fault;
static atomic_t runtime_fault;
static atomic_t sleeping;
static enum papermono_status_led_state current_state =
	PAPERMONO_STATUS_LED_OFF;
/* Guarded by status_led_lock. A failed multi-channel write invalidates the
 * physical output even when the desired logical state did not change.
 */
static bool output_valid;

static int status_led_apply_locked(enum papermono_status_led_state state)
{
	struct papermono_status_led_color color;
	uint8_t channels[3];
	int ret;

	if (!device_is_ready(status_led)) {
		return -ENODEV;
	}
	if (state == current_state && output_valid) {
		return 0;
	}
	ret = papermono_status_led_color(state, &color);
	if (ret < 0) {
		return ret;
	}
	channels[0] = color.red;
	channels[1] = color.green;
	channels[2] = color.blue;
	if (state == PAPERMONO_STATUS_LED_OFF) {
		ret = led_off(status_led, 0U);
	} else {
		/* Going dark first prevents a transient mixed color while the three
		 * physical channels are updated independently.
		 */
		ret = led_set_brightness(status_led, 0U, 0U);
		ret = ret ?: led_set_color(status_led, 0U, 3U, channels);
		ret = ret ?: led_set_brightness(status_led, 0U, color.brightness);
	}
	if (ret == 0) {
		current_state = state;
		output_valid = true;
		LOG_INF("Device status LED: %s", papermono_status_led_state_name(state));
	} else {
		output_valid = false;
	}
	return ret;
}

static struct papermono_status_led_snapshot status_led_snapshot(void)
{
	struct papermono_status_led_snapshot snapshot = {
		.booting = atomic_get(&ready) == 0,
		.startup_fault = atomic_get(&startup_fault) != 0,
		.runtime_fault = atomic_get(&runtime_fault) != 0,
		.sleeping = atomic_get(&sleeping) != 0,
	};
	const enum papermono_media_state media =
		papermono_content_store_media_state();

	snapshot.hard_error = media == PAPERMONO_MEDIA_ERROR;
	snapshot.warning = media == PAPERMONO_MEDIA_UNFORMATTED;
#if defined(CONFIG_PAPERMONO_CONTENT_SERVER)
	struct papermono_content_server_state network;

	papermono_content_server_get_state(&network);
	snapshot.busy = papermono_content_server_busy();
	snapshot.provisioning = network.ap_enabled || network.station_connecting;
	snapshot.wifi_connected = network.station_connected;
#endif
#if defined(CONFIG_PAPERMONO_BLE_HID)
	struct papermono_hid_state bluetooth;

	papermono_hid_get_state(&bluetooth);
	snapshot.pairing = bluetooth.passkey_visible;
	snapshot.ble_connected = bluetooth.connected && bluetooth.secured;
#endif
#if defined(CONFIG_PAPERMONO_BLE_SYNC)
	struct papermono_sync_state sync;

	papermono_sync_get_state(&sync);
	snapshot.busy = snapshot.busy || sync.active;
#endif
	return snapshot;
}

static void status_led_worker(struct k_work *work)
{
	const struct papermono_status_led_snapshot snapshot =
		status_led_snapshot();
	const enum papermono_status_led_state state =
		papermono_status_led_resolve(&snapshot);

	ARG_UNUSED(work);
	k_mutex_lock(&status_led_lock, K_FOREVER);
	if (atomic_get(&sleeping) == 0) {
		const int ret = status_led_apply_locked(state);

		if (ret < 0) {
			LOG_WRN("Unable to update device status LED (%d)", ret);
		}
	}
	k_mutex_unlock(&status_led_lock);
}

K_WORK_DEFINE(status_led_work, status_led_worker);

int papermono_status_led_init(void)
{
	int ret;

	if (!device_is_ready(status_led)) {
		return -ENODEV;
	}
	k_mutex_lock(&status_led_lock, K_FOREVER);
	atomic_set(&initialized, 1);
	atomic_clear(&ready);
	atomic_clear(&startup_fault);
	atomic_clear(&runtime_fault);
	atomic_clear(&sleeping);
	output_valid = false;
	ret = status_led_apply_locked(PAPERMONO_STATUS_LED_BOOTING);
	k_mutex_unlock(&status_led_lock);
	return ret;
}

void papermono_status_led_mark_ready(void)
{
	atomic_set(&ready, 1);
	papermono_status_led_request_refresh();
}

void papermono_status_led_request_refresh(void)
{
	if (atomic_get(&initialized) != 0 && atomic_get(&sleeping) == 0) {
		(void)k_work_submit(&status_led_work);
	}
}

void papermono_status_led_report_startup_fault(bool fault)
{
	atomic_set(&startup_fault, fault ? 1 : 0);
	papermono_status_led_request_refresh();
}

void papermono_status_led_report_runtime_fault(bool fault)
{
	atomic_set(&runtime_fault, fault ? 1 : 0);
	papermono_status_led_request_refresh();
}

int papermono_status_led_prepare_sleep(void)
{
	int ret;

	atomic_set(&sleeping, 1);
	k_mutex_lock(&status_led_lock, K_FOREVER);
	ret = device_is_ready(status_led) ?
		status_led_apply_locked(PAPERMONO_STATUS_LED_OFF) : -ENODEV;
	k_mutex_unlock(&status_led_lock);
	return ret;
}

void papermono_status_led_resume(void)
{
	atomic_clear(&sleeping);
	papermono_status_led_request_refresh();
}

enum papermono_status_led_state papermono_status_led_get_state(void)
{
	enum papermono_status_led_state state;

	k_mutex_lock(&status_led_lock, K_FOREVER);
	state = current_state;
	k_mutex_unlock(&status_led_lock);
	return state;
}

#else

int papermono_status_led_init(void)
{
	return -ENOTSUP;
}

void papermono_status_led_mark_ready(void) {}
void papermono_status_led_request_refresh(void) {}
void papermono_status_led_report_startup_fault(bool fault) { (void)fault; }
void papermono_status_led_report_runtime_fault(bool fault) { (void)fault; }
int papermono_status_led_prepare_sleep(void) { return -ENOTSUP; }
void papermono_status_led_resume(void) {}
enum papermono_status_led_state papermono_status_led_get_state(void)
{
	return PAPERMONO_STATUS_LED_OFF;
}

#endif
