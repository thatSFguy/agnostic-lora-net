// board_config.h — hardware wiring + network-wide PHY parameters.
//
// Everything that is a property of the *radio hardware* or of the *network's
// physical layer* lives here, isolated from firmware logic. The radio HAL and app
// code use the symbolic names below and stay board-independent.
//
// Exactly one AGN_BOARD_* is selected per build env (platformio.ini). All three
// supported boards carry an SX1262 on the nRF52's SPI; following MeshCore (the
// firmware we borrow the radio layer from), the SX1262 sits on the default `SPI`
// object whose pins are remapped to the LoRa pins at init via SPI.setPins() — NOT
// a separate SPIClass instance. Pin/TCXO/RXEN values are taken from MeshCore's
// board definitions.
#pragma once

#if !defined(AGN_BOARD_RAK4631) && !defined(AGN_BOARD_XIAO) && !defined(AGN_BOARD_PROMICRO)
#  define AGN_BOARD_RAK4631   // default (also used by the host compile-check env)
#endif

// ---------------------------------------------------------------------------
// Per-board radio wiring + the SX1262 analog quirks (TCXO voltage, RXEN, power).
// ---------------------------------------------------------------------------
#if defined(AGN_BOARD_RAK4631)
// RAK WisBlock RAK4631. MeshCore defines no TCXO voltage here -> RadioLib default
// 1.6 V (with a 0.0 V crystal fallback in the HAL). DIO2 RF switch, no RXEN.
#  define LORA_SPI_NSS   42   // P1.10
#  define LORA_DIO1      47   // P1.15
#  define LORA_NRST      38   // P1.06
#  define LORA_BUSY      46   // P1.14
#  define LORA_SPI_SCK   43   // P1.11
#  define LORA_SPI_MOSI  44   // P1.12
#  define LORA_SPI_MISO  45   // P1.13
#  define LORA_TCXO_VOLTAGE 1.6f

#elif defined(AGN_BOARD_XIAO)
// Seeed XIAO nRF52840 + Wio-SX1262. SX1262 on the XIAO's default SPI bus, 1.8 V
// TCXO, DIO2 RF switch + an RXEN LNA line. (MeshCore: xiao_nrf52.)
#  define LORA_SPI_NSS   D4
#  define LORA_DIO1      D1
#  define LORA_NRST      D2
#  define LORA_BUSY      D3
#  define LORA_SPI_SCK   PIN_SPI_SCK
#  define LORA_SPI_MOSI  PIN_SPI_MOSI
#  define LORA_SPI_MISO  PIN_SPI_MISO
#  define LORA_RXEN      D5
#  define LORA_TCXO_VOLTAGE 1.8f

#elif defined(AGN_BOARD_PROMICRO)
// Pro Micro nRF52840 ("faketec") + SX1262. 1.8 V TCXO, DIO2 RF switch, RXEN, and a
// radio power-enable pin. (MeshCore: promicro.)
#  define LORA_SPI_NSS   13
#  define LORA_DIO1      11
#  define LORA_NRST      10
#  define LORA_BUSY      16
#  define LORA_SPI_SCK   12
#  define LORA_SPI_MOSI  14
#  define LORA_SPI_MISO  15
#  define LORA_RXEN      2
#  define LORA_POWER_EN  21   // must be driven HIGH to power the radio module
#  define LORA_TCXO_VOLTAGE 1.8f
#endif

// ---------------------------------------------------------------------------
// Network-wide PHY (Agent.md §3 "Confirmed parameters"). ALL nodes must match
// these to hear each other — LongFast preset on a single US channel.
// ---------------------------------------------------------------------------
#define PHY_FREQ_MHZ      904.375f  // single fixed channel for v1
#define PHY_BW_KHZ        250.0f    // bandwidth
#define PHY_SF            11        // spreading factor
#define PHY_CODING_RATE   5         // 4/5  (RadioLib takes the denominator: 5..8)
#define PHY_SYNC_WORD     0x4D      // clear of MeshCore 0x12 / Meshtastic 0x2B / LoRaWAN 0x34
#define PHY_PREAMBLE_SYMS 16        // preamble length in symbols
#define PHY_TX_POWER_DBM  14        // conservative bench default; FCC/airtime budget tuned later (Tier 1)

// SX1262 hardware ceiling (used to range-check PHY_TX_POWER_DBM at init).
#define LORA_MAX_TX_POWER_DBM 22
