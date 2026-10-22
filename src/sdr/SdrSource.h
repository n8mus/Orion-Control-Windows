// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <complex>
#include <functional>
#include <vector>

namespace ttc {

using IqBlock = std::vector<std::complex<float>>;

// Abstraction over the wideband RX front-end (SDRplay RSP2 now, SoapySDR later).
// The panadapter/DSP consume IQ via the callback; nothing above this layer knows
// which SDR is attached.
class SdrSource {
public:
    virtual ~SdrSource() = default;

    virtual bool start(double centerHz, double sampleRate) = 0;
    virtual void stop() = 0;
    virtual void setCenterFrequency(double hz) = 0;

    // Called from the SDR streaming thread with each IQ block.
    virtual void setIqCallback(std::function<void(const IqBlock&)> cb) = 0;
};

} // namespace ttc
