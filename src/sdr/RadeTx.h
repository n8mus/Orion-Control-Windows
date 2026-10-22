// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

struct rade;

namespace ttc {

// FreeDV RADE V1 transmit: mic speech (48 kHz) -> 16 kHz -> LPCNet
// features (Opus, behind ttc_rade_shim) -> rade_tx, 12 frames (120 ms) at
// a time -> 8 kHz real modem audio -> 48 kHz for the SignaLink. The end
// of an over is rade_tx_eoo carrying the callsign in freedv-gui's
// rade_text format (what every RADE receiver decodes), then 200 ms of
// silence before the transmitter may drop — freedv-backend's
// RADETransmitStep sequence (BSD-2, Mooneer Salem).
//
// Level: rade_tx keeps |sample| <= 1; out = 0.5 x that, ~-6 dBFS peaks
// (freedv-backend's x16383), and the operator sets drive with the
// SignaLink TX knob — ALC just moving, speech processor OFF.
class RadeTx {
public:
    explicit RadeTx(const std::string& callsign = "");
    ~RadeTx();
    RadeTx(const RadeTx&) = delete;
    RadeTx& operator=(const RadeTx&) = delete;
    bool ok() const { return r_ != nullptr; }

    void setCallsign(const std::string& c);
    // Mic in (48 kHz mono, -1..1); modem appended to out (48 kHz), in
    // 5760-sample (120 ms) bursts as frames complete.
    void process(const float* mic, int n, std::vector<float>& out);
    // End of over: the end-of-over frame with the callsign + 200 ms of
    // silence appended to out; the half-built frame is dropped.
    void endOver(std::vector<float>& out);

    static constexpr float kOutScale = 0.5f;
    // TX level trim, dB (<= 0): freedv-gui's "TX Attenuation".
    void setLevelDb(float db) { gain_ = std::pow(10.f, std::min(db, 0.f) / 20.f); }

private:
    void upsample(const float* x8, int n, std::vector<float>& out);

    struct rade* r_ = nullptr;
    void* enc_ = nullptr;
    void* text_ = nullptr;
    int nFeat_ = 0, nTx_ = 0, nEoo_ = 0, nEooBits_ = 0;
    std::vector<float> taps_, dHist_, uHist_;
    int dPhase_ = 0;
    float gain_ = 1.f;
    std::vector<short> pcm16_;                     // 16 kHz samples waiting
    std::vector<float> feats_;                     // features for the next frame
    std::vector<float> eooBits_;
};

} // namespace ttc
