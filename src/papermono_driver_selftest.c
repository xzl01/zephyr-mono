/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/fs/fs.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/storage/disk_access.h>

/* Scratch file for the FAT read/write check; removed again afterwards. */
#define PAPERMONO_SD_SELFTEST_PATH "/SD:/papermono/.selftest.tmp"

#include <papermono/content_store.h>
#include <papermono/driver_selftest.h>
#include <papermono/m5ioe1.h>
#include <papermono/m5pm1.h>
#include <papermono/pdm_mic.h>
#include <papermono/ssd1677.h>
#include <papermono/st25r3916.h>

LOG_MODULE_REGISTER(papermono_selftest, LOG_LEVEL_INF);

static const struct device *const ioe = DEVICE_DT_GET(DT_NODELABEL(m5ioe1));
static const struct device *const pmic = DEVICE_DT_GET(DT_NODELABEL(m5pm1));
static const struct device *const touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));
static const struct device *const buttons = DEVICE_DT_GET(DT_PATH(buttons));
static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(imu));
static const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(rtc));
static const struct device *const rgb = DEVICE_DT_GET(DT_ALIAS(led0));
static const struct device *const charger = DEVICE_DT_GET(DT_NODELABEL(charger));
static const struct device *const lora = DEVICE_DT_GET(DT_ALIAS(lora0));
static const struct device *const nfc = DEVICE_DT_GET(DT_NODELABEL(nfc));
static const struct device *const microphone =
	DEVICE_DT_GET(DT_ALIAS(microphone0));
static const struct pwm_dt_spec buzzer =
	PWM_DT_SPEC_GET(DT_NODELABEL(buzzer));
static const struct i2c_dt_spec imu_bus = I2C_DT_SPEC_GET(DT_NODELABEL(imu));
static const struct gpio_dt_spec sd_detect =
	GPIO_DT_SPEC_GET(DT_NODELABEL(sd_slot_control), detect_gpios);

static struct net_mgmt_event_callback wifi_callback;
static K_SEM_DEFINE(wifi_scan_done, 0, 1);
static uint16_t wifi_scan_results;
static int wifi_scan_status;

static int32_t touch_x;
static int32_t touch_y;
static const struct sensor_trigger imu_motion_trigger = {
	.type = SENSOR_TRIG_MOTION,
	.chan = SENSOR_CHAN_ACCEL_XYZ,
};

static void selftest_input_event(struct input_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	if (event->dev == touch) {
		if (event->code == INPUT_ABS_X) {
			touch_x = event->value;
		} else if (event->code == INPUT_ABS_Y) {
			touch_y = event->value;
		} else if (event->code == INPUT_BTN_TOUCH) {
			LOG_INF("HIL touch %s at x=%d y=%d",
				event->value ? "pressed" : "released",
				touch_x, touch_y);
		}
	} else if (event->dev == buttons && event->type == INPUT_EV_KEY) {
		LOG_INF("HIL button code=%u %s", event->code,
			event->value ? "pressed" : "released");
	}
}
INPUT_CALLBACK_DEFINE(NULL, selftest_input_event, NULL);

static void selftest_imu_trigger(const struct device *dev,
				 const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trigger);
	LOG_INF("HIL BMI270 motion interrupt received");
}

static void log_sensor_value(const char *name, const struct sensor_value *value)
{
	if (value->val1 == 0 && value->val2 < 0) {
		LOG_INF("HIL %s=-0.%06d", name, -value->val2);
	} else {
		LOG_INF("HIL %s=%d.%06d", name, value->val1,
			value->val2 < 0 ? -value->val2 : value->val2);
	}
}

