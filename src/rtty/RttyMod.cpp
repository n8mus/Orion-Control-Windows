// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/RttyMod.h"
#include <cmath>

namespace ttc {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

RttyMod::RttyMod() : RttyMod(Params{}) {}
RttyMod::RttyMod(const Params& p) { setParams(p); }

void RttyMod::setParams(const Params& p) { p_ = p; }

void RttyMod::queue(const std::string& text) {
    // Encode with the case the sender is already in, so "73" after
    // "TU 73" doesn't key FIGS twice... encodeText starts fresh; feed it
    // our case by prefixing nothing and tracking shifts as they pass.
    for (uint8_t c : ita2::encodeText(text, p_.usos)) {
        if (c == ita2::kLtrs) { if (!figs_ && !codes_.empty()) continue; figs_ = false; }
        else if (c == ita2::kFigs) { if (figs_) continue; figs_ = true; }
        else if (c == ita2::kSpace && p_.usos) figs_ = false;
        codes_.push_back(c);
    }
}

void RttyMod::clear() { codes_.clear(); }

void RttyMod::loadNext() {
    uint8_t code;
    if (!codes_.empty()) {
        code = codes_.front();
        codes_.pop_front();
    } else if (p_.diddle) {
        code = ita2::kLtrs;
        figs_ = false;
    } else {
        return;                         // idle on mark
    }
    const double bitSamples = p_.sampleRate / p_.baud;
    bits_.emplace_back(false, bitSamples);                    // start
    for (int i = 0; i < 5; ++i)
        bits_.emplace_back((code >> i) & 1, bitSamples);      // data, LSB first
    bits_.emplace_back(true, bitSamples * p_.stopBits);       // stop
}

void RttyMod::render(float* out, size_t n) {
    const double mHz = p_.reverse ? p_.spaceHz : p_.markHz;
    const double sHz = p_.reverse ? p_.markHz : p_.spaceHz;
    for (size_t i = 0; i < n; ++i) {
        if (bitLeft_ <= 0.0) {
            if (bits_.empty()) loadNext();
            if (!bits_.empty()) {
                curMark_ = bits_.front().first;
                bitLeft_ += bits_.front().second;
                bits_.pop_front();
            } else {
                curMark_ = true;        // idle mark, one sample at a time
                bitLeft_ = 1.0;
            }
        }
        const double hz = curMark_ ? mHz : sHz;
        phase_ += 2.0 * kPi * hz / p_.sampleRate;
        if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        out[i] = float(p_.level * std::sin(phase_));
        bitLeft_ -= 1.0;
    }
}

void RttyMod::renderMark(float* out, size_t n) {
    const double mHz = p_.reverse ? p_.spaceHz : p_.markHz;
    for (size_t i = 0; i < n; ++i) {
        phase_ += 2.0 * kPi * mHz / p_.sampleRate;
        if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        out[i] = float(p_.level * std::sin(phase_));
    }
}

}  // namespace ttc
