// radio_hal.h — interrupt-driven, never-blocking SX1262 transport.
//
// Design principle #6 (Agent.md §2): the radio core NEVER blocks. At SF11/BW250 a
// single TX/RX is hundreds of ms to >1 s; busy-waiting that long starves the BLE
// stack and drops the phone link (observed in a prior LoRaMesher attempt). So:
//
//   * all radio ops use RadioLib's async API (startTransmit / startReceive),
//   * the SX1262 DIO1 line fires an ISR that only sets a volatile flag,
//   * poll() — called from loop() — does the actual SPI work outside interrupt
//     context and re-arms RX, never spinning on airtime.
//
// This HAL owns exactly one responsibility: get bytes on and off the air and
// report RSSI/SNR. It knows nothing about packet structure or routing — those sit
// above it (and, in the real firmware, replace MeshCore's routing seam while this
// async radio model is borrowed from working firmware, §9).
#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include "packet.h"

// ISR placement attribute: ESP32/ESP8266 want the handler in IRAM; nRF52 et al.
// have no such requirement, so this collapses to nothing there.
#if defined(ESP32) || defined(ESP8266)
#  define AGN_ISR_ATTR IRAM_ATTR
#else
#  define AGN_ISR_ATTR
#endif

// Callback invoked from poll() (NOT from interrupt context) when a frame arrives.
// `buf`/`len` are valid only for the duration of the call. rssi in dBm, snr in dB.
typedef void (*RadioRxCallback)(const uint8_t* buf, uint16_t len, float rssi, float snr);

// Runtime-configurable PHY parameters. Defaults come from board_config.h's PHY_*
// (see radio_default_config()); the management console (`rf …`) and web/manage.html
// can change them live and persist them in flash. ALL nodes MUST share these values
// or they can't hear each other, so a network-wide change is a coordinated "retune"
// (see docs/remote-config.md). Stored as integers so it round-trips through the
// console/flash with no float-formatting drift.
struct RadioCfg {
    uint32_t freq_hz;      // carrier, e.g. 904375000
    uint32_t bw_hz;        // bandwidth in Hz (250000 = 250 kHz; exact for 31250/41670/…)
    uint8_t  sf;           // spreading factor 5..12
    uint8_t  cr;           // coding-rate denominator 5..8  (= 4/5 .. 4/8)
    int8_t   power_dbm;    // TX power, -9..+22 dBm (clamped to LORA_MAX_TX_POWER_DBM)
    uint8_t  sync;         // sync word
    uint16_t preamble;     // preamble length in symbols
};

// The compile-time network defaults (board_config.h PHY_*). Used when no config has
// been persisted yet, and as the target of a `rf default` reset.
RadioCfg radio_default_config();

class RadioHal {
public:
    RadioHal();

    // One-time init: SPI module, the given PHY params, DIO1 ISR, then arm RX.
    // Returns RADIOLIB_ERR_NONE (0) on success, else a RadioLib error code.
    int16_t begin(RadioRxCallback on_rx, const RadioCfg& cfg);

    // Re-apply PHY parameters to an already-running radio — the runtime `rf` console
    // and (future) authenticated remote-config control plane use this. Returns a
    // RadioLib status; on success the radio is reconfigured and back in RX, and
    // config() reflects the new values.
    int16_t apply_config(const RadioCfg& cfg);

    // The PHY parameters currently in force.
    const RadioCfg& config() const { return cfg_; }

    // Call every loop() iteration. Services a completed TX or RX (set by the ISR)
    // and keeps the radio armed for RX. Cheap and non-blocking when nothing is
    // pending. Returns true if it serviced an event this call.
    bool poll();

    // Queue a frame for transmission. Non-blocking: kicks off startTransmit and
    // returns immediately; completion is handled in a later poll(). Returns false
    // if a TX is already in flight (caller should retry later) or on radio error.
    bool send(const uint8_t* buf, uint16_t len);

    // True while a transmission is in flight (radio can't accept another send()).
    bool busy() const { return state_ == TX; }

    // Metrics of the most recently received frame.
    float last_rssi() const { return last_rssi_; }
    float last_snr()  const { return last_snr_; }

    // Diagnostics.
    uint32_t rx_count()      const { return rx_count_; }
    uint32_t tx_count()      const { return tx_count_; }
    uint32_t rx_err_count()  const { return rx_err_count_; }

private:
    enum State : uint8_t { RX, TX };

    void arm_rx();                   // (re)enter continuous receive
    static void AGN_ISR_ATTR isr();  // DIO1 handler — sets the flag only

    SX1262          radio_;
    RadioRxCallback on_rx_;
    RadioCfg        cfg_;             // PHY parameters currently applied
    volatile State  state_;

    float    last_rssi_;
    float    last_snr_;
    uint32_t rx_count_;
    uint32_t tx_count_;
    uint32_t rx_err_count_;

    // Set by the ISR, cleared in poll(). volatile: written in interrupt context.
    static volatile bool dio1_flag_;
    static RadioHal*     instance_;  // ISR trampoline target (single radio per node)
};