static int selftest_mfd(void)
{
	struct m5ioe1_info ioe_info;
	struct m5pm1_info pmic_info;
	enum m5ioe1_i2c_wake_edge ioe_wake_edge;
	uint16_t sample;
	uint8_t source;
	uint8_t wake_source;
	uint8_t hold;
	uint8_t sleep_timeout;
	uint8_t ioe_sleep_timeout;
	uint8_t rtc_saved;
	uint8_t rtc_test;
	uint8_t rtc_actual;
	int restore_ret;
	int ret;

	ret = m5ioe1_get_info(ioe, &ioe_info);
	ret = ret ?: m5pm1_get_info(pmic, &pmic_info);
	ret = ret ?: m5ioe1_get_i2c_sleep_timeout(ioe, &ioe_sleep_timeout);
	ret = ret ?: m5ioe1_get_i2c_wake_edge(ioe, &ioe_wake_edge);
	ret = ret ?: m5ioe1_send_wake_signal(ioe);
	if (ret < 0) {
		return ret;
	}
	LOG_INF("HIL M5IOE1 uid=0x%04x rev=0x%02x vref=%u mV",
		ioe_info.uid, ioe_info.revision, ioe_info.reference_mv);
	LOG_INF("HIL M5IOE1 I2C-sleep=%u s wake-edge=%s",
		ioe_sleep_timeout,
		ioe_wake_edge == M5IOE1_I2C_WAKE_RISING ? "rising" : "falling");
	LOG_INF("HIL M5PM1 id=0x%02x model=0x%02x hw=0x%02x sw=0x%02x",
		pmic_info.id, pmic_info.model, pmic_info.hardware_revision,
		pmic_info.software_revision);

	ret = m5ioe1_read_temperature(ioe, &sample);
	if (ret == 0) {
		LOG_INF("HIL M5IOE1 temperature sample=%u", sample);
	}
	ret = ret ?: m5ioe1_read_adc(ioe, 2U, &sample);
	if (ret == 0) {
		LOG_INF("HIL M5IOE1 ADC2 sample=%u", sample);
	}
	ret = ret ?: m5pm1_get_power_source(pmic, &source);
	if (ret == 0) {
		LOG_INF("HIL M5PM1 power source=0x%02x", source);
	}
	for (enum m5pm1_voltage voltage = M5PM1_VOLTAGE_REFERENCE;
	     ret == 0 && voltage <= M5PM1_VOLTAGE_5V_OUTPUT; ++voltage) {
		ret = m5pm1_read_voltage(pmic, voltage, &sample);
		if (ret == 0) {
			LOG_INF("HIL M5PM1 voltage[%u]=%u mV", voltage, sample);
		}
	}
	ret = ret ?: m5pm1_read_wake_source(
		pmic, &wake_source, false);
	ret = ret ?: m5pm1_get_power_hold(pmic, &hold);
	ret = ret ?: m5pm1_get_i2c_sleep_timeout(
		pmic, &sleep_timeout);
	ret = ret ?: m5pm1_send_wake_signal(pmic);
	if (ret == 0) {
		LOG_INF("HIL M5PM1 wake=0x%02x hold=0x%02x I2C-sleep=%u s",
			wake_source, hold, sleep_timeout);
	}
	ret = ret ?: m5pm1_rtc_ram_read(
		pmic, M5PM1_RTC_RAM_SIZE - 1U, &rtc_saved,
		sizeof(rtc_saved));
	if (ret < 0) {
		return ret;
	}
	rtc_test = rtc_saved ^ 0xA5U;
	ret = m5pm1_rtc_ram_write(
		pmic, M5PM1_RTC_RAM_SIZE - 1U, &rtc_test,
		sizeof(rtc_test), true);
	ret = ret ?: m5pm1_rtc_ram_read(
		pmic, M5PM1_RTC_RAM_SIZE - 1U, &rtc_actual,
		sizeof(rtc_actual));
	if (ret == 0 && rtc_actual != rtc_test) {
		ret = -EIO;
	}
	/* Restore only after the original byte was read successfully.  Even when
	 * the test write or readback fails, preserve the first error while making
	 * a best-effort attempt to put the saved value back.
	 */
	restore_ret = m5pm1_rtc_ram_write(
		pmic, M5PM1_RTC_RAM_SIZE - 1U, &rtc_saved,
		sizeof(rtc_saved), true);
	if (ret == 0) {
		ret = restore_ret;
	}
	if (ret == 0) {
		LOG_INF("HIL M5PM1 RTC RAM read/write/restore passed");
	}
	return ret;
}

