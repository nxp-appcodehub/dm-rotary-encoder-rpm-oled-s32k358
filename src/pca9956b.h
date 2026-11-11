/*
 * Copyright 2024-2026 NXP
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PCA9956B_H_
#define PCA9956B_H_

#include <stdint.h>
#include <stdbool.h>

/* I2C configuration */
#define PCA9956B_LPI2C_INSTANCE    (1U)     /* LPI2C1, PTC6 = SDA, PTC7 = SCL */
#define PCA9956B_SLAVE_ADDR_7BIT   (0x15U)
/* Timeout passed to Lpi2c_Ip_Master*DataBlocking. This project uses the
 * OSIF dummy counter (I2C_TIMEOUT_TYPE), where each poll counts as one tick,
 * so the number of polls is roughly sqrt(2 * timeout). Use the same large
 * value as the other working LPI2C projects. */
#define PCA9956B_I2C_TIMEOUT       (1000000U)

/* Registers */
#define PCA9956B_REG_MODE1         (0x00U)
#define PCA9956B_REG_MODE2         (0x01U)
#define PCA9956B_REG_LEDOUT0       (0x02U)
#define PCA9956B_REG_PWMALL        (0x3FU)
#define PCA9956B_REG_IREFALL       (0x40U)
#define PCA9956B_AUTO_INC_FLAG     (0x80U)
#define PCA9956B_MODE1_RST_VAL     (0x09U)

#define PCA9956B_NUM_LEDS          (24U)
#define PCA9956B_IREFALL_INIT_VAL  (0xFFU)  /* max current, about 8.4 mA per channel */

/* LEDOUT 2-bit modes */
#define PCA9956B_LED_OFF           (0x0U)
#define PCA9956B_LED_ON            (0x1U)
#define PCA9956B_LED_PWM           (0x2U)
#define PCA9956B_LED_GRP           (0x3U)

/**
 * @brief  Initialises the driver: checks the device answers (MODE1 reset value)
 *         and sets IREFALL. The RST pin must already have been pulsed
 *         (done by SSD1306_Init, same PTA13 line).
 * @return 0 on success, non-zero on I2C error or wrong MODE1 value.
 */
int32_t PCA9956B_Init(void);

/** @brief  Raw write of num bytes starting at register reg (auto-increment). */
int32_t PCA9956B_WriteReg(uint8_t reg, const uint8_t *data, uint8_t num);

/** @brief  Raw read of num bytes starting at register reg. */
int32_t PCA9956B_ReadReg(uint8_t reg, uint8_t *data, uint8_t num);

/** @brief  Sets the mode of LEDs [first..last] (inclusive), all others off. */
int32_t PCA9956B_SetRange(uint8_t first, uint8_t last, uint8_t mode);

/** @brief  Lights the first count LEDs (0..24) and turns the rest off. */
int32_t PCA9956B_SetLevel(uint8_t count);

/* Ring orientation. Logical position 0 is the 12 o'clock LED and the logical
 * position increases clockwise. If LED 0 on the PCB is not at 12 o'clock,
 * set PCA9956B_LED_ZERO_OFFSET to the PCB LED index that is at 12 o'clock.
 * If the ring is wired counter-clockwise, set PCA9956B_LED_DIRECTION to -1. */
#define PCA9956B_LED_ZERO_OFFSET   (0)
#define PCA9956B_LED_DIRECTION     (1)

/** @brief  Lights exactly one LED at logical ring position pos (0..23, 0 =
 *          12 o'clock, clockwise) and turns all the others off. pos is
 *          wrapped modulo 24. */
int32_t PCA9956B_SetSingle(int32_t pos);

/** @brief  Signed level: value > 0 lights LEDs 0..n-1, value < 0 lights
 *          LEDs 23 down to 24-n (opposite direction), n = |value|*24/maxAbs. */
int32_t PCA9956B_SetSigned(int32_t value, int32_t maxAbs);

#endif /* PCA9956B_H_ */
