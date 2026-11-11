/*==================================================================================================
 * Project : RTD AUTOSAR 4.9
 * Platform : CORTEXM
 * Peripheral : S32K3XX
 * Dependencies : none
 *
 * Autosar Version : 4.9.0
 * Autosar Revision : ASR_REL_4_9_REV_0000
 * Autosar Conf.Variant :
 * SW Version : 7.0.1
 * Build Version : S32K3_RTD_7_0_1_D2602_ASR_REL_4_9_REV_0000_20260206
 *
 * Copyright 2020 - 2026 NXP
 *
 *   NXP Proprietary. This software is owned or controlled by NXP and may only be
 *   used strictly in accordance with the applicable license terms. By expressly
 *   accepting such terms or by downloading, installing, activating and/or otherwise
 *   using the software, you are agreeing that you have read, and that you agree to
 *   comply with and are bound by, such license terms. If you do not agree to be
 *   bound by the applicable license terms, then you may not retain, install,
 *   activate or otherwise use the software.
 ==================================================================================================*/

/*==================================================================================================
 *   @file    main.c
 *   @brief   Rotary Encoder Position / RPM Dashboard - Main Entry Point
 *   @details Initialises all peripherals (clocks, GPIO, FlexIO I2C, OLED, SIUL2 ICU, LPI2C LED
 *            ring) and runs the main loop that renders the value of a rotary encoder on an
 *            SSD1306 OLED display and on the 24 LED ring of the Knob G click.
 *
 *            Two modes, selected with the SW2 button (PTH1), default is Position mode:
 *
 *            POSITION mode (GREEN_LED0 off)
 *              Multi-turn relative position from -3600 to +3600 degrees (10 turns in each
 *              direction). One encoder click moves the position by 15 degrees (one LED of the
 *              ring). The position stops at the limits. The OLED shows the total angle, the
 *              number of turns and a centered bar graph. The ring shows the pointer inside
 *              the current turn with a single LED: LED 0 at 12 o'clock, clockwise.
 *
 *            RPM mode (GREEN_LED0 on, PTG30)
 *              Signed RPM value from -2400 to +2400 in steps of 100 per click. The OLED shows
 *              the value and a centered bar graph. The ring fills in one direction for
 *              positive values (LED 0 up to LED 23) and in the opposite direction for negative
 *              values (LED 23 down to LED 0), one LED per click.
 *
 *            Each mode keeps its own value when the mode is changed. The encoder push button
 *            (ENSW) resets the value of the active mode to 0.
 *
 *            Application flow:
 *              1. Peripheral initialisation (clocks, pins, FlexIO I2C, OLED, SIUL2 ICU).
 *              2. Encoder rotation is handled asynchronously by Icu_ENA_Notification() /
 *                 Icu_ENB_Notification(), which run a quadrature decode and update the value
 *                 of the active mode. SW2 is handled by Icu_SW2_Notification(), which only
 *                 sets a flag.
 *              3. The main loop debounces SW2, toggles the mode, drives GREEN_LED0 and
 *                 redraws the OLED and the LED ring whenever the active value or the mode
 *                 changes.
 *
 *   @board   FRDM-A-S32K358
 *   @mcu     S32K358 (Cortex-M7)
 *==================================================================================================*/

/*==================================================================================================
 *                                         INCLUDES
 *==================================================================================================*/
#include "Mcal.h"
#include "Clock_Ip.h"
#include "Siul2_Port_Ip.h"
#include "Siul2_Dio_Ip.h"
#include "Siul2_Icu_Ip.h"
#include "Flexio_Mcl_Ip.h"
#include "Flexio_I2c_Ip.h"
#include "OsIf.h"
#include "IntCtrl_Ip.h"
#include <stdio.h>
#include <stdbool.h>
#include "ssd1306.h"
#include "Lpi2c_Ip.h"
#include "Lpi2c_Ip_Sa_PBcfg.h"
#include "pca9956b.h"

/*==================================================================================================
 *                                       DEFINES
 *==================================================================================================*/