static int selftest_imu(void)
{
	struct sensor_value value[3];
	struct sensor_value full_scale = { .val1 = 2, .val2 = 0 };
	struct sensor_value sampling_frequency = { .val1 = 100, .val2 = 0 };
	struct sensor_value oversampling = { .val1 = 1, .val2 = 0 };
	int temp_ret;
	int ret;
	bool sample_valid = false;

	ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_FULL_SCALE, &full_scale);
	ret = ret ?: sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				     SENSOR_ATTR_OVERSAMPLING, &oversampling);
	/* Sampling frequency is set last because it also enables the sensor. */
	ret = ret ?: sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
				     SENSOR_ATTR_SAMPLING_FREQUENCY,
				     &sampling_frequency);

	full_scale.val1 = 500;
	ret = ret ?: sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ,
				     SENSOR_ATTR_FULL_SCALE, &full_scale);
	ret = ret ?: sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ,
				     SENSOR_ATTR_OVERSAMPLING, &oversampling);
	ret = ret ?: sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ,
				     SENSOR_ATTR_SAMPLING_FREQUENCY,
				     &sampling_frequency);
	if (ret < 0) {
		return ret;
	}

	/* Gyroscope startup can take tens of milliseconds.  Poll several 100 Hz
	 * periods and require a physically plausible, non-zero gravity sample.
	 */
	for (uint8_t attempt = 0U; attempt < 10U && !sample_valid; ++attempt) {
		k_msleep(20);
		ret = sensor_sample_fetch(imu);
		ret = ret ?: sensor_channel_get(
			imu, SENSOR_CHAN_ACCEL_XYZ, value);
		if (ret < 0) {
			return ret;
		}
		sample_valid = value[0].val1 != 0 || value[0].val2 != 0 ||
			       value[1].val1 != 0 || value[1].val2 != 0 ||
			       value[2].val1 != 0 || value[2].val2 != 0;
	}
	if (!sample_valid) {
		uint8_t pwr_ctrl = 0U;
		uint8_t acc_conf = 0U;
		uint8_t gyr_conf = 0U;
		uint8_t status = 0U;

		(void)i2c_reg_read_byte_dt(&imu_bus, 0x7dU, &pwr_ctrl);
		(void)i2c_reg_read_byte_dt(&imu_bus, 0x40U, &acc_conf);
		(void)i2c_reg_read_byte_dt(&imu_bus, 0x42U, &gyr_conf);
		(void)i2c_reg_read_byte_dt(&imu_bus, 0x03U, &status);
		LOG_ERR("HIL BMI270 zero samples: pwr=0x%02x acc=0x%02x gyr=0x%02x status=0x%02x",
			pwr_ctrl, acc_conf, gyr_conf, status);
		return -ENODATA;
	}
	if (ret == 0) {
		log_sensor_value("accel-x", &value[0]);
		log_sensor_value("accel-y", &value[1]);
		log_sensor_value("accel-z", &value[2]);
	}
	ret = ret ?: sensor_channel_get(imu, SENSOR_CHAN_GYRO_XYZ, value);
	if (ret == 0) {
		log_sensor_value("gyro-x", &value[0]);
		log_sensor_value("gyro-y", &value[1]);
		log_sensor_value("gyro-z", &value[2]);
	}
	temp_ret = sensor_channel_get(imu, SENSOR_CHAN_DIE_TEMP, &value[0]);
	if (temp_ret == 0) {
		log_sensor_value("imu-temp", &value[0]);
	} else if (temp_ret != -ENOTSUP) {
		LOG_WRN("HIL BMI270 optional temperature read failed (%d)",
			temp_ret);
	}
	ret = ret ?: sensor_trigger_set(
		imu, &imu_motion_trigger, selftest_imu_trigger);
	if (ret == 0) {
		LOG_INF("HIL BMI270 motion interrupt armed");
	}
	return ret;
}

static int selftest_rtc(void)
{
	struct rtc_time time;
	uint16_t mask;
	int ret;

	ret = rtc_get_time(rtc, &time);
	if (ret == 0) {
		LOG_INF("HIL RTC %04d-%02d-%02d %02d:%02d:%02d",
			time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
			time.tm_hour, time.tm_min, time.tm_sec);
	}
	ret = ret ?: rtc_alarm_get_supported_fields(rtc, 0U, &mask);
	if (ret == 0) {
		LOG_INF("HIL RTC alarm fields=0x%04x", mask);
	}
	return ret;
}

static int selftest_sd(void)
{
	static uint8_t sector[512] __aligned(4);
	uint32_t sector_count;
	uint32_t sector_size;
	int raw_level;
	int inserted;
	int ret;

	ret = gpio_pin_configure_dt(&sd_detect, GPIO_INPUT | GPIO_PULL_UP);
	if (ret < 0) {
		return ret;
	}
	inserted = gpio_pin_get_dt(&sd_detect);
	raw_level = gpio_pin_get_raw(sd_detect.port, sd_detect.pin);
	if (inserted < 0) {
		return inserted;
	}
	LOG_INF("HIL microSD detect raw=%d inserted=%d", raw_level, inserted);
	if (inserted == 0) {
		LOG_INF("HIL microSD not inserted; media test skipped");
		return 0;
	}

	ret = disk_access_init("SD");
	ret = ret ?: disk_access_ioctl(
		"SD", DISK_IOCTL_GET_SECTOR_COUNT, &sector_count);
	ret = ret ?: disk_access_ioctl(
		"SD", DISK_IOCTL_GET_SECTOR_SIZE, &sector_size);
	ret = ret ?: disk_access_read("SD", sector, 0U, 1U);
	if (ret == 0) {
		LOG_INF("HIL microSD ready: %u sectors x %u bytes",
			sector_count, sector_size);
	}
	return ret;
}

