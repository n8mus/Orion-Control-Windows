// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdr/RadeRx.h"

#include <algorithm>
#include <cmath>
#include <mutex>

extern "C" {
#include "rade_api.h"
#include "rade_text.h"
}
#include "ttc_rade_shim.h"

namespace ttc {

namespace {
constexpr int kTaps = 145;                         // 48 kHz FIR, odd = linear phase
std::once_flag gInit;

// Windowed-sinc lowpass at 48 kHz. 3.6 kHz serves both: it is under the
// 4 kHz Nyquist of the 8 kHz modem (RADE's carriers end at ~2.25 kHz) and,
// on the way up, keeps 16 kHz speech's images (above 8 kHz) out.
std::vector<float> lowpass(double fc) {
    std::vector<float> h(kTaps);
    const int m = kTaps / 2;
    double sum = 0;
    for (int i = 0; i < kTaps; ++i) {
        const int k = i - m;
        const double x = 2.0 * fc / 48000.0;
        const double s = k == 0 ? x : std::sin(M_PI * x * k) / (M_PI * k);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1))
                       + 0.08 * std::cos(4 * M_PI * i / (kTaps - 1));   // Blackman
        h[size_t(i)] = float(s * w);
        sum += s * w;
    }
    for (float& v : h) v = float(v / sum);
    return h;
}
}  // namespace

RadeRx::RadeRx() {
    std::call_once(gInit, [] { rade_initialize(); });
    char model[1] = {0};
    r_ = rade_open(model, RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
    if (!r_) return;
    nFeat_ = rade_n_features_in_out(r_);
    nEoo_ = rade_n_eoo_bits(r_);
    ninMax_ = rade_nin_max(r_);
    feat_.resize(size_t(nFeat_));
    eoo_.resize(size_t(std::max(nEoo_, 1)));
    fargan_ = ttc_fargan_new();
    text_ = rade_text_create();
    rade_text_set_rx_callback(text_, &RadeRx::onText, this);
    taps_ = lowpass(3600.0);
    dHist_.assign(kTaps, 0.f);
    uHist_.assign(kTaps / 3 + 2, 0.f);
}

RadeRx::~RadeRx() {
    if (text_) rade_text_destroy(text_);
    if (fargan_) ttc_fargan_free(fargan_);
    if (r_) rade_close(r_);
}

void RadeRx::onText(void*, const char* txt, int len, void* self) {
    auto* me = static_cast<RadeRx*>(self);
    std::string s(txt, size_t(std::max(len, 0)));
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    if (s.empty()) return;
    {
        std::lock_guard<std::mutex> lk(me->callMux_);
        me->call_ = s;
    }
    me->calls_.fetch_add(1);
}

std::string RadeRx::lastCallsign() const {
    std::lock_guard<std::mutex> lk(callMux_);
    return call_;
}

void RadeRx::reset() {
    std::fill(dHist_.begin(), dHist_.end(), 0.f);
    std::fill(uHist_.begin(), uHist_.end(), 0.f);
    dPhase_ = 0;
    in8_.clear();
    pend_.clear();
}

void RadeRx::process(const float* in, int n, std::vector<float>& out) {
    if (!r_) return;
    // 48 kHz -> 8 kHz: run the FIR only on every 6th output (polyphase in
    // effect), the history a ring of kTaps.
    for (int i = 0; i < n; ++i) {
        std::move(dHist_.begin() + 1, dHist_.end(), dHist_.begin());
        dHist_.back() = in[i];
        if (++dPhase_ < 6) continue;
        dPhase_ = 0;
        float acc = 0.f;
        for (int k = 0; k < kTaps; ++k) acc += taps_[size_t(k)] * dHist_[size_t(kTaps - 1 - k)];
        in8_.push_back(acc);
    }
    std::vector<RADE_COMP> rx(static_cast<size_t>(ninMax_));
    size_t used = 0;
    for (;;) {
        const int nin = rade_nin(r_);
        if (in8_.size() - used < size_t(nin)) break;
        for (int i = 0; i < nin; ++i) {
            rx[size_t(i)].real = in8_[used + size_t(i)];
            rx[size_t(i)].imag = 0.f;
        }
        used += size_t(nin);
        int hasEoo = 0;
        const int nout = rade_rx(r_, feat_.data(), &hasEoo, eoo_.data(), rx.data());
        if (hasEoo) {
            rade_text_rx(text_, eoo_.data(), nEoo_ / 2);
        } else {
            for (int i = 0; i < nout; ++i) {
                pend_.push_back(feat_[size_t(i)]);
                if (int(pend_.size()) < TTC_RADE_NFEAT) continue;
                float pcm[TTC_RADE_FRAME];
                ttc_fargan_synth(fargan_, pcm, pend_.data());
                pend_.clear();
                // 16 kHz -> 48 kHz: zero-stuff 1:3 through the same FIR
                // (gain 3 for the stuffed zeros), polyphase.
                for (int s = 0; s < TTC_RADE_FRAME; ++s) {
                    std::move(uHist_.begin() + 1, uHist_.end(), uHist_.begin());
                    uHist_.back() = pcm[s];
                    const int L = int(uHist_.size());
                    for (int ph = 0; ph < 3; ++ph) {
                        float acc = 0.f;
                        for (int j = 0; j < L; ++j) {
                            const int k = 3 * j + ph;
                            if (k >= kTaps) break;
                            acc += taps_[size_t(k)] * uHist_[size_t(L - 1 - j)];
                        }
                        out.push_back(3.f * acc);
                    }
                }
            }
        }
        const bool s = rade_sync(r_) != 0;
        sync_.store(s);
        if (s) {
            snr_.store(rade_snrdB_3k_est(r_));
            foff_.store(rade_freq_offset(r_));
        }
    }
    in8_.erase(in8_.begin(), in8_.begin() + long(used));
}

} // namespace ttc
