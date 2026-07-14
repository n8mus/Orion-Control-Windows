// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/CwDecoder.h"

#include <QHash>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ttc {

namespace {
// ITU Morse, plus the prosigns that matter on the air.
const QHash<QString, QChar>& morseTable() {
    static const QHash<QString, QChar> t = {
        {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'},
        {"..-.", 'F'}, {"--.", 'G'}, {"....", 'H'}, {"..", 'I'}, {".---", 'J'},
        {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'}, {"---", 'O'},
        {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'},
        {"..-", 'U'}, {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'},
        {"--..", 'Z'},
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
        {"---..", '8'}, {"----.", '9'},
        {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'},
        {"-...-", '='}, {".-.-.", '+'}, {"-....-", '-'}, {".--.-.", '@'},
        {"---...", ':'}, {"-.-.-.", ';'}, {".-..-.", '"'}, {"..--.-", '_'},
        {"-.--.", '('}, {"-.--.-", ')'}, {"...-..-", '$'}, {".-...", '&'},
    };
    return t;
}
} // namespace

CwDecoder::CwDecoder(double inputRate, double offsetHz, QObject* parent)
    : QObject(parent), inputRate_(inputRate) {
    decim_ = static_cast<int>(inputRate / 2000.0);   // -> 2 ksps envelope
    tickMs_ = 1000.0 * decim_ / inputRate;
    retune(offsetHz);
}

void CwDecoder::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
}

void CwDecoder::retune(double offsetHz) {
    pendingOffset_.store(offsetHz, std::memory_order_relaxed);
    retunePending_.store(true, std::memory_order_release);
}

void CwDecoder::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (retunePending_.exchange(false, std::memory_order_acquire)) {
        // Mix the carrier at offsetHz (negative = below the LO) up to DC,
        // and forget everything learned about the previous station.
        const double inc =
            -2.0 * M_PI * pendingOffset_.load(std::memory_order_relaxed)
            / inputRate_;
        loStep_ = {std::cos(inc), std::sin(inc)};
        lo_ = {1.0, 0.0};
        acc_ = {0.0, 0.0};
        accN_ = 0;
        for (auto& v : ma_) v = {};
        maLen_ = 8;
        afcPrevZ_ = {0.0f, 0.0f};
        afcErrAvg_ = 0.0;
        afcTicks_ = 0;
        afcTotal_.store(0.0, std::memory_order_relaxed);
        env_ = 0.0f;
        peak_ = 0.0f;
        floor_ = 1e-3f;
        key_ = false;
        runMs_ = 0.0;
        ditMs_ = 60.0;
        gapMin_ = 60.0;
        lastSpaceMs_ = 0.0;
        sym_.clear();
        wordGapSent_ = true;
        lastWpm_ = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        acc_ += std::complex<double>(d[i]) * lo_;
        lo_ *= loStep_;
        if (++renorm_ >= 4096) {           // keep the phasor on the unit circle
            renorm_ = 0;
            lo_ /= std::abs(lo_);
        }
        if (++accN_ < decim_) continue;
        // one decimated complex sample
        std::complex<float> z(float(acc_.real() / decim_),
                              float(acc_.imag() / decim_));
        acc_ = {0.0, 0.0};
        accN_ = 0;
        ma_[maIdx_] = z;
        maIdx_ = (maIdx_ + 1) & 7;
        maLen_ = gapMin_ < 35.0 ? 4 : 8;   // wide open for 40+ WPM
        std::complex<float> s{0.0f, 0.0f};
        for (int k = 0; k < maLen_; ++k)
            s += ma_[(maIdx_ - 1 - k) & 7];
        s /= float(maLen_);
        // AFC: while the key is down the filtered sample is a carrier;
        // successive-sample phase advance IS the residual frequency error.
        if (key_) {
            const std::complex<float> d = s * std::conj(afcPrevZ_);
            if (std::abs(d) > 1e-9f) {
                const double errHz =
                    std::arg(d) * (inputRate_ / decim_) / (2.0 * M_PI);
                if (std::abs(errHz) < 150.0)       // in-band only
                    afcErrAvg_ += 0.05 * (errHz - afcErrAvg_);
            }
            if (++afcTicks_ >= 200) {              // nudge every ~100 ms
                afcTicks_ = 0;
                double total =
                    afcTotal_.load(std::memory_order_relaxed);
                double step = std::clamp(afcErrAvg_, -20.0, 20.0);
                step = std::clamp(step, -80.0 - total, 80.0 - total);
                if (std::abs(step) > 0.5) {
                    const double inc = -2.0 * M_PI * step / inputRate_;
                    loStep_ *= std::complex<double>(std::cos(inc),
                                                    std::sin(inc));
                    afcTotal_.store(total + step,
                                    std::memory_order_relaxed);
                    afcErrAvg_ = 0.0;
                }
            }
        }
        afcPrevZ_ = s;
        tick(std::abs(s));
    }
}

void CwDecoder::tick(float mag) {
    // Envelope smoothing: ~4 ms at 20 WPM, proportionally faster at high
    // speed so 27 ms elements keep crisp edges. Driven by the gap
    // bootstrap, not the dit clock — the clock is exactly what's stuck
    // when a fast station starts up.
    const float envA =
        std::clamp(0.12f * float(60.0 / gapMin_), 0.12f, 0.45f);
    env_ += envA * (mag - env_);
    if (std::getenv("TTC_CWENV")) {        // debug: envelope/slicer state dump
        static int k = 0;
        if (++k % 100 == 0)
            std::fprintf(stderr, "[env] mag %.5f env %.5f peak %.5f floor %.5f key %d\n",
                         mag, env_, peak_, floor_, int(key_));
    }
    // Track key-down level and noise floor — each learns ONLY during its
    // own state. Letting the floor rise during a long dah collapses the
    // SNR gate mid-element and chops it (found via envelope dump: the
    // floor climbed to half the signal within 300 ms of key-down).
    if (key_) {
        if (env_ > peak_) peak_ += 0.20f * (env_ - peak_);
        else              peak_ += 0.01f * (env_ - peak_);
    } else {
        if (env_ < floor_) floor_ += 0.20f * (env_ - floor_);
        else               floor_ += 0.01f * (env_ - floor_);
        // A plausible signal onset must charge the peak FAST or the very
        // first element gets bitten off waiting for the SNR gate (C decoded
        // as R, every time). Plausible = well clear of the noise floor —
        // otherwise ordinary noise wiggle pumps the peak up and the gate
        // opens on a dead frequency (live test: an empty spot babbled E's).
        if (env_ > peak_ && env_ > 3.0f * floor_)
            peak_ += 0.20f * (env_ - peak_);
        else
            peak_ += 0.001f * (env_ - peak_);  // QSB: stale peaks let go
    }
    if (floor_ < 1e-6f) floor_ = 1e-6f;

    bool key = key_;
    if (peak_ > 3.0f * floor_) {           // usable SNR only
        const float span = peak_ - floor_;
        const float on  = floor_ + 0.55f * span;
        const float off = floor_ + 0.35f * span;
        if (!key_ && env_ > on)  key = true;
        if (key_  && env_ < off) key = false;
    } else {
        key = false;
    }
    // QSB return: a signal well clear of the noise floor keys on even if
    // the peak tracker is still holding the pre-fade level (the relative
    // threshold would wait out the whole recovery otherwise). Only when
    // the envelope stays BELOW that stale peak though — an excursion past
    // it is a static crash, not a returning station, and keying on QRN
    // impulses is exactly the babble v1 was cured of (live-found: every
    // hunting skimmer channel "decoding" 30-55 WPM noise).
    if (!key_ && env_ > 8.0f * floor_ && env_ < 1.1f * peak_
        && peak_ > 8.0f * floor_)
        key = true;

    if (key == key_) {
        runMs_ += tickMs_;
        // Long silence closes out the pending character and word.
        if (!key_ && !sym_.isEmpty() && runMs_ > 2.5 * ditMs_) emitSymbol();
        if (!key_ && !wordGapSent_ && runMs_ > 6.0 * ditMs_) {
            wordGapSent_ = true;
            emit textDecoded(" ");
        }
        return;
    }
    // Edge: classify the run that just ended.
    const double ms = runMs_;
    runMs_ = 0.0;
    key_ = key;
    if (key) {
        // A space ended. Short spaces are element gaps = exactly one dit —
        // they carry the sender's dit clock even when the marks are being
        // misclassified, which is what breaks the 35 WPM acquisition trap
        // (dahs captured as dits keep the estimate high; the 34 ms gaps
        // pull it right back down).
        if (ms > 0.4 * ditMs_ && ms < 2.0 * ditMs_)
            ditMs_ += 0.35 * (ms - ditMs_);
        // Gap bootstrap: snap down to any credible shorter gap, relax
        // slowly upward so a speed drop is followed too. Learns ONLY in
        // the presence of a real signal — noise chatter making the gaps
        // look short would switch in the wide fast filters and admit
        // even more noise (live-found feedback loop on empty channels).
        if (ms > 8.0 && peak_ > 6.0f * floor_)
            gapMin_ = std::min(ms, gapMin_ + 0.03 * (120.0 - gapMin_));
        // Merged-mark pollution escape: if the dit clock sits way above
        // the observed element gap, the marks it learned from were
        // smeared pairs — resync to the sender's real clock.
        if (ditMs_ > 2.2 * gapMin_) ditMs_ = gapMin_;
        lastSpaceMs_ = ms;
        return;
    }
    // A mark ended: dit or dah, and adapt the dit clock.
    if (ms < 0.35 * ditMs_) {
        // Impulse blip, not an element — and it must not SPLIT the gap it
        // landed in, or characters never close in noise: resume the space
        // as if the blip never happened.
        runMs_ = lastSpaceMs_ + ms;
        return;
    }
    if (ms < 2.0 * ditMs_) {
        sym_ += '.';
        if (ms > 0.4 * ditMs_) {           // too short to trust for timing
            ditMs_ += 0.35 * (ms - ditMs_);
            const int wpm = int(std::lround(1200.0 / ditMs_));
            if (wpm != lastWpm_ && wpm >= 5 && wpm <= 60) {
                lastWpm_ = wpm;
                emit wpmEstimated(wpm);
            }
        }
    } else {
        sym_ += '-';
        ditMs_ += 0.35 * (ms / 3.0 - ditMs_);
    }
    if (sym_.size() > 8) sym_.clear();     // garbage guard
}

void CwDecoder::emitSymbol() {
    const auto it = morseTable().constFind(sym_);
    sym_.clear();
    wordGapSent_ = false;
    if (it != morseTable().constEnd())
        emit textDecoded(QString(it.value()));
    else
        emit textDecoded("*");             // unreadable character
}

} // namespace ttc