/* Prove the mounted FAT volume is writable and readable, not just that the card
 * answers raw commands. Uses a temporary file it removes again, so running the
 * self-test leaves no content behind.
 */
static int selftest_sd_filesystem(void)
{
	static const char pattern[] = "PaperMono self-test";
	char readback[sizeof(pattern)];
	struct fs_file_t file;
	struct fs_dirent entry;
	ssize_t length;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, PAPERMONO_SD_SELFTEST_PATH,
		FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret < 0) {
		return ret;
	}
	length = fs_write(&file, pattern, sizeof(pattern));
	(void)fs_sync(&file);
	{
		const int close_ret = fs_close(&file);

		if (length != (ssize_t)sizeof(pattern)) {
			(void)fs_unlink(PAPERMONO_SD_SELFTEST_PATH);
			return length < 0 ? (int)length : -EIO;
		}
		if (close_ret < 0) {
			(void)fs_unlink(PAPERMONO_SD_SELFTEST_PATH);
			return close_ret;
		}
	}
	ret = fs_stat(PAPERMONO_SD_SELFTEST_PATH, &entry);
	if (ret == 0 && entry.size == sizeof(pattern)) {
		LOG_INF("HIL microSD FAT write ok: %u bytes",
			(unsigned int)entry.size);
	} else {
		ret = ret < 0 ? ret : -EIO;
	}
	if (ret == 0) {
		fs_file_t_init(&file);
		ret = fs_open(&file, PAPERMONO_SD_SELFTEST_PATH, FS_O_READ);
		if (ret == 0) {
			memset(readback, 0, sizeof(readback));
			length = fs_read(&file, readback, sizeof(readback));
			(void)fs_close(&file);
			if (length != (ssize_t)sizeof(pattern) ||
			    memcmp(readback, pattern, sizeof(pattern)) != 0) {
				ret = -EIO;
			} else {
				LOG_INF("HIL microSD FAT read back and verified");
			}
		}
	}
	if (fs_unlink(PAPERMONO_SD_SELFTEST_PATH) < 0 && ret == 0) {
		ret = -EIO;
	}
	return ret;
}

/* Exercise the content-store import transaction end to end: begin with a known
 * size, stream the body, commit (which validates the file and swaps it in),
 * read it back through the public open path, then delete it. This is the same
 * code path an HTTP upload uses once the request body length is known, so it
 * covers the storage side of the content-center upload on real hardware.
 */
static int selftest_content_import(void)
{
	static const char text[] = "PaperMono import self-test\n";
	static const char name[] = "selftest-import.txt";
	char readback[sizeof(text)];
	struct papermono_content_transaction transaction;
	struct papermono_content_entry entry = {
		.kind = PAPERMONO_CONTENT_TXT,
	};
	struct fs_file_t file;
	ssize_t length;
	int ret;

	ret = papermono_content_store_import_begin(&transaction,
		PAPERMONO_CONTENT_TXT, name, sizeof(text) - 1U, false, 0U);
	if (ret < 0) {
		return ret;
	}
	ret = papermono_content_store_import_write(&transaction,
		(const uint8_t *)text, sizeof(text) - 1U);
	if (ret < 0) {
		(void)papermono_content_store_import_abort(&transaction);
		return ret;
	}
	ret = papermono_content_store_import_commit(&transaction);
	if (ret < 0) {
		return ret;
	}
	(void)snprintf(entry.path, sizeof(entry.path), "%s/%s",
		PAPERMONO_BOOKS_DIRECTORY, name);
	fs_file_t_init(&file);
	ret = papermono_content_store_open(&entry, &file);
	if (ret == 0) {
		memset(readback, 0, sizeof(readback));
		length = fs_read(&file, readback, sizeof(readback));
		(void)fs_close(&file);
		if (length != (ssize_t)(sizeof(text) - 1U) ||
		    memcmp(readback, text, sizeof(text) - 1U) != 0) {
			ret = -EIO;
		} else {
			LOG_INF("HIL content import committed and read back");
		}
	}
	if (papermono_content_store_delete(&entry) < 0 && ret == 0) {
		ret = -EIO;
	}
	if (ret == 0) {
		LOG_INF("HIL content import transaction cleaned up");
	}
	return ret;
}