/* ---- Position mode ------------------------------------------------------ */
/** @brief  Positions moved per encoder detent (1 click = 1 LED). */
#define POS_STEP          ((int32_t)1)
/** @brief  Angle covered by one click (360 / 24 LEDs). */
#define POS_DEG_PER_CLICK ((int32_t)15)
/** @brief  Angle of one full turn. */
#define POS_DEG_PER_TURN  ((int32_t)360)
/** @brief  Maximum total angle in each direction (10 turns). */
#define POS_MAX_DEG       ((int32_t)3600)
/** @brief  Maximum position in clicks in each direction (3600 / 15 = 240). */
#define POS_LIMIT         (POS_MAX_DEG / POS_DEG_PER_CLICK)

/* ---- RPM mode ----------------------------------------------------------- */
/** @brief  RPM change per encoder detent. */
#define RPM_STEP          ((int32_t)100)
/** @brief  Minimum RPM value. */
#define RPM_MIN           ((int32_t)-2400)
/** @brief  Maximum RPM value. */
#define RPM_MAX           ((int32_t)2400)

/* ---- SW2 and GREEN_LED0 ------------------------------------------------- */
/** @brief  Minimum time between two accepted SW2 presses, in microseconds. */
#define SW2_DEBOUNCE_US   (200000U)
/** @brief  Pin level that turns GREEN_LED0 on. Set to 0U if the LED is active low. */
#define GREEN_LED_ON_LEVEL  (1U)
/** @brief  Pin level that turns GREEN_LED0 off. */
#define GREEN_LED_OFF_LEVEL (0U)
/** @brief  SIUL2 ICU hardware channel of SW2 (EIRQ17, see Siul2_Icu_Ip_SA_PBcfg.c). */
#define SW2_ICU_CHANNEL   (17U)

/*==================================================================================================
 *                                  TYPES AND GLOBAL VARIABLES
 *==================================================================================================*/

/** @brief  Application modes. */
typedef enum {
	APP_MODE_POSITION = 0, /**< Multi-turn position, GREEN_LED0 off (default) */
	APP_MODE_RPM = 1       /**< Signed RPM value, GREEN_LED0 on */
} app_mode_t;

/** @brief  Application exit code; read by the debugger / startup code on exit. */
volatile int exit_code = 0;

/* -------------------------------------------------------------------------
 * Rotary encoder configuration (quadrature phases ENA/ENB).
 * The encoder phases are read via the low-level Siul2_Icu_Ip driver (this
 * project does not use the AUTOSAR Icu wrapper) using both-edge interrupt
 * detection:
 *   - ENA (PTD1)  -> SIUL2 instance 0, HW channel 9  -> Icu_ENA_Notification
 *   - ENB (PTB17) -> SIUL2 instance 0, HW channel 31 -> Icu_ENB_Notification
 * (see generate/src/Siul2_Icu_Ip_SA_PBcfg.c for the channel configuration:
 * both channels are SIUL2_ICU_BOTH_EDGES with the notification pointers
 * already wired to the two functions below).
 *
 * On every edge (on either phase) the current level of both phases is
 * sampled directly from the SIUL2 GPIO data registers (via
 * Siul2_Dio_Ip_ReadPin, using the ENA_PORT/ENA_PIN and ENB_PORT/ENB_PIN
 * defines from board/Siul2_Port_Ip_Cfg.h) and combined with the previous
 * 2-bit AB state to run a standard quadrature decode (4x resolution
 * gray-code state table). Each valid detent changes the value of the
 * active mode by one step.
 * ------------------------------------------------------------------------- */

/* Quadrature gray-code decode table.
 * Index = (previous AB state << 2) | (current AB state), where AB state is
 * encoded as (A << 1) | B.
 *  +1 = clockwise step, -1 = counter-clockwise step, 0 = no movement / bounce
 *  (invalid double transition, ignored). */
static const int8_t s_quadratureTable[16] = {
	/* prevAB=00 */0, -1, 1, 0,
	/* prevAB=01 */1, 0, 0, -1,
	/* prevAB=10 */-1, 0, 0, 1,
	/* prevAB=11 */0, 1, -1, 0
};

static volatile uint8_t s_encoderPrevState = 0U;

