#include "radio_hal.h"
#include "board_config.h"
#include <SPI.h>
#include <math.h>   // lroundf

// SX1262 max single-frame size. Stack buffer in poll() is sized to this.
static const uint16_t RADIO_MAX_FRAME = 255;

// The SX1262 sits on the default global `SPI`, whose pins are remapped to the LoRa
// pins in begin() (the MeshCore approach — reuse SPI, don't spin up a second SPIM
// instance, which hangs on nRF52).
static Module* make_lora_module() {
    return new Module(LORA_SPI_NSS, LORA_DIO1, LORA_NRST, LORA_BUSY);
}

// ---- Board power rails: VEXT + front-end module (Heltec V4) -----------------
// On most boards these are no-ops. The Heltec V4 gates the LoRa antenna boost on
// an active-low VEXT line, and feeds the SX1262 through an external PA/LNA (FEM)
// that must be powered and switched between full-PA (TX) and bypass/LNA (RX).
// *** The FEM control is board-revision-specific — verify on first flash (see the
// note in board_config.h). ***
#if defined(LORA_VEXT_EN)
static inline void board_vext_power_up() {
    pinMode(LORA_VEXT_EN, OUTPUT);
    digitalWrite(LORA_VEXT_EN, LORA_VEXT_ON_LEVEL);
}
#else
static inline void board_vext_power_up() {}
#endif

#if defined(LORA_FEM_EN)
static inline void fem_power_up() {
#  if defined(LORA_FEM_POWER)
    pinMode(LORA_FEM_POWER, OUTPUT);
    digitalWrite(LORA_FEM_POWER, HIGH);   // power the FEM LDO
#  endif
    pinMode(LORA_FEM_EN, OUTPUT);
    digitalWrite(LORA_FEM_EN, HIGH);      // chip-enable (CSD)
    pinMode(LORA_FEM_TX_EN, OUTPUT);
    digitalWrite(LORA_FEM_TX_EN, LOW);    // idle in RX/bypass
}
static inline void fem_set_tx(bool tx) { digitalWrite(LORA_FEM_TX_EN, tx ? HIGH : LOW); }
#else
static inline void fem_power_up() {}
static inline void fem_set_tx(bool) {}
#endif