/*
 * Import the embedded minimal EPUB through the same store transaction the HTTP
 * upload uses. This exercises EPUB validation, the ZIP reader and the chunked
 * filesystem reads without needing a network peer.
 */
extern const uint8_t papermono_selftest_epub[];
extern const size_t papermono_selftest_epub_size;

static int selftest_content_import_epub(void)
{
	static const char name[] = "selftest-import.epub";
	struct papermono_content_transaction transaction;
	struct papermono_content_entry entry = {
		.kind = PAPERMONO_CONTENT_EPUB,
	};
	int ret;
	size_t written = 0U;

	ret = papermono_content_store_import_begin(&transaction,
		PAPERMONO_CONTENT_EPUB, name, papermono_selftest_epub_size, false, 0U);
	if (ret < 0) {
		return ret;
	}
	while (written < papermono_selftest_epub_size) {
		size_t chunk = MIN(papermono_selftest_epub_size - written, 512U);

		ret = papermono_content_store_import_write(&transaction,
			&papermono_selftest_epub[written], chunk);
		if (ret < 0) {
			(void)papermono_content_store_import_abort(&transaction);
			return ret;
		}
		written += chunk;
	}
	ret = papermono_content_store_import_commit(&transaction);
	if (ret < 0) {
		return ret;
	}
	(void)snprintf(entry.path, sizeof(entry.path), "%s/%s",
		PAPERMONO_BOOKS_DIRECTORY, name);
	if (papermono_content_store_delete(&entry) < 0 && ret == 0) {
		ret = -EIO;
	}
	if (ret == 0) {
		LOG_INF("HIL EPUB import committed, validated and removed");
	}
	return ret;
}

static int selftest_charger(void)
{
	union charger_propval value;
	int ret;

	ret = charger_get_prop(charger, CHARGER_PROP_ONLINE, &value);
	if (ret == 0) {
		LOG_INF("HIL charger online=%u", value.online);
	}
#if defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TIMER) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_DEEP_SLEEP_TOUCH) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_BUTTON) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_RTC) || \
	defined(CONFIG_PAPERMONO_POWER_HIL_SHUTDOWN_IMU)
	/* The optional IP2315 register interface at 0x75 is not populated or does
	 * not ACK on the tested board.  An ESP32 address-phase timeout here can
	 * obscure the power event under test, while the M5PM1-backed online state
	 * above remains available and is sufficient for the product UI.
	 */
	LOG_INF("HIL charger register probe skipped for isolated power test");
	return ret;
#endif
	ret = ret ?: charger_get_prop(charger, CHARGER_PROP_STATUS, &value);
	if (ret == 0) {
		LOG_INF("HIL charger status=%u", value.status);
	} else if (ret == -EFAULT || ret == -ENXIO || ret == -ENODEV) {
		LOG_WRN("HIL charger register interface did not acknowledge (%d); USB/battery voltage monitoring remains available through M5PM1",
			ret);
		return 0;
	}
	return ret;
}

static int selftest_buzzer(void)
{
	int ret;

	ret = pwm_set_dt(&buzzer, PWM_HZ(2000), PWM_HZ(2000) / 2U);
	if (ret < 0) {
		return ret;
	}
	k_msleep(100);
	ret = pwm_set_pulse_dt(&buzzer, 0U);
	if (ret == 0) {
		LOG_INF("HIL buzzer 2 kHz tone complete");
	}
	return ret;
}

