// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdr/RadeTx.h"

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
constexpr int kTaps = 145;
std::once_flag gInit;

// 48 kHz windowed-sinc lowpass (Blackman) — for both 48->16 (speech, cut
// 7.2 kHz) and 8->48 (modem, cut 3.6 kHz).
std::vector<float> lowpass(double fc) {
    std::vector<float> h(kTaps);
    const int m = kTaps / 2;
    double sum = 0;
    for (int i = 0; i < kTaps; ++i) {
        const int k = i - m;
        const double x = 2.0 * fc / 48000.0;
        const double s = k == 0 ? x : std::sin(M_PI * x * k) / (M_PI * k);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1))
                       + 0.08 * std::cos(4 * M_PI * i / (kTaps - 1));
        h[size_t(i)] = float(s * w);
        sum += s * w;
    }
    for (float& v : h) v = float(v / sum);
    return h;
}
const std::vector<float>& speechTaps() { static const auto t = lowpass(7200.0); return t; }
const std::vector<float>& modemTaps() { static const auto t = lowpass(3600.0); return t; }
}  // namespace

RadeTx::RadeTx(const std::string& callsign) {
    std::call_once(gInit, [] { rade_initialize(); });
    char model[1] = {0};
    r_ = rade_open(model, RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
    if (!r_) return;
    nFeat_ = rade_n_features_in_out(r_);
    nTx_ = rade_n_tx_out(r_);
    nEoo_ = rade_n_tx_eoo_out(r_);
    nEooBits_ = rade_n_eoo_bits(r_);
    enc_ = ttc_lpcnet_enc_new();
    text_ = rade_text_create();
    dHist_.assign(kTaps, 0.f);
    uHist_.assign(kTaps / 6 + 2, 0.f);
    setCallsign(callsign);
}

RadeTx::~RadeTx() {
    if (text_) rade_text_destroy(text_);
    if (enc_) ttc_lpcnet_enc_free(enc_);
    if (r_) rade_close(r_);
}

void RadeTx::setCallsign(const std::string& c) {
    if (!r_ || nEooBits_ <= 0) return;
    eooBits_.assign(size_t(nEooBits_), 0.f);
    rade_text_generate_tx_string(text_, c.c_str(), int(c.size()), eooBits_.data(), nEooBits_);
    rade_tx_set_eoo_bits(r_, eooBits_.data());
}

void RadeTx::upsample(const float* x8, int n, std::vector<float>& out) {
    // 8 -> 48 kHz: zero-stuff 1:6 through the 3.6 kHz FIR (gain 6),
    // polyphase.
    const auto& h = modemTaps();
    const int L = int(uHist_.size());
    for (int s = 0; s < n; ++s) {
        std::move(uHist_.begin() + 1, uHist_.end(), uHist_.begin());
        uHist_.back() = x8[s];
        for (int ph = 0; ph < 6; ++ph) {
            float acc = 0.f;
            for (int j = 0; j < L; ++j) {
                const int k = 6 * j + ph;
                if (k >= kTaps) break;
                acc += h[size_t(k)] * uHist_[size_t(L - 1 - j)];
            }
            out.push_back(6.f * kOutScale * gain_ * acc);
        }
    }
}

void RadeTx::process(const float* mic, int n, std::vector<float>& out) {
    if (!r_) return;
    const auto& h = speechTaps();
    for (int i = 0; i < n; ++i) {
        std::move(dHist_.begin() + 1, dHist_.end(), dHist_.begin());
        dHist_.back() = mic[i];
        if (++dPhase_ < 3) continue;
        dPhase_ = 0;
        float acc = 0.f;
        for (int k = 0; k < kTaps; ++k) acc += h[size_t(k)] * dHist_[size_t(kTaps - 1 - k)];
        pcm16_.push_back(short(std::clamp(int(std::lround(acc * 32767.f)), -32767, 32767)));
        if (int(pcm16_.size()) < TTC_RADE_FRAME) continue;
        float f[TTC_RADE_NFEAT];
        ttc_lpcnet_features(enc_, pcm16_.data(), f);
        pcm16_.clear();
        feats_.insert(feats_.end(), f, f + TTC_RADE_NFEAT);
        if (int(feats_.size()) < nFeat_) continue;
        std::vector<RADE_COMP> tx(static_cast<size_t>(nTx_));
        rade_tx(r_, tx.data(), feats_.data());
        feats_.clear();
        std::vector<float> re(tx.size());
        for (size_t k = 0; k < tx.size(); ++k) re[k] = tx[k].real;
        upsample(re.data(), int(re.size()), out);
    }
}

void RadeTx::endOver(std::vector<float>& out) {
    if (!r_) return;
    feats_.clear();
    pcm16_.clear();
    std::vector<RADE_COMP> eoo(static_cast<size_t>(nEoo_));
    rade_tx_eoo(r_, eoo.data());
    std::vector<float> re(eoo.size());
    for (size_t k = 0; k < eoo.size(); ++k) re[k] = eoo[k].real;
    upsample(re.data(), int(re.size()), out);
    out.insert(out.end(), 48000 / 5, 0.f);         // 200 ms before PTT drops
}

} // namespace ttc
