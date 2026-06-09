#include "link_metric.h"

namespace mesh {

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float quality_from_rf(float snr_db, float rssi_dbm) {
    // Linear map of SNR across the usable band -> [0,1].
    float q = (snr_db - SNR_FLOOR_DB) / (SNR_GOOD_DB - SNR_FLOOR_DB);
    q = clampf(q, 0.0f, 1.0f);

    // A frame can decode at very low RSSI yet be unreliable; fold in a mild RSSI
    // penalty so two links with equal SNR but very different RSSI aren't rated
    // identically.
    if (rssi_dbm < RSSI_FLOOR_DBM) {
        q *= 0.5f;
    }

    return clampf(q, Q_MIN, Q_MAX);
}

float ewma_quality(float prev, float sample, float alpha) {
    if (prev < 0.0f) return clampf(sample, Q_MIN, Q_MAX);
    float q = alpha * sample + (1.0f - alpha) * prev;
    return clampf(q, Q_MIN, Q_MAX);
}

} // namespace mesh