/** @brief  Active mode, toggled by SW2 in the main loop. Default: Position. */
volatile app_mode_t g_mode = APP_MODE_POSITION;

/** @brief  Multi-turn position in clicks (-POS_LIMIT..POS_LIMIT, 1 click =
 *          15 degrees, positive = clockwise from the zero point). Used in
 *          Position mode. Modified only by the encoder notifications. */
volatile int32_t position = 0;

/** @brief  Signed RPM value (RPM_MIN..RPM_MAX). Used in RPM mode. Modified
 *          only by the encoder notifications. */
volatile int32_t rpm = 0;

/** @brief  PCA9956B init result (0 = OK); non-zero disables the LED ring. */
volatile int32_t g_ledStatus = -1;

/** @brief  Set by Icu_SW2_Notification(), consumed by the main loop. */
static volatile bool s_sw2Pressed = false;

/** @brief  Accumulates decoded quadrature steps; the value is changed once
 *          every QUAD_STEPS_PER_DETENT steps so one physical detent = one
 *          step. */
#define QUAD_STEPS_PER_DETENT ((int8_t)2)
static volatile int8_t s_stepAccum = 0;


/*==================================================================================================
 *                                  FUNCTION PROTOTYPES
 *==================================================================================================*/

/**
 * @brief   Draws the static title of the given mode on the OLED frame buffer.
 */
static void DrawTitle(app_mode_t mode);

/**
 * @brief   Draws the Position mode screen (turn counter, angle, centered bar).
 * @param[in] pos  Position in clicks (-POS_LIMIT..POS_LIMIT).
 */
static void Render_Position(int32_t pos);

/**
 * @brief   Draws the RPM mode screen (RPM value, centered bar).
 * @param[in] value  RPM value (RPM_MIN..RPM_MAX).
 */
static void Render_Rpm(int32_t value);

/**
 * @brief   Updates the 24 LED ring for the given mode and value.
 */
static void Update_Leds(app_mode_t mode, int32_t value);

/**
 * @brief   Switches GREEN_LED0 on for RPM mode and off for Position mode.
 */
static void Update_ModeLed(app_mode_t mode);

/**
 * @brief   Shared quadrature update helper, called from both ENA and ENB
 *          notifications (either edge on either phase triggers this).
 *          Reads the current level of both encoder phases directly from the
 *          SIUL2 GPIO data registers, builds the 2-bit AB state, looks up
 *          the direction in the quadrature table and, on a valid detent,
 *          changes the value of the active mode by one step.
 *          Runs in ICU interrupt context, keep it short.
 */
static void Encoder_Update(void);

/*==================================================================================================
 *                                       MAIN
 *==================================================================================================*/

/**
 * @brief   Application entry point.
 * @details Performs the following sequence:
 *          - Initialises system clocks via Clock_Ip.
 *          - Initialises OsIf tick counter.
 *          - Configures all GPIO pins (FlexIO I2C, RST, ENA/ENB, SW2, LED, etc.).
 *          - Initialises FlexIO I2C master for SSD1306 OLED communication.
 *          - Initialises SSD1306 display.
 *          - Initialises the SIUL2 ICU driver and enables both-edge
 *            interrupt detection + notifications for the ENA/ENB encoder
 *            channels, and the notifications of ENSW and SW2.
 *          - Enters the infinite loop that handles SW2 and redraws the
 *            dashboard and the LED ring.
 *
 * @return  exit_code (0 under normal operation).
 */