// The compile-time network defaults (board_config.h §"Network-wide PHY").
RadioCfg radio_default_config() {
    RadioCfg c;
    c.freq_hz    = (uint32_t)lroundf(PHY_FREQ_MHZ * 1.0e6f);
    c.bw_hz      = (uint32_t)lroundf(PHY_BW_KHZ * 1000.0f);
    c.sf         = PHY_SF;
    c.cr         = PHY_CODING_RATE;
    c.power_dbm  = PHY_TX_POWER_DBM;
    c.sync       = PHY_SYNC_WORD;
    c.preamble   = PHY_PREAMBLE_SYMS;
    return c;
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

int16_t RadioHal::begin(RadioRxCallback on_rx, const RadioCfg& cfg) {
    instance_ = this;
    on_rx_    = on_rx;
    cfg_      = cfg;

    // Bring up board power rails before talking to the radio. No-ops on boards that
    // don't gate the radio supply / antenna boost / FEM (RAK, XIAO, T1000).
    board_vext_power_up();
    fem_power_up();
#if defined(LORA_VEXT_EN) || defined(LORA_FEM_EN) || defined(LORA_POWER_EN)
#  if defined(LORA_POWER_EN)
    // Some modules (Pro Micro) gate the radio's supply on a GPIO — drive it high.
    pinMode(LORA_POWER_EN, OUTPUT);
    digitalWrite(LORA_POWER_EN, HIGH);
#  endif
    delay(10);   // let the rail(s) settle before the first SPI command
#endif

    // Remap the default SPI bus to the radio's LoRa pins, then start it. On nRF52
    // this is the MeshCore approach (SPI.setPins + SPI.begin) — reuse the global SPI
    // rather than spin up a second SPIM instance (which wedges begin()). ESP32's
    // SPIClass has no setPins(), so pass the pins straight to begin().
#if defined(ESP32)
    SPI.begin(LORA_SPI_SCK, LORA_SPI_MISO, LORA_SPI_MOSI, LORA_SPI_NSS);
#else
    SPI.setPins(LORA_SPI_MISO, LORA_SPI_SCK, LORA_SPI_MOSI);
    SPI.begin();
#endif

    // Range-check the configured TX power against the hardware ceiling.
    int8_t power = cfg_.power_dbm;
    if (power > LORA_MAX_TX_POWER_DBM) power = LORA_MAX_TX_POWER_DBM;

    const float freq_mhz = (float)cfg_.freq_hz / 1.0e6f;
    const float bw_khz   = (float)cfg_.bw_hz / 1000.0f;

    // Apply the full network-wide PHY in one shot. All nodes share
    // these exact values or they can't hear each other.
    int16_t st = radio_.begin(
        freq_mhz,              // carrier (default 904.375 MHz)
        bw_khz,                // bandwidth (default 250 kHz)
        cfg_.sf,               // spreading factor (default SF11)
        cfg_.cr,               // coding rate (default 4/5)
        cfg_.sync,             // sync word (default 0x4D)
        power,                 // dBm
        cfg_.preamble,         // preamble symbols (default 16)
        LORA_TCXO_VOLTAGE);    // board TCXO voltage on DIO3

    // Some modules drive the XOSC from a crystal, not a TCXO; if the configured
    // TCXO voltage makes the SPI command fail, retry in crystal mode (tcxo = 0).
    // (MeshCore does exactly this fallback.)
    if (st == RADIOLIB_ERR_SPI_CMD_FAILED || st == RADIOLIB_ERR_SPI_CMD_INVALID) {
        st = radio_.begin(freq_mhz, bw_khz, cfg_.sf, cfg_.cr,
                          cfg_.sync, power, cfg_.preamble, 0.0f);
    }
    if (st != RADIOLIB_ERR_NONE) return st;

    // CRC on, explicit header (RadioLib default) — matches the LongFast preset (§3).
    st = radio_.setCRC(true);
    if (st != RADIOLIB_ERR_NONE) return st;

#if defined(AGN_RADIO_LR1110)
    // LR1110 (T1000-E): the RX/TX path is switched internally by the radio. Tell
    // RadioLib that no external GPIOs drive the switch (matches Meshtastic's
    // tracker-t1000-e config), then enable the boosted-RX LNA gain. Both are
    // non-fatal — a setup hiccup must not block the radio coming up.
    static const uint32_t rfsw_pins[] = {
        RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC };
    static const Module::RfSwitchMode_t rfsw_table[] = {
        { LR11x0::MODE_STBY,  {} }, { LR11x0::MODE_RX,    {} },
        { LR11x0::MODE_TX,    {} }, { LR11x0::MODE_TX_HP, {} },
        { LR11x0::MODE_TX_HF, {} }, { LR11x0::MODE_GNSS,  {} },
        { LR11x0::MODE_WIFI,  {} }, END_OF_MODE_TABLE };
    radio_.setRfSwitchTable(rfsw_pins, rfsw_table);
    (void)radio_.setRxBoostedGainMode(true);
#else
    // SX1262 boards route the antenna switch through DIO2.
    st = radio_.setDio2AsRfSwitch(true);
    if (st != RADIOLIB_ERR_NONE) return st;
#endif

#if defined(LORA_RXEN)
    // Boards with an external RXEN line (XIAO Wio-SX1262, Pro Micro) have RadioLib
    // drive it for the RX path; TX side is handled by DIO2 above. (Returns void.)
    // NOTE: verify on first flash — if the board reaches "radio up" but never hears
    // traffic, this RF-switch wiring is the first thing to revisit.
    radio_.setRfSwitchPins(LORA_RXEN, RADIOLIB_NC);
#endif

    // Wire the radio's IRQ line -> our ISR, then arm continuous receive. (SX126x
    // raises it on DIO1; the LR1110 names the same hook setIrqAction.)
#if defined(AGN_RADIO_LR1110)
    radio_.setIrqAction(RadioHal::isr);
#else
    radio_.setDio1Action(RadioHal::isr);
#endif
    arm_rx();
    return RADIOLIB_ERR_NONE;
}

// Re-apply PHY parameters to a running radio. Each setter is checked; on the first
// failure we bail WITHOUT updating cfg_ (so config() still reflects what's actually
// on the air) and the caller can report the error. On success the new values stick
// and we drop back into RX. NOTE: changing freq/SF/BW takes the node off-air from
// any peer still on the old PHY — a network-wide change must be coordinated (a
// "retune"; see docs/remote-config.md).
int16_t RadioHal::apply_config(const RadioCfg& c) {
    int8_t power = c.power_dbm;
    if (power > LORA_MAX_TX_POWER_DBM) power = LORA_MAX_TX_POWER_DBM;

    int16_t st;
    if ((st = radio_.setFrequency((float)c.freq_hz / 1.0e6f))      != RADIOLIB_ERR_NONE) return st;
    if ((st = radio_.setBandwidth((float)c.bw_hz / 1000.0f))       != RADIOLIB_ERR_NONE) return st;
    if ((st = radio_.setSpreadingFactor(c.sf))                     != RADIOLIB_ERR_NONE) return st;
    if ((st = radio_.setCodingRate(c.cr))                          != RADIOLIB_ERR_NONE) return st;
    if ((st = radio_.setSyncWord(c.sync))                          != RADIOLIB_ERR_NONE) return st;
    if ((st = radio_.setOutputPower(power))                        != RADIOLIB_ERR_NONE) return st;
    if ((st = radio_.setPreambleLength(c.preamble))               != RADIOLIB_ERR_NONE) return st;

    cfg_ = c;
    arm_rx();   // re-enter RX with the new PHY in force
    return RADIOLIB_ERR_NONE;
}

void RadioHal::arm_rx() {
    state_ = RX;
    fem_set_tx(false);   // FEM to RX/bypass (no-op without a FEM)
    radio_.startReceive();
}

bool RadioHal::send(const uint8_t* buf, uint16_t len) {
    if (state_ != RX || pend_len_ > 0) return false;  // a send is pending or in flight
    if (len == 0 || len > RADIO_MAX_FRAME) return false;
    // A DIO1 event is pending but unserviced (an RX completed since the last poll).
    // Starting a scan/transmit NOW would desync the state machine: the next poll()
    // would consume that stale flag as if it were OUR event — read a garbage CAD
    // verdict, or "finish" a transmission still in the air and truncate it mid-frame.
    // Observed under image load as the radio going deaf+mute for ~60-90 s. Let
    // poll() service the event first; the caller retries next loop.
    if (dio1_flag_) return false;

    // Park the frame; CSMA airs it. With CAD off this degenerates to the
    // pre-0.5 immediate transmit.
    memcpy(pend_buf_, buf, len);
    pend_len_  = len;
    cad_tries_ = 0;
    if (cad_enabled_) start_cad();
    else              start_pending_tx();
    return true;
}

// Listen-before-talk: ask the SX1262 to scan for LoRa activity. DIO1 fires on
// CAD_DONE and poll() reads the verdict. If the scan can't even start, fall
// through to transmitting — a send must never be lost to a scan hiccup.
void RadioHal::start_cad() {
    state_ = CAD;
    if (radio_.startChannelScan() != RADIOLIB_ERR_NONE) start_pending_tx();
}

void RadioHal::start_pending_tx() {
    // FEM to full-PA before keying up (no-op without a FEM). arm_rx() returns it to
    // RX once the transmission completes (or on the failure path below).
    fem_set_tx(true);
    // startTransmit copies the frame into the radio FIFO synchronously, so the
    // pending slot frees as soon as the call returns.
    int16_t st = radio_.startTransmit(pend_buf_, pend_len_);
    pend_len_ = 0;
    if (st != RADIOLIB_ERR_NONE) {
        // Mirror the old send() failure path: recover into RX and drop the frame
        // (per-hop ARQ re-sends data; beacons have the next period).
        arm_rx();
        return;
    }
    state_ = TX;
}

bool RadioHal::poll() {
    // Backoff expired while the channel was busy? Re-scan for the parked frame —
    // but never while a DIO1 event is pending (same stale-flag race as send()).
    if (state_ == RX && pend_len_ > 0 && !dio1_flag_ &&
        (int32_t)(millis() - next_cad_ms_) >= 0)
        start_cad();

    if (!dio1_flag_) return false;
    dio1_flag_ = false;

    if (state_ == TX) {
        // Transmission finished — release the radio and go back to listening.
        radio_.finishTransmit();
        tx_count_++;
        arm_rx();
        return true;
    }

    if (state_ == CAD) {
        // Scan verdict for the parked frame.
        int16_t res = radio_.getChannelScanResult();
        if (res == RADIOLIB_LORA_DETECTED && cad_tries_ + 1 < CAD_MAX_TRIES) {
            // Channel busy: hold off and listen. Randomized exponential backoff
            // (~20-100 ms doubling per round) so two waiting senders don't fire
            // in lock-step when the channel frees — the failure the fixed NACK
            // timer had before 0.4.7.
            cad_busy_count_++;
            cad_tries_++;
            next_cad_ms_ = millis() + ((20 + (uint32_t)random(0, 80)) << (cad_tries_ - 1));
            arm_rx();   // hear the traffic we detected while we wait
        } else {
            // Channel free — or busy too many rounds (send anyway rather than
            // starve; the collision risk is now the lesser evil).
            if (res == RADIOLIB_LORA_DETECTED) { cad_busy_count_++; cad_forced_count_++; }
            start_pending_tx();
        }
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
