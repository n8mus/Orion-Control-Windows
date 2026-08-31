// SPDX-License-Identifier: GPL-2.0-or-later
#include "dsp/SpectrumComputer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ttc {

SpectrumComputer::SpectrumComputer(int fftSize)
    : n_(fftSize), fft_(fftSize), window_(fftSize), frame_(fftSize),
      outDb_(fftSize, -120.0f) {
    for (int i = 0; i < n_; ++i)                 // Hann window
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979f * i / (n_ - 1)));
    buf_.reserve(n_ * 2);
}

double SpectrumComputer::now_ms() {
    // steady_clock = CLOCK_MONOTONIC on POSIX, QPC on Windows — same
    // monotonic-ms contract, no platform split.
    using namespace std::chrono;
    return duration<double, std::milli>(
               steady_clock::now().time_since_epoch()).count();
}

void SpectrumComputer::addSamples(const IqBlock& iq) {
    buf_.insert(buf_.end(), iq.begin(), iq.end());

    // Welch averaging: FFT every full block as it arrives (nothing is thrown
    // away) and accumulate POWER spectra. Averaging in the power domain cuts
    // the chi-square noise variance ~1/sqrt(k) without biasing peaks down, so
    // the floor comes out flat instead of a field of one-FFT spikes.
    if (psAcc_.size() != static_cast<size_t>(n_)) psAcc_.assign(n_, 0.0f);
    const float norm = 1.0f / static_cast<float>(n_);
    while (static_cast<int>(buf_.size()) >= n_) {
        // Subtract the block's complex mean before windowing: this removes
        // the residual zero-IF DC line at the source, so its Hann leakage
        // skirt (which no amount of center-bin patching covers) never forms.
        std::complex<float> mean(0.0f, 0.0f);
        for (int i = 0; i < n_; ++i) mean += buf_[i];
        mean *= 1.0f / static_cast<float>(n_);
        for (int i = 0; i < n_; ++i) frame_[i] = (buf_[i] - mean) * window_[i];
        buf_.erase(buf_.begin(), buf_.begin() + n_);
        fft_.forward(frame_);
        for (int i = 0; i < n_; ++i) {
            const std::complex<float> c = frame_[i];
            psAcc_[i] += (c.real() * c.real() + c.imag() * c.imag()) * norm * norm;
        }
        ++psCount_;
    }

    const double t = now_ms();
    if (psCount_ == 0 || t - lastEmitMs_ < minInterval_) return;
    lastEmitMs_ = t;

    const float inv = 1.0f / static_cast<float>(psCount_);
    for (int i = 0; i < n_; ++i) {
        const int k = (i + n_ / 2) % n_;          // fftshift: DC to center
        const float db = 10.0f * std::log10(psAcc_[k] * inv + 1e-12f);
        outDb_[i] = smooth_ * outDb_[i] + (1.0f - smooth_) * db;  // temporal smoothing
    }
    // Zero-IF DC removal: the RSP2's residual LO leakage survives the API's
    // DC correction as a narrow spike parked on the display center (= the
    // dial). It lives in a handful of known bins, so bridge straight across
    // them from the neighbors — display-only data, honest enough.
    {
        const int c = n_ / 2, half = 3;           // +/-3 bins (~183 Hz at 61 Hz/bin)
        const float a = outDb_[c - half - 1], b = outDb_[c + half + 1];
        for (int i = -half; i <= half; ++i)
            outDb_[c + i] = a + (b - a) * (i + half + 1) / float(2 * half + 2);
    }
    std::fill(psAcc_.begin(), psAcc_.end(), 0.0f);
    psCount_ = 0;
    if (out_) out_(outDb_);
}

} // namespace ttc
