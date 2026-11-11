/*
 * Copyright 2024-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pca9956b.h"
#include "Lpi2c_Ip.h"

static int32_t s_i2cWrite(uint8_t *buf, uint8_t len, bool stop)
{
	Lpi2c_Ip_StatusType st;

	Lpi2c_Ip_MasterSetSlaveAddr(PCA9956B_LPI2C_INSTANCE,
			PCA9956B_SLAVE_ADDR_7BIT, (boolean) FALSE);
	st = Lpi2c_Ip_MasterSendDataBlocking(PCA9956B_LPI2C_INSTANCE, buf, len,
			(boolean) stop, PCA9956B_I2C_TIMEOUT);
	return (st == LPI2C_IP_SUCCESS_STATUS) ? 0 : (int32_t) st;
}

static int32_t s_i2cRead(uint8_t *buf, uint8_t len)
{
	Lpi2c_Ip_StatusType st;

	Lpi2c_Ip_MasterSetSlaveAddr(PCA9956B_LPI2C_INSTANCE,
			PCA9956B_SLAVE_ADDR_7BIT, (boolean) FALSE);
	st = Lpi2c_Ip_MasterReceiveDataBlocking(PCA9956B_LPI2C_INSTANCE, buf, len,
			(boolean) TRUE, PCA9956B_I2C_TIMEOUT);
	return (st == LPI2C_IP_SUCCESS_STATUS) ? 0 : (int32_t) st;
}

int32_t PCA9956B_SetSigned(int32_t value, int32_t maxAbs)
{
	int32_t mag = (value < 0) ? -value : value;
	uint8_t n;

	if (maxAbs <= 0) {
		return -1;
	}
	if (mag > maxAbs) {
		mag = maxAbs;
	}
	n = (uint8_t) ((mag * (int32_t) PCA9956B_NUM_LEDS) / maxAbs);
	if (value >= 0) {
		return PCA9956B_SetLevel(n);
	}
	if (n == 0U) {
		return PCA9956B_SetLevel(0U);
	}
	return PCA9956B_SetRange((uint8_t) (PCA9956B_NUM_LEDS - n),
			(uint8_t) (PCA9956B_NUM_LEDS - 1U), PCA9956B_LED_ON);
}

int32_t PCA9956B_WriteReg(uint8_t reg, const uint8_t *data, uint8_t num)
{
	uint8_t buf[1U + 8U];
	uint8_t i;

	if ((data == NULL) || (num > 8U)) {
		return -1;
	}
	buf[0] = (uint8_t) (reg | PCA9956B_AUTO_INC_FLAG);
	for (i = 0U; i < num; i++) {
		buf[1U + i] = data[i];
	}
	return s_i2cWrite(buf, (uint8_t) (num + 1U), true);
}

int32_t PCA9956B_ReadReg(uint8_t reg, uint8_t *data, uint8_t num)
{
	int32_t st;
	uint8_t cmd = (uint8_t) (reg | PCA9956B_AUTO_INC_FLAG);

	if (data == NULL) {
		return -1;
	}
	st = s_i2cWrite(&cmd, 1U, false); /* repeated start before the read */
	if (st != 0) {
		return st;
	}
	return s_i2cRead(data, num);
}

/* Debug variables, readable in the debugger.
 * g_pcaStepStatus[0] = read MODE1, [1] = write IREFALL, [2] = write PWMALL,
 * [3] = write MODE1 / LEDOUT clear. 0 = OK, otherwise Lpi2c_Ip_StatusType
 * (or -1 / -2 for driver errors). g_pcaMode1 = last MODE1 value read. */
volatile int32_t g_pcaStepStatus[4] = { -100, -100, -100, -100 };
volatile uint8_t g_pcaMode1 = 0xFFU;

int32_t PCA9956B_Init(void)
{
	int32_t st;
	uint8_t mode1 = 0U;
	uint8_t iref = PCA9956B_IREFALL_INIT_VAL;
	uint8_t pwm = 0xFFU;
	uint8_t mode1Cfg = 0x00U;

	st = PCA9956B_ReadReg(PCA9956B_REG_MODE1, &mode1, 1U);
	g_pcaStepStatus[0] = st;
	g_pcaMode1 = mode1;
	if (st != 0) {
		return st;
	}
	if ((mode1 & 0x0FU) != PCA9956B_MODE1_RST_VAL) {
		g_pcaStepStatus[0] = -2;
		return -2; /* device not answering as expected */
	}

	st = PCA9956B_WriteReg(PCA9956B_REG_IREFALL, &iref, 1U);
	g_pcaStepStatus[1] = st;
	if (st != 0) {
		return st;
	}

	st = PCA9956B_WriteReg(PCA9956B_REG_PWMALL, &pwm, 1U);
	g_pcaStepStatus[2] = st;
	if (st != 0) {
		return st;
	}

	/* Make sure the oscillator is running (SLEEP = 0). */
	st = PCA9956B_WriteReg(PCA9956B_REG_MODE1, &mode1Cfg, 1U);
	if (st != 0) {
		g_pcaStepStatus[3] = st;
		return st;
	}

	st = PCA9956B_SetLevel(0U);
	g_pcaStepStatus[3] = st;
	return st;
}

int32_t PCA9956B_SetRange(uint8_t first, uint8_t last, uint8_t mode)
{
	uint8_t ledout[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
	uint8_t i;

	if ((first > last) || (last >= PCA9956B_NUM_LEDS)) {
		return -1;
	}
	for (i = first; i <= last; i++) {
		ledout[i >> 2U] |= (uint8_t) ((mode & 0x3U) << ((i & 0x3U) * 2U));
	}
	return PCA9956B_WriteReg(PCA9956B_REG_LEDOUT0, ledout, 6U);
}

int32_t PCA9956B_SetLevel(uint8_t count)
{
	uint8_t zero[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

	if (count == 0U) {
		return PCA9956B_WriteReg(PCA9956B_REG_LEDOUT0, zero, 6U);
	}
	if (count > PCA9956B_NUM_LEDS) {
		count = PCA9956B_NUM_LEDS;
	}
	return PCA9956B_SetRange(0U, (uint8_t) (count - 1U), PCA9956B_LED_ON);
}

int32_t PCA9956B_SetSingle(int32_t pos)
{
	int32_t n = (int32_t) PCA9956B_NUM_LEDS;
	int32_t phys;

	/* Wrap the logical position to 0..23 (also for negative values) */
	pos = ((pos % n) + n) % n;
	/* Map the logical position (0 = 12 o'clock, clockwise) to the PCB LED */
	phys = (PCA9956B_LED_ZERO_OFFSET + (PCA9956B_LED_DIRECTION * pos)) % n;
	if (phys < 0) {
		phys += n;
	}
	return PCA9956B_SetRange((uint8_t) phys, (uint8_t) phys,
			PCA9956B_LED_ON);
}
