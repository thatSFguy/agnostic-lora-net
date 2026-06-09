#include "radio_hal.h"
#include "board_config.h"
#include <SPI.h>

// SX1262 max single-frame size. Stack buffer in poll() is sized to this.
static const uint16_t RADIO_MAX_FRAME = 255;

// The SX1262 sits on the default global `SPI`, whose pins are remapped to the LoRa
// pins in begin() (the MeshCore approach — reuse SPI, don't spin up a second SPIM
// instance, which hangs on nRF52).
static Module* make_lora_module() {
    return new Module(LORA_SPI_NSS, LORA_DIO1, LORA_NRST, LORA_BUSY);
}

// Static members.
volatile bool RadioHal::dio1_flag_ = false;
RadioHal*     RadioHal::instance_  = nullptr;

RadioHal::RadioHal()
    // Module(NSS, DIO1, NRST, BUSY) — the SX1262's control lines (board_config.h).
    : radio_(make_lora_module()),
      on_rx_(nullptr),
      state_(RX),
      last_rssi_(0.0f),
      last_snr_(0.0f),
      rx_count_(0),
      tx_count_(0),
      rx_err_count_(0) {}

// DIO1 ISR. Keep this to the absolute minimum — just record that the radio raised
// an interrupt. ALL SPI work (which is slow and must not run in interrupt context)
// happens later in poll(). This is what keeps the radio core non-blocking (§2.6).
void AGN_ISR_ATTR RadioHal::isr() {
    dio1_flag_ = true;
}

int16_t RadioHal::begin(RadioRxCallback on_rx) {
    instance_ = this;
    on_rx_    = on_rx;

#if defined(LORA_POWER_EN)
    // Some modules (Pro Micro) gate the radio's supply on a GPIO — power it up and
    // let it settle before talking to it. One-time, at init: safe to delay here.
    pinMode(LORA_POWER_EN, OUTPUT);
    digitalWrite(LORA_POWER_EN, HIGH);
    delay(10);
#endif

    // Remap the default SPI bus to the SX1262's LoRa pins, then start it. This is
    // what MeshCore does on nRF52 (SPI.setPins + SPI.begin) — reusing the global
    // SPI rather than constructing a second SPIM instance (which wedges begin()).
    SPI.setPins(LORA_SPI_MISO, LORA_SPI_SCK, LORA_SPI_MOSI);
    SPI.begin();

    // Range-check the configured TX power against the hardware ceiling.
    int8_t power = PHY_TX_POWER_DBM;
    if (power > LORA_MAX_TX_POWER_DBM) power = LORA_MAX_TX_POWER_DBM;

    // Apply the full network-wide PHY in one shot (Agent.md §3). All nodes share
    // these exact values or they can't hear each other.
    int16_t st = radio_.begin(
        PHY_FREQ_MHZ,          // 904.375 MHz
        PHY_BW_KHZ,            // 250 kHz
        PHY_SF,                // SF11
        PHY_CODING_RATE,       // 4/5
        PHY_SYNC_WORD,         // 0x4D
        power,                 // dBm
        PHY_PREAMBLE_SYMS,     // 16 symbols
        LORA_TCXO_VOLTAGE);    // board TCXO voltage on DIO3

    // Some modules drive the XOSC from a crystal, not a TCXO; if the configured
    // TCXO voltage makes the SPI command fail, retry in crystal mode (tcxo = 0).
    // (MeshCore does exactly this fallback.)
    if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
        st = radio_.begin(PHY_FREQ_MHZ, PHY_BW_KHZ, PHY_SF, PHY_CODING_RATE,
                          PHY_SYNC_WORD, power, PHY_PREAMBLE_SYMS, 0.0f);
    }
    if (st != RADIOLIB_ERR_NONE) return st;

    // CRC on, explicit header (RadioLib default) — matches the LongFast preset (§3).
    st = radio_.setCRC(true);
    if (st != RADIOLIB_ERR_NONE) return st;

    // All three boards route the antenna switch through the SX1262's DIO2.
    st = radio_.setDio2AsRfSwitch(true);
    if (st != RADIOLIB_ERR_NONE) return st;

#if defined(LORA_RXEN)
    // Boards with an external RXEN line (XIAO Wio-SX1262, Pro Micro) have RadioLib
    // drive it for the RX path; TX side is handled by DIO2 above. (Returns void.)
    // NOTE: verify on first flash — if the board reaches "radio up" but never hears
    // traffic, this RF-switch wiring is the first thing to revisit.
    radio_.setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
#endif

    // Wire DIO1 -> our ISR, then arm continuous receive.
    radio_.setDio1Action(RadioHal::isr);
    arm_rx();
    return RADIOLIB_ERR_NONE;
}

void RadioHal::arm_rx() {
    state_ = RX;
    radio_.startReceive();
}

bool RadioHal::send(const uint8_t* buf, uint16_t len) {
    if (state_ == TX) return false;          // a TX is already in flight
    if (len == 0 || len > RADIO_MAX_FRAME) return false;

    // Non-blocking: kick off the transmission and return. DIO1 will fire on
    // completion; poll() finishes up and re-arms RX.
    int16_t st = radio_.startTransmit(const_cast<uint8_t*>(buf), len);
    if (st != RADIOLIB_ERR_NONE) {
        arm_rx();                            // recover the radio into RX
        return false;
    }
    state_ = TX;
    return true;
}

bool RadioHal::poll() {
    if (!dio1_flag_) return false;
    dio1_flag_ = false;

    if (state_ == TX) {
        // Transmission finished — release the radio and go back to listening.
        radio_.finishTransmit();
        tx_count_++;
        arm_rx();
        return true;
    }

    // RX-done: pull the frame out, capture link metrics, hand it up, re-arm.
    uint16_t len = radio_.getPacketLength();
    uint8_t  buf[RADIO_MAX_FRAME];
    if (len > RADIO_MAX_FRAME) len = RADIO_MAX_FRAME;

    int16_t st = radio_.readData(buf, len);
    if (st == RADIOLIB_ERR_NONE) {
        last_rssi_ = radio_.getRSSI();
        last_snr_  = radio_.getSNR();
        rx_count_++;
        if (on_rx_) on_rx_(buf, len, last_rssi_, last_snr_);
    } else {
        // CRC failure / header error / etc. Count it and keep listening — a bad
        // frame is normal on a lossy link and must never wedge the receiver.
        rx_err_count_++;
    }

    arm_rx();
    return true;
}