int main(void) {
	/* -----------------------------------------------------------------------
	 * Peripheral Initialisation
	 * --------------------------------------------------------------------- */
	Clock_Ip_Init(Clock_Ip_aClockConfig);
	OsIf_Init(NULL_PTR);

	/* Configure all pins: FlexIO SCL/SDA, RST (OLED), ENA/ENB (encoder), etc. */
	Siul2_Port_Ip_Init(
	NUM_OF_CONFIGURED_PINS_PortContainer_0_BOARD_InitPeripherals,
			g_pin_mux_InitConfigArr_PortContainer_0_BOARD_InitPeripherals);

	/* Default mode is Position: GREEN_LED0 off. */
	Update_ModeLed(g_mode);

	/* FlexIO I2C instance 0, channel 0 - SSD1306 OLED display */
	Flexio_Mcl_Ip_InitDevice(&Flexio_Ip_Sa_xFlexioInit);
	Flexio_I2c_Ip_MasterInit(OLED_FLEXIO_INSTANCE, OLED_FLEXIO_CHANNEL,
			&Flexio_I2cMasterChannel0);

	/* Enable the LED ring outputs: PCA9956B OE is active low (PTA1). */
	Siul2_Dio_Ip_WritePin(ENOE_PORT, ENOE_PIN, 0U);

	/* Initialise OLED (reset sequence + command stream) */
	SSD1306_Init();

	/* (The LED ring PCA9956B is initialised below, after IntCtrl_Ip_Init.) */

	/* Initialise the interrupt controller so the NVIC actually routes the
	 * SIUL2 external interrupt vectors (SIUL2_EXT_IRQ_8_15_ISR for EIRQ9/ENA
	 * and EIRQ10/ENSW, SIUL2_EXT_IRQ_16_23_ISR for EIRQ17/SW2 and
	 * SIUL2_EXT_IRQ_24_31_ISR for EIRQ31/ENB, see IntCtrl_Ip_Cfg.c) to
	 * their ISR handlers. Without this call the NVIC never dispatches these
	 * interrupts, so Siul2_Icu_Ip_EnableInterrupt/EnableNotification below
	 * would have no effect and the notifications would never be reached. */
	(void) IntCtrl_Ip_Init(&IntCtrlConfig_0);

	/* Knob G Click LED ring (PCA9956B) on LPI2C1 (PTC6 SDA, PTC7 SCL).
	 * The RST line (PTA13) was already pulsed by SSD1306_Init.
	 * LPI2C1 is interrupt driven, so it must be initialised after
	 * IntCtrl_Ip_Init (LPI2C1_Master_Slave_IRQHandler is enabled there). */
	Lpi2c_Ip_MasterInit(1U, &I2c_Lpi2cMaster_HwChannel1_Channel0);
	g_ledStatus = PCA9956B_Init();

	/* Initialise the SIUL2 ICU driver and enable the interrupt detection +
	 * notifications:
	 *   HW channel 9  -> ENA (PTD1)  -> Icu_ENA_Notification
	 *   HW channel 31 -> ENB (PTB17) -> Icu_ENB_Notification
	 *   HW channel 10 -> ENSW (PTD2) -> Icu_ENSW_Notification
	 *   HW channel 17 -> SW2 (PTH1)  -> Icu_SW2_Notification */
	(void) Siul2_Icu_Ip_Init(0U, &Siul2_Icu_Ip_0_Config_PB);

	Siul2_Icu_Ip_EnableInterrupt(0U, 9U);
	Siul2_Icu_Ip_EnableNotification(0U, 9U);
	Siul2_Icu_Ip_EnableInterrupt(0U, 10U);
	Siul2_Icu_Ip_EnableNotification(0U, 10U);
	Siul2_Icu_Ip_EnableInterrupt(0U, 31U);
	Siul2_Icu_Ip_EnableNotification(0U, 31U);
	Siul2_Icu_Ip_EnableInterrupt(0U, SW2_ICU_CHANNEL);
	Siul2_Icu_Ip_EnableNotification(0U, SW2_ICU_CHANNEL);

	/* -----------------------------------------------------------------------
	 * Local Variables
	 * --------------------------------------------------------------------- */
	int32_t last_value = 0;
	bool redraw = true; /* full redraw on start-up and on every mode change */

	/* Software debounce of SW2 with the OsIf system counter */
	uint32 debounceTicks = OsIf_MicrosToTicks(SW2_DEBOUNCE_US,
			OSIF_COUNTER_SYSTEM);
	uint32 lastTick = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
	uint32 sinceToggle = debounceTicks; /* the first press is accepted */

	/* -----------------------------------------------------------------------
	 * Main Application Loop
	 * --------------------------------------------------------------------- */
	for (;;) {
		app_mode_t mode;
		int32_t current_value;

		/* ---- SW2: debounce and toggle the mode ------------------------- */
		if (sinceToggle < debounceTicks) {
			sinceToggle += OsIf_GetElapsed(&lastTick, OSIF_COUNTER_SYSTEM);
		} else {
			lastTick = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
		}

		if (s_sw2Pressed) {
			s_sw2Pressed = false;
			if (sinceToggle >= debounceTicks) {
				g_mode = (g_mode == APP_MODE_POSITION) ?
						APP_MODE_RPM : APP_MODE_POSITION;
				s_stepAccum = 0;
				Update_ModeLed(g_mode);
				redraw = true;
				sinceToggle = 0U;
				lastTick = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
			}
		}

		/* ---- Redraw when the mode or the active value changed ---------- */
		mode = g_mode;
		current_value = (mode == APP_MODE_POSITION) ? position : rpm;

		if (redraw || (current_value != last_value)) {
			if (redraw) {
				SSD1306_ClearBuffer();
				DrawTitle(mode);
			}

			if (mode == APP_MODE_POSITION) {
				Render_Position(current_value);
			} else {
				Render_Rpm(current_value);
			}
			SSD1306_UpdateScreen();

			Update_Leds(mode, current_value);

			last_value = current_value;
			redraw = false;
		}

		/* Short busy-wait when idle to avoid hammering the I2C bus */
		for (volatile int i = 0; i < 30000; i++)
			;
	} /* end for(;;) */

	return exit_code;
}

