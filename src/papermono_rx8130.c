/*
 * Copyright (c) 2026 PaperMono Zephyr contributors
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <papermono/rx8130.h>

#define RX8130_REG_TIMER_LOW 0x1AU
#define RX8130_REG_FLAG      0x1DU
#define RX8130_REG_CONTROL0  0x1EU

#define RX8130_EXTENSION_TE  BIT(4)
#define RX8130_EXTENSION_TSEL_MASK 0x07U
#define RX8130_FLAG_TF       BIT(4)
#define RX8130_FLAG_AF       BIT(3)
#define RX8130_CONTROL0_TIE  BIT(4)

static const struct i2c_dt_spec rx8130 =
	I2C_DT_SPEC_GET(DT_NODELABEL(rtc));
K_MUTEX_DEFINE(rx8130_timer_lock);

static int rx8130_read_timer_registers(uint8_t registers[3])
{
	return i2c_burst_read_dt(&rx8130, RX8130_REG_TIMER_LOW,
				 registers, 3U);
}

static int rx8130_write_timer_registers(const uint8_t registers[3])
{
	return i2c_burst_write_dt(&rx8130, RX8130_REG_TIMER_LOW,
				  registers, 3U);
}

int papermono_rx8130_set_timer_irq(uint32_t requested_ms,
				    uint32_t *actual_ms)
{
	uint32_t divisor = 1U;
	uint32_t multiplier = 1U;
	uint8_t select;
	uint8_t verify[3];
	uint8_t registers[3];
	uint8_t control0;
	uint8_t control0_verify;
	uint64_t cycles;
	uint16_t verify_cycles;
	int ret;

	if (actual_ms == NULL || !i2c_is_ready_dt(&rx8130)) {
		return actual_ms == NULL ? -EINVAL : -ENODEV;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	if (requested_ms < 65536U * 1000U / 4096U) {
		select = 0U;
		divisor = 4096U;
	} else if (requested_ms < 65536U * 1000U / 64U) {
		select = 1U;
		divisor = 64U;
	} else if (requested_ms < 65536U * 1000U) {
		select = 2U;
	} else if ((uint64_t)requested_ms <
		   ((uint64_t)UINT16_MAX + 1U) * 60U * 1000U) {
		select = 3U;
		multiplier = 60U;
	} else {
		select = 4U;
		multiplier = 3600U;
	}

	k_mutex_lock(&rx8130_timer_lock, K_FOREVER);
	ret = rx8130_read_timer_registers(registers);
	ret = ret ?: i2c_reg_read_byte_dt(
		&rx8130, RX8130_REG_CONTROL0, &control0);
	if (ret < 0) {
		goto out;
	}

	cycles = ((uint64_t)requested_ms * divisor +
		  (uint64_t)multiplier * 500U) /
		 ((uint64_t)multiplier * 1000U);
	cycles = MIN(cycles, UINT16_MAX);
	registers[0] = (uint8_t)cycles;
	registers[1] = (uint8_t)(cycles >> 8);
	if (cycles == 0U) {
		registers[2] &= ~RX8130_EXTENSION_TE;
		control0 &= ~RX8130_CONTROL0_TIE;
		*actual_ms = 0U;
		ret = rx8130_write_timer_registers(registers);
		ret = ret ?: i2c_reg_write_byte_dt(
			&rx8130, RX8130_REG_CONTROL0, control0);
	} else {
		registers[2] &= ~(RX8130_EXTENSION_TE |
				  RX8130_EXTENSION_TSEL_MASK);
		registers[2] |= RX8130_EXTENSION_TE | select;
		control0 |= RX8130_CONTROL0_TIE;
		*actual_ms = (uint32_t)(cycles * multiplier * 1000U /
					divisor);
		/* Match M5Unified's RX8130 sequence: route timer IRQ first, then
		 * write counter/selector/TE as one three-byte transaction so the
		 * countdown starts only after nIRQ is enabled.
		 */
		ret = i2c_reg_write_byte_dt(
			&rx8130, RX8130_REG_CONTROL0, control0);
		ret = ret ?: rx8130_write_timer_registers(registers);
	}
	if (ret == 0) {
		ret = rx8130_read_timer_registers(verify);
		ret = ret ?: i2c_reg_read_byte_dt(
			&rx8130, RX8130_REG_CONTROL0, &control0_verify);
	}
	if (ret == 0) {
		verify_cycles = (uint16_t)verify[0] |
				((uint16_t)verify[1] << 8);
		/* A running counter can decrement between the write and readback,
		 * especially with the 4096 Hz source.  Verify its direction and the
		 * control bits instead of requiring a byte-for-byte snapshot match.
		 */
		if ((cycles == 0U &&
		     (verify_cycles != 0U ||
		      (verify[2] & RX8130_EXTENSION_TE) != 0U)) ||
		    (cycles != 0U &&
		     (verify_cycles == 0U || verify_cycles > cycles ||
		      (verify[2] & (RX8130_EXTENSION_TE |
				    RX8130_EXTENSION_TSEL_MASK)) !=
		      (registers[2] & (RX8130_EXTENSION_TE |
				       RX8130_EXTENSION_TSEL_MASK)))) ||
		    (control0_verify & RX8130_CONTROL0_TIE) !=
			(control0 & RX8130_CONTROL0_TIE)) {
			ret = -EIO;
		}
	}
out:
	k_mutex_unlock(&rx8130_timer_lock);
	return ret;
}

int papermono_rx8130_disable_timer_irq(void)
{
	uint32_t actual_ms;

	return papermono_rx8130_set_timer_irq(0U, &actual_ms);
}

int papermono_rx8130_get_irq_status(bool *timer, bool *alarm)
{
	uint8_t flag;
	int ret;

	if (timer == NULL || alarm == NULL || !i2c_is_ready_dt(&rx8130)) {
		return timer == NULL || alarm == NULL ? -EINVAL : -ENODEV;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	k_mutex_lock(&rx8130_timer_lock, K_FOREVER);
	ret = i2c_reg_read_byte_dt(&rx8130, RX8130_REG_FLAG, &flag);
	k_mutex_unlock(&rx8130_timer_lock);
	if (ret == 0) {
		*timer = (flag & RX8130_FLAG_TF) != 0U;
		*alarm = (flag & RX8130_FLAG_AF) != 0U;
	}
	return ret;
}

int papermono_rx8130_clear_irq_status(bool timer, bool alarm)
{
	uint8_t flag;
	int ret;

	if (!i2c_is_ready_dt(&rx8130)) {
		return -ENODEV;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	k_mutex_lock(&rx8130_timer_lock, K_FOREVER);
	ret = i2c_reg_read_byte_dt(&rx8130, RX8130_REG_FLAG, &flag);
	if (ret == 0) {
		if (timer) {
			flag &= ~RX8130_FLAG_TF;
		}
		if (alarm) {
			flag &= ~RX8130_FLAG_AF;
		}
		ret = i2c_reg_write_byte_dt(&rx8130, RX8130_REG_FLAG, flag);
	}
	k_mutex_unlock(&rx8130_timer_lock);
	return ret;
}
