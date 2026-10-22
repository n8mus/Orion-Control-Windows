// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <complex>
#include <vector>

namespace ttc {

// Spectral-gate noise reduction for radio audio (48 kHz mono). Classic
// spectral subtraction: 512-sample Hann frames, 50% overlap-add, per-bin
// noise floor learned as a slow minimum, gain = how far the bin stands
// above its floor. Tuned for TONES (CW) rather than speech — the speech
// world's RNNoise treats a steady carrier as suspicious; this does the
// opposite (idea from AetherSDR's NR suite; implementation our own).
class SpectralNr {
public:
    SpectralNr();

    // In-place on 48 kHz mono float samples (any count; internally
    // buffered into frames — adds 512 samples (~11 ms) of latency).
    // Output count == input count.
    void process(float* x, int n);
    void reset();

private:
    void frame();                          // one 512-pt analysis/synthesis

    static constexpr int kN = 512;         // ~94 Hz bins at 48 kHz
    static constexpr int kHop = kN / 2;
    std::vector<float> win_;
    std::vector<float> inBuf_, outBuf_;    // streaming overlap buffers
    int inFill_ = 0;
    std::vector<float> noise_;             // per-bin noise-floor estimate
    std::vector<std::complex<float>> fft_;
    bool primed_ = false;
    // FIFO of processed samples ready to hand back
    std::vector<float> ready_;
    size_t readyPos_ = 0;
};

} // namespace ttc