static int selftest_lora(void)
{
	struct lora_modem_config config = {
		.frequency = 868000000U,
		.bandwidth = BW_62_KHZ,
		.datarate = SF_12,
		.coding_rate = CR_4_8,
		.preamble_len = 10U,
		.tx_power = 22,
		.tx = false,
		.iq_inverted = false,
		.public_network = false,
		.packet_crc_disable = false,
	};
	int ret;

#if defined(CONFIG_PAPERMONO_LORA_HIL_RX) || \
	defined(CONFIG_PAPERMONO_LORA_HIL_TX)
	config.frequency = CONFIG_PAPERMONO_LORA_HIL_FREQUENCY_HZ;
#endif
#if defined(CONFIG_PAPERMONO_LORA_HIL_TX)
	config.tx = true;
	config.tx_power = CONFIG_PAPERMONO_LORA_HIL_TX_POWER_DBM;
#endif
	ret = lora_config(lora, &config);
	if (ret < 0) {
		return ret;
	}

#if defined(CONFIG_PAPERMONO_LORA_HIL_RX)
	uint8_t packet[255];
	int16_t rssi;
	int8_t snr;

	LOG_INF("HIL SX1262 listening at %u Hz for %u ms",
		config.frequency, CONFIG_PAPERMONO_LORA_HIL_RX_TIMEOUT_MS);
	ret = lora_recv(lora, packet, sizeof(packet),
			K_MSEC(CONFIG_PAPERMONO_LORA_HIL_RX_TIMEOUT_MS),
			&rssi, &snr);
	if (ret == -EAGAIN || ret == -ETIMEDOUT) {
		LOG_INF("HIL SX1262 receive window completed; no packet received");
		return 0;
	}
	if (ret < 0) {
		return ret;
	}
	LOG_INF("HIL SX1262 received %d bytes RSSI=%d dBm SNR=%d dB",
		ret, rssi, snr);
	LOG_HEXDUMP_INF(packet, ret, "HIL SX1262 RX");
	return 0;
#elif defined(CONFIG_PAPERMONO_LORA_HIL_TX)
	uint8_t packet[] = CONFIG_PAPERMONO_LORA_HIL_TX_PAYLOAD;

	LOG_WRN("HIL SX1262 transmitting %u bytes at %u Hz, %d dBm",
		(unsigned int)(sizeof(packet) - 1U), config.frequency,
		config.tx_power);
	ret = lora_send(lora, packet, sizeof(packet) - 1U);
	if (ret == 0) {
		LOG_INF("HIL SX1262 packet transmission complete");
	}
	return ret;
#else
	LOG_INF("HIL SX1262 configured at 868 MHz; no RF transmission performed");
	return 0;
#endif
}

static void selftest_wifi_event(struct net_mgmt_event_callback *callback,
				uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (event == NET_EVENT_WIFI_SCAN_RESULT) {
		const struct wifi_scan_result *result = callback->info;

		++wifi_scan_results;
		if (wifi_scan_results <= 8U) {
			LOG_INF("HIL Wi-Fi AP %u SSID=%.*s channel=%u RSSI=%d dBm",
				wifi_scan_results, result->ssid_length,
				result->ssid, result->channel, result->rssi);
		}
	} else if (event == NET_EVENT_WIFI_SCAN_DONE) {
		const struct wifi_status *status = callback->info;

		wifi_scan_status = status != NULL ? status->status : -EIO;
		k_sem_give(&wifi_scan_done);
	}
}

static int selftest_wifi(void)
{
	struct net_if *iface = net_if_get_default();
	int ret;

	if (iface == NULL) {
		return -ENODEV;
	}
	wifi_scan_results = 0U;
	wifi_scan_status = -EINPROGRESS;
	LOG_INF("HIL Wi-Fi scan starting");
	k_sem_reset(&wifi_scan_done);
	net_mgmt_init_event_callback(
		&wifi_callback, selftest_wifi_event,
		NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE);
	net_mgmt_add_event_callback(&wifi_callback);
	ret = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, NULL, 0U);
	if (ret == 0) {
		ret = k_sem_take(&wifi_scan_done, K_SECONDS(20));
		if (ret == 0) {
			ret = wifi_scan_status == 0 ? 0 : -EIO;
		}
	}
	net_mgmt_del_event_callback(&wifi_callback);
	if (ret == 0) {
		LOG_INF("HIL Wi-Fi scan complete: %u APs", wifi_scan_results);
	}
	return ret;
}

