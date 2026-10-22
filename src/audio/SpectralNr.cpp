// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/SpectralNr.h"
#include "dsp/Fft.h"

#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
Fft& fft512() {
    static Fft f(512);
    return f;
}
} // namespace

SpectralNr::SpectralNr() {
    win_.resize(kN);
    for (int i = 0; i < kN; ++i)
        win_[i] = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (kN - 1));
    reset();
}

void SpectralNr::reset() {
    inBuf_.assign(kN, 0.0f);
    outBuf_.assign(kN, 0.0f);
    inFill_ = kHop;                        // half-frame priming delay
    noise_.assign(kN / 2 + 1, 0.0f);
    fft_.assign(kN, {0.0f, 0.0f});
    primed_ = false;
    ready_.clear();
    readyPos_ = 0;
}

void SpectralNr::frame() {
    // Analysis
    for (int i = 0; i < kN; ++i)
        fft_[i] = {inBuf_[i] * win_[i], 0.0f};
    fft512().forward(fft_);
    // Noise floor per bin: fast drop, slow rise — a keyed CW tone leaves
    // its bin during every gap, so the floor learns the true noise while
    // the carrier bins keep their headroom. Gain is Wiener-ish with an
    // aggressive floor (12 dB max cut) so artifacts stay polite.
    for (int k = 0; k <= kN / 2; ++k) {
        const float mag = std::abs(fft_[k]);
        if (!primed_) noise_[k] = mag;
        else if (mag < noise_[k]) noise_[k] += 0.30f * (mag - noise_[k]);
        else                      noise_[k] += 0.002f * (mag - noise_[k]);
        const float nf = noise_[k] * 2.0f;             // oversubtraction
        float g = mag > nf ? (mag - nf) / mag : 0.0f;
        g = std::max(g, 0.25f);                        // -12 dB floor
        fft_[k] *= g;
        if (k > 0 && k < kN / 2) fft_[kN - k] *= g;    // mirror (real in)
    }
    primed_ = true;
    // Inverse via conjugation (the Fft class is forward-only):
    // ifft(X) = conj(fft(conj(X))) / N
    for (auto& c : fft_) c = std::conj(c);
    fft512().forward(fft_);
    // Overlap-add synthesis. Hann analysis+synthesis at 50% hop sums to
    // 0.75·N with the conjugation's /N folded in — verified unity gain on
    // a clean tone in nrtest before anything else was trusted.
    for (int i = 0; i < kN; ++i)
        outBuf_[i] += std::conj(fft_[i]).real() * win_[i] / (kN * 0.75f);
    for (int i = 0; i < kHop; ++i) ready_.push_back(outBuf_[i]);
    std::copy(outBuf_.begin() + kHop, outBuf_.end(), outBuf_.begin());
    std::fill(outBuf_.begin() + kHop, outBuf_.end(), 0.0f);
    std::copy(inBuf_.begin() + kHop, inBuf_.end(), inBuf_.begin());
    inFill_ = kHop;
}

void SpectralNr::process(float* x, int n) {
    for (int i = 0; i < n; ++i) {
        inBuf_[inFill_ >= kN ? kN - 1 : inFill_] = x[i];
        if (++inFill_ >= kN) frame();
        // hand back one processed sample per input sample (latency kN)
        x[i] = readyPos_ < ready_.size() ? ready_[readyPos_++] : 0.0f;
    }
    // keep the FIFO from growing without bound
    if (readyPos_ > 4096) {
        ready_.erase(ready_.begin(), ready_.begin() + readyPos_);
        readyPos_ = 0;
    }
}

} // namespace ttc
