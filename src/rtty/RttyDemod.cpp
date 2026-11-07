// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/RttyDemod.h"
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

RttyDemod::RttyDemod() : RttyDemod(Params{}) {}
RttyDemod::RttyDemod(const Params& p) { setParams(p); }

void RttyDemod::setParams(const Params& p) {
    p_ = p;
    ita_.setUsos(p.usos);
    decim_ = std::max(1, int(std::lround(p.sampleRate / 1000.0)));
    const double rate1k = p.sampleRate / decim_;
    bitLen_ = rate1k / p.baud;
    const double mHz = p.reverse ? p.spaceHz : p.markHz;
    const double sHz = p.reverse ? p.markHz : p.spaceHz;
    rotM_ = std::polar(1.0, -2.0 * kPi * mHz / p.sampleRate);
    rotS_ = std::polar(1.0, -2.0 * kPi * sHz / p.sampleRate);
    const size_t L = std::max<size_t>(2, size_t(std::lround(bitLen_)));
    ringM_.assign(L, {0, 0});
    ringS_.assign(L, {0, 0});
    reset();
}

void RttyDemod::reset() {
    phM_ = phS_ = {1, 0};
    accM_ = accS_ = {0, 0};
    accN_ = 0;
    std::fill(ringM_.begin(), ringM_.end(), std::complex<double>{0, 0});
    std::fill(ringS_.begin(), ringS_.end(), std::complex<double>{0, 0});
    sumM_ = sumS_ = {0, 0};
    ringPos_ = 0;
    pkM_ = pkS_ = floor_ = 1e-3;
    quality_ = 0.0;
    warm_ = 0;
    uart_ = Uart::Idle;
    t_ = 0;
    dataBits_ = 0;
    code_ = 0;
    lastBit_ = 1;
    ita_.reset();
    st_ = {};
}

void RttyDemod::processReal(const float* s, size_t n) {
    // Small stack batches: no per-call allocation on the audio thread.
    std::complex<float> buf[256];
    while (n) {
        const size_t k = std::min<size_t>(n, 256);
        for (size_t i = 0; i < k; ++i) buf[i] = {s[i], 0.0f};
        process(buf, k);
        s += k;
        n -= k;
    }
}

void RttyDemod::process(const std::complex<float>* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const std::complex<double> x(s[i].real(), s[i].imag());
        accM_ += x * phM_;
        accS_ += x * phS_;
        phM_ *= rotM_;
        phS_ *= rotS_;
        if (++accN_ >= decim_) {
            // Re-normalise the phasors now and then (recurrence drift).
            phM_ /= std::abs(phM_);
            phS_ /= std::abs(phS_);
            // Boxcar over one bit at 1 kHz.
            sumM_ += accM_ - ringM_[ringPos_];
            sumS_ += accS_ - ringS_[ringPos_];
            ringM_[ringPos_] = accM_;
            ringS_[ringPos_] = accS_;
            if (++ringPos_ >= ringM_.size()) ringPos_ = 0;
            accM_ = accS_ = {0, 0};
            accN_ = 0;
            decimated(float(std::abs(sumM_)), float(std::abs(sumS_)));
        }
    }
}

// One decimated sample (1 kHz) of each tone's matched-filter magnitude.
void RttyDemod::decimated(float m, float s) {
    // Peak trackers: instant attack, ~1 s decay (an idle sender holds
    // mark for seconds; space shows in every start bit, so both stay
    // fresh while anyone keys).
    constexpr double kDecay = 1.0 - 1.0 / 1000.0;
    pkM_ = std::max(double(m), pkM_ * kDecay);
    pkS_ = std::max(double(s), pkS_ * kDecay);
    // A tone nobody has keyed for a while (an idle sender holds mark)
    // leaves its tracker on the OTHER tone's leakage, and normalising to
    // that reads the leakage as a full tone — the first start bit then
    // came out a bit and a half long. Neither peak may sit more than
    // 12 dB under the other; a real selective fade deeper than that is
    // lost copy anyway.
    const double hi = std::max(pkM_, pkS_);
    pkM_ = std::max(pkM_, hi * 0.25);
    pkS_ = std::max(pkS_, hi * 0.25);
    // Noise floor: the quieter tone, tracked with a slow rise and a
    // quick fall. With a signal present one tone is always off, so the
    // floor reads the noise in a bit-wide filter — the right ruler.
    const double lo = std::min(double(m), double(s));
    if (lo < floor_) floor_ += (lo - floor_) * 0.05;
    else             floor_ += (lo - floor_) * 0.0005;
    const double pk = std::max(pkM_, pkS_);
    const double snr = 20.0 * std::log10(std::max(pk, 1e-9) / std::max(floor_, 1e-9));
    // Keying contrast: with a signal one tone is on and the other off, so
    // |m-s|/(m+s) sits near 1 most of the time; two noise envelopes give
    // ~0.4 at any level. Averaged over ~1 s it is the squelch's ruler —
    // a level-free one, so a fade changes nothing but the SNR readout.
    const double c = std::abs(double(m) - double(s)) / (double(m) + double(s) + 1e-12);
    quality_ += (c - quality_) * 0.001;
    if (warm_ < 300) ++warm_;           // filters and trackers settling
    st_.mark = float(m / pkM_);
    st_.space = float(s / pkS_);
    st_.snrDb = float(snr);
    st_.quality = float(quality_);
    if (p_.squelch <= 0.0) st_.open = warm_ >= 300;
    else if (st_.open) st_.open = warm_ >= 300 && quality_ > p_.squelch - 0.05;
    else st_.open = warm_ >= 300 && quality_ > p_.squelch;
    // ATC: compare each tone against its own recent peak.
    const bool mark = (m / pkM_) >= (s / pkS_);
    bit(mark);
}

// UART at 1 kHz with a fractional bit length. Start = first space after
// mark; verified half a bit later; data sampled at the bit centres; the
// stop bit must read mark or the character is a framing error (dropped,
// and the next space edge starts over — that is the resync).
void RttyDemod::bit(bool mark) {
    const int b = mark ? 1 : 0;
    switch (uart_) {
        case Uart::Idle:
            if (lastBit_ == 1 && b == 0) { uart_ = Uart::Start; t_ = 0; }
            break;
        case Uart::Start:
            t_ += 1.0;
            if (t_ >= bitLen_ * 0.5) {
                if (b == 0) { uart_ = Uart::Data; dataBits_ = 0; code_ = 0; }
                else uart_ = Uart::Idle;           // a glitch, not a start
            }
            break;
        case Uart::Data:
            t_ += 1.0;
            if (dataBits_ < 5) {
                if (t_ >= bitLen_ * (1.5 + dataBits_)) {
                    code_ |= uint8_t(b << dataBits_);
                    ++dataBits_;
                }
            } else if (t_ >= bitLen_ * 6.5) {
                uart_ = Uart::Idle;
                if (b == 1) {
                    if (st_.open) {
                        const char c = ita_.feed(code_);
                        if (c && c != '\r') {
                            ++chars_;
                            if (onChar) onChar(c);
                        }
                    }
                } else {
                    ++frameErr_;
                }
            }
            break;
    }
    lastBit_ = b;
}

}  // namespace ttc