static int selftest_nfc(void)
{
	struct papermono_st25r3916_info info;
	struct papermono_nfca_tag tag;
	struct papermono_nfcb_tag tag_b;
	struct papermono_nfcf_tag tag_f;
	struct papermono_nfcv_tag tag_v;
	char uid[PAPERMONO_NFCA_UID_MAX_LENGTH * 3U];
	size_t offset = 0U;
	int ret;
	int power_ret;

	ret = papermono_st25r3916_set_power(nfc, true);
	ret = ret ?: papermono_st25r3916_get_info(nfc, &info);
	if (ret == 0) {
		LOG_INF("HIL ST25R3916 identity=0x%02x type=0x%02x revision=%u IRQ=%d",
			info.identity, info.type, info.revision,
			gpio_pin_get_raw(DEVICE_DT_GET(DT_NODELABEL(gpio0)), 6U));
	}
	ret = ret ?: papermono_st25r3916_configure_nfca(nfc);
	if (ret == 0) {
		ret = papermono_st25r3916_nfca_scan(nfc, &tag);
		if (ret == -ENOMSG) {
			LOG_INF("HIL NFC-A RF field active; no tag present");
			ret = 0;
		} else if (ret == -EAGAIN) {
			LOG_INF("HIL NFC-A RF field active; multiple-tag collision detected");
			ret = 0;
		} else if (ret == 0) {
			for (uint8_t i = 0U; i < tag.uid_length &&
			     offset < sizeof(uid); ++i) {
				offset += snprintk(
					&uid[offset], sizeof(uid) - offset,
					i == 0U ? "%02X" : ":%02X", tag.uid[i]);
			}
			LOG_INF("HIL NFC-A tag uid=%s ATQA=0x%04x SAK=0x%02x",
				uid, tag.atqa, tag.sak);
		}
	}
	if (ret == 0) {
		ret = papermono_st25r3916_configure_nfcb(nfc);
	}
	if (ret == 0) {
		ret = papermono_st25r3916_nfcb_scan(nfc, &tag_b);
		if (ret == -ENOMSG) {
			LOG_INF("HIL NFC-B RF field active; no tag present");
			ret = 0;
		} else if (ret == -EAGAIN) {
			LOG_INF("HIL NFC-B RF field active; collision detected");
			ret = 0;
		} else if (ret == 0) {
			LOG_INF("HIL NFC-B tag PUPI=%02X:%02X:%02X:%02X",
				tag_b.pupi[0], tag_b.pupi[1], tag_b.pupi[2],
				tag_b.pupi[3]);
		}
	}
	if (ret == 0) {
		ret = papermono_st25r3916_configure_nfcf(nfc);
	}
	if (ret == 0) {
		ret = papermono_st25r3916_nfcf_scan(nfc, &tag_f);
		if (ret == -ENOMSG) {
			LOG_INF("HIL NFC-F RF field active; no tag present");
			ret = 0;
		} else if (ret == -EAGAIN) {
			LOG_INF("HIL NFC-F RF field active; collision detected");
			ret = 0;
		} else if (ret == 0) {
			LOG_INF("HIL NFC-F tag IDm=%02X%02X%02X%02X%02X%02X%02X%02X",
				tag_f.idm[0], tag_f.idm[1], tag_f.idm[2],
				tag_f.idm[3], tag_f.idm[4], tag_f.idm[5],
				tag_f.idm[6], tag_f.idm[7]);
		}
	}
	if (ret == 0) {
		ret = papermono_st25r3916_configure_nfcv(nfc);
	}
	if (ret == 0) {
		ret = papermono_st25r3916_nfcv_scan(nfc, &tag_v);
		if (ret == -ENOMSG) {
			LOG_INF("HIL NFC-V RF field active; no tag present");
			ret = 0;
		} else if (ret == -EAGAIN) {
			LOG_INF("HIL NFC-V RF field active; collision detected");
			ret = 0;
		} else if (ret == 0) {
			LOG_INF("HIL NFC-V tag UID=%02X%02X%02X%02X%02X%02X%02X%02X DSFID=0x%02X",
				tag_v.uid[0], tag_v.uid[1], tag_v.uid[2],
				tag_v.uid[3], tag_v.uid[4], tag_v.uid[5],
				tag_v.uid[6], tag_v.uid[7], tag_v.dsfid);
		}
	}
	if (ret == 0) {
		ret = papermono_st25r3916_disable_field(nfc);
	}
	power_ret = papermono_st25r3916_set_power(nfc, false);
	return ret < 0 ? ret : power_ret;
}

static int selftest_microphone(void)
{
	int16_t samples[256];
	size_t count = 0U;
	int16_t minimum = INT16_MAX;
	int16_t maximum = INT16_MIN;
	uint64_t absolute_sum = 0U;
	int stop_ret;
	int ret;

	ret = papermono_pdm_mic_start(microphone);
	/* Discard the first two DMA blocks while the PDM filter settles. */
	for (uint8_t block = 0U; ret == 0 && block < 3U; ++block) {
		ret = papermono_pdm_mic_read(microphone, samples,
					     ARRAY_SIZE(samples), &count);
	}
	if (ret == 0 && count > 0U) {
		for (size_t i = 0U; i < count; ++i) {
			int32_t sample = samples[i];

			minimum = MIN(minimum, samples[i]);
			maximum = MAX(maximum, samples[i]);
			absolute_sum += sample < 0 ? -sample : sample;
		}
		LOG_INF("HIL PDM microphone %u samples min=%d max=%d mean-abs=%llu",
			(unsigned int)count, minimum, maximum,
			(unsigned long long)(absolute_sum / count));
	}
	stop_ret = papermono_pdm_mic_stop(microphone);
	return ret < 0 ? ret : stop_ret;
}