/*==================================================================================================
 *                                  LOCAL FUNCTIONS
 *==================================================================================================*/

/**
 * @brief   Draws the static title of the given mode.
 * @details Layout (96 x 39 px display):
 *
 *          Position mode                       RPM mode
 *          +--------------------------+        +--------------------------+
 *          | Position       Turn -10  |        | RPM                      |
 *          |         [-3600]          |        |         [-2400]          |
 *          |  [####|              ]   |        |  [####|              ]   |
 *          +--------------------------+        +--------------------------+
 */
static void DrawTitle(app_mode_t mode) {
	if (mode == APP_MODE_POSITION) {
		/* 8 chars * 6 px = 48 px, left part of the top line. The turn counter
		 * is drawn by Render_Position() in the right part (x = 48..95). */
		SSD1306_DrawString(0, 0, "Position", 1, true);
	} else {
		SSD1306_DrawString(0, 0, "RPM", 1, true);
	}
}

static void Render_Position(int32_t pos) {
	char text[16];
	/* Total signed angle: -3600..+3600 degrees */
	int32_t angle = pos * POS_DEG_PER_CLICK;
	/* Number of complete turns, truncated towards zero: -10..+10 */
	int32_t turns = angle / POS_DEG_PER_TURN;

	/* ---- Turn counter (top line, right side) --------------------------- */
	SSD1306_FillRect(48, 0, 48, 8, false); /* erase old value */
	snprintf(text, sizeof(text), "Turn %+3ld", (long) turns);
	SSD1306_DrawString(48, 0, text, 1, true);

	/* ---- Numeric angle field (total degrees) --------------------------- */
	SSD1306_FillRect(30, 10, 60, 16, false); /* erase old value */
	snprintf(text, sizeof(text), "%5ld", (long) angle);
	SSD1306_DrawString(30, 10, text, 2, true);

	/* ---- Centered bar graph (-3600..+3600 degrees) --------------------- */
	SSD1306_DrawCenteredBar(4, 28, 88, 9, angle, POS_MAX_DEG);
}

static void Render_Rpm(int32_t value) {
	char text[16];

	/* ---- Numeric RPM field --------------------------------------------- */
	SSD1306_FillRect(30, 10, 60, 16, false); /* erase old value */
	snprintf(text, sizeof(text), "%5ld", (long) value);
	SSD1306_DrawString(30, 10, text, 2, true);

	/* ---- Centered bar graph (-2400..+2400 RPM) ------------------------- */
	SSD1306_DrawCenteredBar(4, 28, 88, 9, value, RPM_MAX);
}

