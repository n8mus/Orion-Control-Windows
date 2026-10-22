// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct rade;

namespace ttc {

// FreeDV RADE V1 receive on the SDR's demodulated USB audio: 48 kHz in ->
// 8 kHz modem (FIR, 6:1) -> rade_rx (rade_c, BSD-2: Peter Marks, David
// Rowe) -> 36-feature frames -> FARGAN vocoder (Opus) -> 16 kHz speech ->
// 48 kHz (FIR, 1:3). The end-of-over frame carries the sender's callsign
// (freedv-gui's rade_text codec). The call sequence is freedv-backend's
// RADEReceiveStep (BSD-2, Mooneer Salem). One instance per process —
// rade_c keeps one context, rade_text a static scratch buffer.
class RadeRx {
public:
    RadeRx();
    ~RadeRx();
    RadeRx(const RadeRx&) = delete;
    RadeRx& operator=(const RadeRx&) = delete;
    bool ok() const { return r_ != nullptr; }

    // Modem audio in (48 kHz mono); decoded speech appended to out (48 kHz).
    // Nothing comes out while unsynced: freedv-gui's behaviour (the modem's
    // hiss is not speech).
    void process(const float* in, int n, std::vector<float>& out);
    void reset();                                  // drop buffered state

    bool sync() const { return sync_.load(); }
    float snrDb() const { return snr_.load(); }    // last value while in sync
    float freqOffsetHz() const { return foff_.load(); }
    // Last end-of-over callsign and how many have arrived (a new one = the
    // count moved).
    std::string lastCallsign() const;
    int callsignCount() const { return calls_.load(); }

private:
    static void onText(void*, const char* txt, int len, void* self);

    struct rade* r_ = nullptr;
    void* fargan_ = nullptr;
    void* text_ = nullptr;
    int nFeat_ = 0, nEoo_ = 0, ninMax_ = 0;
    std::vector<float> taps_;                      // 48 kHz lowpass, both ways
    std::vector<float> dHist_;                     // decimator history
    int dPhase_ = 0;
    std::vector<float> in8_;                       // modem samples waiting
    std::vector<float> feat_, eoo_, pend_;
    std::vector<float> uHist_;                     // interpolator history (16 kHz)
    std::atomic<bool> sync_{false};
    std::atomic<float> snr_{0.f}, foff_{0.f};
    mutable std::mutex callMux_;
    std::string call_;
    std::atomic<int> calls_{0};
};

} // namespace ttc
