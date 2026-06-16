// board_config.h — hardware wiring + network-wide PHY parameters.
//
// Everything that is a property of the *radio hardware* or of the *network's
// physical layer* lives here, isolated from firmware logic. The radio HAL and app
// code use the symbolic names below and stay board-independent.
//
// Exactly one AGN_BOARD_* is selected per build env (platformio.ini). Most boards
// carry an SX1262; the Seeed T1000-E carries a Semtech LR1110 instead (selected via
// AGN_RADIO_LR1110 below — the radio HAL is written against RadioLib's common LoRa
// API and only branches on the chip for RF-switch setup). Following MeshCore (the
// firmware we borrow the radio layer from), the radio sits on the default `SPI`
// object whose pins are remapped to the LoRa pins at init — via SPI.setPins() on
// nRF52, or SPI.begin(sck,miso,mosi,ss) on ESP32. Pin/TCXO/RXEN/FEM values come from
// MeshCore's and Meshtastic's board definitions.
#pragma once

#if !defined(AGN_BOARD_RAK4631) && !defined(AGN_BOARD_XIAO) && \
    !defined(AGN_BOARD_PROMICRO) && !defined(AGN_BOARD_T1000) && \
    !defined(AGN_BOARD_HELTEC_V4) && !defined(AGN_BOARD_XIAO_S3)
#  define AGN_BOARD_RAK4631   // default (also used by the host compile-check env)
#endif

// ---------------------------------------------------------------------------
// Per-board radio wiring + the analog quirks (radio chip, TCXO voltage, RXEN,
// power-enable, front-end module).
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

#elif defined(AGN_BOARD_T1000)
// Seeed SenseCAP T1000-E. nRF52840 + Semtech LR1110 (NOT an SX1262). 1.6 V TCXO on
// the LR1110's DIO3; the RX/TX path is switched inside the radio (no external RXEN).
// Pins are raw nRF GPIOs from Meshtastic's tracker-t1000-e variant.
#  define AGN_RADIO_LR1110            // selects the RadioLib LR1110 class in the HAL
#  define LORA_SPI_NSS   12   // P0.12
#  define LORA_DIO1      33   // P1.01  (IRQ)
#  define LORA_NRST      42   // P1.10
#  define LORA_BUSY      7    // P0.07
#  define LORA_SPI_SCK   11   // P0.11
#  define LORA_SPI_MOSI  41   // P1.09
#  define LORA_SPI_MISO  40   // P1.08
#  define LORA_TCXO_VOLTAGE 1.6f

#elif defined(AGN_BOARD_HELTEC_V4)
// Heltec WiFi LoRa 32 V4. ESP32-S3 + SX1262 behind a GC1109/KCT8103L front-end
// module (FEM). 1.8 V TCXO on DIO3, antenna switch on the SX1262's DIO2. VEXT
// (active-low) powers the OLED rail + LoRa antenna boost; the FEM is powered and
// switched TX/RX in lockstep with the radio state (see radio_hal.cpp). Pins from
// Meshtastic's heltec_v4 variant.
//   *** The FEM TX/RX control is board-revision-specific (high-power GC1109 CPS on
//   GPIO46 vs low-power KCT8103L CTX on GPIO5). Defaults below target the GC1109;
//   verify on first flash — RF-switch/FEM wiring is the first suspect if the node
//   comes up "radio: up" but never hears or is never heard. Override the LORA_FEM_*
//   pins with -D build flags if your unit uses the KCT8103L population. ***
#  define LORA_SPI_NSS   8
#  define LORA_DIO1      14   // SX1262 IRQ
#  define LORA_NRST      12
#  define LORA_BUSY      13
#  define LORA_SPI_SCK   9
#  define LORA_SPI_MOSI  10
#  define LORA_SPI_MISO  11
#  define LORA_TCXO_VOLTAGE 1.8f
// VEXT: drive to LORA_VEXT_ON_LEVEL to power the OLED rail + LoRa antenna boost.
#  ifndef LORA_VEXT_EN
#    define LORA_VEXT_EN       36
#  endif
#  define LORA_VEXT_ON_LEVEL   LOW
// Front-end module. LORA_FEM_POWER gates the FEM LDO; LORA_FEM_EN is the chip-enable
// (CSD); LORA_FEM_TX_EN selects full-PA (HIGH) on TX vs RX/bypass (LOW) on RX.
#  ifndef LORA_FEM_POWER
#    define LORA_FEM_POWER     7
#  endif
#  ifndef LORA_FEM_EN
#    define LORA_FEM_EN        2
#  endif
#  ifndef LORA_FEM_TX_EN
#    define LORA_FEM_TX_EN     46
#  endif

#elif defined(AGN_BOARD_XIAO_S3)
// Seeed XIAO ESP32-S3 + the SAME Wio-SX1262 expansion as the nRF XIAO (Meshtastic: "Seeed Xiao
// ESP32-S3"). The Wio module plugs into the standard XIAO pad layout, so the LOGICAL pads are
// identical to AGN_BOARD_XIAO — NSS=D4, DIO1=D1, NRST=D2, BUSY=D3, RXEN=D5, default SPI on
// D8/D9/D10 — only the silicon behind each pad differs (ESP32-S3 GPIOs vs nRF52). 1.8 V TCXO,
// DIO2 RF switch + an RXEN LNA line, no FEM. ESP32 brings the bus up via SPI.begin(sck,miso,
// mosi,ss) in the HAL, so the SPI pins are the XIAO's default SPI pads.
//   *** Pads match the Wio-SX1262-for-XIAO wiring, but VERIFY ON FIRST FLASH per project policy:
//   if it comes up "radio: up" but never hears / is never heard, RF-switch/RXEN wiring (or a
//   D-pad→GPIO mismatch in the board variant) is the first suspect. ***
#  define LORA_SPI_NSS   D4
#  define LORA_DIO1      D1
#  define LORA_NRST      D2
#  define LORA_BUSY      D3
#  define LORA_SPI_SCK   D8
#  define LORA_SPI_MISO  D9
#  define LORA_SPI_MOSI  D10
#  define LORA_RXEN      D5
#  define LORA_TCXO_VOLTAGE 1.8f
#endif

// ---------------------------------------------------------------------------
// Network-wide PHY (Agent.md §3 "Confirmed parameters"). ALL nodes must match
// these to hear each other — LongFast preset on a single US channel.
// ---------------------------------------------------------------------------
#define PHY_FREQ_MHZ      906.625f  // single fixed channel for v1. One 250 kHz slot below
                                    // Meshtastic US LongFast (906.875) on the US915 raster:
                                    // close, non-overlapping, and clear of the busier lower
                                    // LoRaWAN-uplink area. Network-wide — all nodes must match.
#define PHY_BW_KHZ        250.0f    // bandwidth
#define PHY_SF            9         // spreading factor: 5 dB more budget than SF7,
                                    // texts still ~2 s — the deployment sweet spot
#define PHY_CODING_RATE   5         // 4/5  (RadioLib takes the denominator: 5..8)
#define PHY_SYNC_WORD     0x4D      // clear of MeshCore 0x12 / Meshtastic 0x2B / LoRaWAN 0x34
#define PHY_PREAMBLE_SYMS 16        // preamble length in symbols
#define PHY_TX_POWER_DBM  22        // SX1262 max (Meshtastic parity); FCC 915 ISM allows it

// SX1262 hardware ceiling (used to range-check PHY_TX_POWER_DBM at init).
#define LORA_MAX_TX_POWER_DBM 22
