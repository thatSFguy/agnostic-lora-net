// variant.h — Seeed SenseCAP T1000-E (nRF52840 + Semtech LR1110).
//
// Minimal Adafruit-nRF5-core variant for this firmware. Only the symbols the core
// and our app actually touch are defined here; the LoRa wiring itself lives in
// include/board_config.h (raw nRF GPIO numbers), so this file stays small. Pin
// assignments are from Meshtastic's tracker-t1000-e variant.
#ifndef _VARIANT_TRACKER_T1000_E_H_
#define _VARIANT_TRACKER_T1000_E_H_

/** Master clock frequency */
#define VARIANT_MCK       (64000000ul)

#define USE_LFXO          // Board uses a 32 kHz crystal for the low-frequency clock

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

// nRF52840: 32 pins on P0 + 16 on P1 = 48, mapped 1:1 in variant.cpp.
#define PINS_COUNT              (48)
#define NUM_DIGITAL_PINS        (48)
#define NUM_ANALOG_INPUTS       (6)
#define NUM_ANALOG_OUTPUTS      (0)

// LED — P0.24 (green), lit when driven HIGH. The T1000-E has a single LED; the
// Bluefruit stack references LED_RED/LED_BLUE for its (runtime-disabled) status +
// connection LEDs, so alias them all to the one LED we have.
#define PIN_LED1                (0 + 24)
#define LED_BUILTIN             PIN_LED1
#define LED_RED                 PIN_LED1
#define LED_BLUE                PIN_LED1
#define LED_GREEN               PIN_LED1
#define LED_STATE_ON            (1)

// User button — P0.06.
#define PIN_BUTTON1             (0 + 6)

// Sensor / accelerometer power rails (driven in initVariant()).
#define PIN_3V3_EN              (32 + 6)   // P1.06
#define PIN_3V3_ACC_EN          (32 + 7)   // P1.07

// Analog inputs (unused by this firmware, but the core expects the symbols).
#define PIN_A0                  (0 + 2)    // P0.02 / AIN0 (BAT_ADC)
#define PIN_A1                  (0 + 3)
#define PIN_A2                  (0 + 4)
#define PIN_A3                  (0 + 5)
#define PIN_A4                  (0 + 28)
#define PIN_A5                  (0 + 29)
static const uint8_t A0 = PIN_A0;
static const uint8_t A1 = PIN_A1;
static const uint8_t A2 = PIN_A2;
static const uint8_t A3 = PIN_A3;
static const uint8_t A4 = PIN_A4;
static const uint8_t A5 = PIN_A5;
#define ADC_RESOLUTION          (14)

// Serial1 — GNSS UART (P0.14 RX / P0.13 TX).
#define PIN_SERIAL1_RX          (0 + 14)
#define PIN_SERIAL1_TX          (0 + 13)

// SPI0 — the LR1110 bus (also declared symbolically in board_config.h).
#define SPI_INTERFACES_COUNT    (1)
#define PIN_SPI_MISO            (32 + 8)   // P1.08
#define PIN_SPI_MOSI            (32 + 9)   // P1.09
#define PIN_SPI_SCK             (0 + 11)   // P0.11

// I2C — accelerometer (unused here, but the core builds a Wire instance from these).
#define WIRE_INTERFACES_COUNT   (1)
#define PIN_WIRE_SDA            (0 + 26)   // P0.26
#define PIN_WIRE_SCL            (0 + 27)   // P0.27

#ifdef __cplusplus
}
#endif

#endif // _VARIANT_TRACKER_T1000_E_H_
