// link_metric.h — turn a single received frame's RF stats into a link quality.
//
// The metric model is from the routing sandbox: derive a
// per-direction quality q ∈ [0,1] from RSSI/SNR. LoRa demodulation is
// SNR-limited, so SNR is the primary signal; RSSI only guards against the
// pathological "decoded but absurdly weak" case.
//
// Pure, portable, no Arduino — compiles for the nRF52 firmware and host tests
// alike.
#pragma once

#include <stdint.h>

namespace mesh {

// Quality floor/ceiling in SNR (dB). Below FLOOR a frame is essentially at the
// demod limit for SF11 (~-17 dB); at/above GOOD the link is as good as it gets.
constexpr float SNR_FLOOR_DB = -17.0f;
constexpr float SNR_GOOD_DB  = 8.0f;

// RSSI below this is treated as a hard quality penalty regardless of SNR.
constexpr float RSSI_FLOOR_DBM = -130.0f;

// Clamp link quality into a usable band. Never 0 (would make link cost infinite)
// and never above 1.
constexpr float Q_MIN = 0.02f;
constexpr float Q_MAX = 1.0f;

// Instantaneous quality of one received frame, in [Q_MIN, Q_MAX].
float quality_from_rf(float snr_db, float rssi_dbm);

// Exponentially-weighted update of a running quality estimate. `prev < 0` means
// "no estimate yet" and the new sample is taken as-is.
float ewma_quality(float prev, float sample, float alpha = 0.3f);

} // namespace mesh