static int selftest_gray4_reader(
	void *context, enum papermono_ssd1677_gray_plane plane,
	size_t offset, uint8_t *buffer, size_t length)
{
	ARG_UNUSED(context);

	for (size_t i = 0U; i < length; ++i) {
		const size_t native_row = (offset + i) /
			PAPERMONO_SSD1677_GRAY4_ROW_BYTES;
		const size_t native_column_byte = (offset + i) %
			PAPERMONO_SSD1677_GRAY4_ROW_BYTES;
		const bool lower_half = native_row >=
			PAPERMONO_SSD1677_GRAY4_ROWS / 2U;
		const bool second_half = native_column_byte >=
			PAPERMONO_SSD1677_GRAY4_ROW_BYTES / 2U;

		/* 00, 10, 01 and 11 produce the four OTP gray levels. */
		buffer[i] = ((plane == PAPERMONO_SSD1677_GRAY_PLANE_1 &&
			      second_half) ||
			     (plane == PAPERMONO_SSD1677_GRAY_PLANE_2 &&
			      lower_half)) ? 0xFFU : 0x00U;
	}
	return 0;
}

static int selftest_gray4(const struct device *display)
{
	int ret = papermono_ssd1677_refresh_gray4(
		display, selftest_gray4_reader, NULL);

	if (ret == 0) {
		LOG_INF("HIL four-gray quadrant image displayed: white/light/dark/black");
	}
	return ret;
}

static int selftest_visual(const struct device *display)
{
	static const uint8_t colors[][3] = {
		{255U, 0U, 0U},
		{0U, 255U, 0U},
		{0U, 0U, 255U},
		{0U, 0U, 0U},
	};
	int ret = 0;

	for (size_t i = 0U; ret == 0 && i < ARRAY_SIZE(colors); ++i) {
		ret = led_set_color(rgb, 0U, 3U, colors[i]);
		k_msleep(300);
	}
	ret = ret ?: display_set_brightness(display, 48U);
	k_msleep(700);
	ret = ret ?: display_set_brightness(display, 0U);
	return ret;
}

static void selftest_record_result(const char *name, int check, int *result)
{
	if (check >= 0) {
		LOG_INF("HIL %s check passed", name);
		return;
	}

	LOG_ERR("HIL %s check failed (%d)", name, check);
	if (*result == 0) {
		*result = check;
	}
}

int papermono_driver_selftest_run(const struct device *display)
{
	const struct device *devices[] = {
		ioe, pmic, touch, buttons, imu, rtc, rgb, charger, lora, nfc,
		microphone,
	};
	int check;
	int ret = 0;

	for (size_t i = 0U; i < ARRAY_SIZE(devices); ++i) {
		if (!device_is_ready(devices[i])) {
			LOG_ERR("HIL device %s is not ready", devices[i]->name);
			return -ENODEV;
		}
	}
	if (!pwm_is_ready_dt(&buzzer)) {
		LOG_ERR("HIL buzzer PWM is not ready");
		return -ENODEV;
	}

	LOG_INF("HIL driver self-test starting");
	check = selftest_mfd();
	selftest_record_result("MFD", check, &ret);
	check = selftest_imu();
	selftest_record_result("IMU", check, &ret);
	check = selftest_rtc();
	selftest_record_result("RTC", check, &ret);
	check = selftest_sd();
	selftest_record_result("microSD", check, &ret);
	check = selftest_sd_filesystem();
	selftest_record_result("microSD FAT", check, &ret);
	check = selftest_content_import();
	selftest_record_result("content import", check, &ret);
	check = selftest_content_import_epub();
	selftest_record_result("content import epub", check, &ret);
	check = selftest_charger();
	selftest_record_result("charger", check, &ret);
	check = selftest_buzzer();
	selftest_record_result("buzzer", check, &ret);
	check = selftest_lora();
	selftest_record_result("LoRa", check, &ret);
	check = selftest_wifi();
	selftest_record_result("Wi-Fi", check, &ret);
	/* Keep shared M5IOE1/M5PM1 checks ahead of the switchable NFC rail. */
	check = selftest_microphone();
	selftest_record_result("microphone", check, &ret);
	check = selftest_visual(display);
	selftest_record_result("visual output", check, &ret);
	check = selftest_nfc();
	selftest_record_result("NFC", check, &ret);
	check = selftest_gray4(display);
	selftest_record_result("four-gray display", check, &ret);
	if (ret < 0) {
		LOG_ERR("HIL automatic checks failed (%d)", ret);
		return ret;
	}
	LOG_INF("HIL automatic checks passed; touch the panel and press both side buttons");
	return 0;
}