static void Update_Leds(app_mode_t mode, int32_t value) {
	if (mode == APP_MODE_POSITION) {
		/* Exactly one LED lit = pointer inside the current turn
		 * (position modulo 24), LED 0 at 12 o'clock, clockwise. */
		(void) PCA9956B_SetSingle(value);
	} else {
		/* 0..2400 lights LED 0 up to LED 23, 0..-2400 lights LED 23 down
		 * to LED 0 (opposite direction). One click = 100 RPM = one LED. */
		(void) PCA9956B_SetSigned(value, RPM_MAX);
	}
}

static void Update_ModeLed(app_mode_t mode) {
	Siul2_Dio_Ip_WritePin(GREEN_LED0_PORT, GREEN_LED0_PIN,
			(mode == APP_MODE_RPM) ? GREEN_LED_ON_LEVEL : GREEN_LED_OFF_LEVEL);
}

/**
 * @brief   Shared quadrature update helper, called from both ENA and ENB
 *          notifications (either edge on either phase triggers this).
 */
static void Encoder_Update(void) {
	uint8_t levelA;
	uint8_t levelB;
	uint8_t currState;
	int8_t direction;

	/* Read the live level of both encoder phases directly from the SIUL2
	 * GPIO data registers. */
	levelA = (uint8_t) Siul2_Dio_Ip_ReadPin(ENA_PORT, ENA_PIN); /* ENA = PTD1  */
	levelB = (uint8_t) Siul2_Dio_Ip_ReadPin(ENB_PORT, ENB_PIN); /* ENB = PTB17 */

	currState = (uint8_t) (((levelA & 0x1U) << 1U) | (levelB & 0x1U));

	direction =
			s_quadratureTable[((s_encoderPrevState & 0x3U) << 2U)
					| (currState & 0x3U)];

	if (direction != 0) {
		s_stepAccum = (int8_t) (s_stepAccum + direction);

		/* One physical detent produces QUAD_STEPS_PER_DETENT valid decoded
		 * transitions, so only change the value once that many have
		 * accumulated. */
		if ((s_stepAccum >= QUAD_STEPS_PER_DETENT)
				|| (s_stepAccum <= -QUAD_STEPS_PER_DETENT)) {
			int8_t dir = (s_stepAccum > 0) ? 1 : -1;
			s_stepAccum = 0;

			/* Sign chosen so clockwise rotation increases the value (and
			 * counter-clockwise decreases it), matching the convention used
			 * in the other encoder demo projects. The values stop at their
			 * end limits, they do not wrap around. */
			if (g_mode == APP_MODE_POSITION) {
				int32_t new_position = position - ((int32_t) dir * POS_STEP);

				if (new_position > POS_LIMIT) {
					new_position = POS_LIMIT;
				} else if (new_position < -POS_LIMIT) {
					new_position = -POS_LIMIT;
				}
				position = new_position;
			} else {
				int32_t new_rpm = rpm - ((int32_t) dir * RPM_STEP);

				if (new_rpm > RPM_MAX) {
					new_rpm = RPM_MAX;
				} else if (new_rpm < RPM_MIN) {
					new_rpm = RPM_MIN;
				}
				rpm = new_rpm;
			}
		}
	}


	s_encoderPrevState = currState;
}

/**
 * @brief ENA (PTD1, SIUL2 instance 0, HW channel 9) both-edge notification.
 */
void Icu_ENA_Notification(void) {
	Encoder_Update();
}


/**
 * @brief ENB (PTB17, SIUL2 instance 0, HW channel 31) both-edge notification.
 */
void Icu_ENB_Notification(void) {
	Encoder_Update();
}

/**
 * @brief ENSW (PTD2, SIUL2 instance 0, HW channel 10) notification.
 *        Resets the value of the active mode to 0.
 */
void Icu_ENSW_Notification(void) {
	s_stepAccum = 0;
	if (g_mode == APP_MODE_POSITION) {
		position = 0;
	} else {
		rpm = 0;
	}
}

/**
 * @brief SW2 (PTH1, SIUL2 instance 0, HW channel 17) notification.
 *        Only sets a flag, the main loop debounces it and toggles the mode.
 */
void Icu_SW2_Notification(void) {
	s_sw2Pressed = true;
}
