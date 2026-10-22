// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include "sdr/SdrSource.h"      // IqBlock
#include "dsp/Fft.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace ttc {

// Turns a stream of IQ blocks into a display spectrum: Hann window -> FFT ->
// power(dB) -> fftshift (DC centered) -> temporal smoothing. Throttled to a target
// frame rate so we FFT ~20x/sec, not per callback. Called on the SDR thread; the
// output callback fires there too (marshal to the GUI thread in the consumer).
class SpectrumComputer {
public:
    explicit SpectrumComputer(int fftSize = 2048);

    void setOutput(std::function<void(const std::vector<float>&)> cb) { out_ = std::move(cb); }
    void setFrameIntervalMs(double ms) { minInterval_ = ms; }
    // Display frame rate: every block is FFT'd regardless (Welch); this sets
    // how often an averaged frame goes out. The temporal smoothing is 0.5 per
    // 50 ms (the 20 fps feel at any rate).
    void setFrameRateHz(int fps) {
        fps_ = fps < 5 ? 5 : fps > 60 ? 60 : fps;
        // Emits ride block arrivals (one FFT block = 16.4 ms at 1 MS/s): 0.8
        // lets 60 fps take every block instead of every other one.
        minInterval_ = 1000.0 / fps_ * 0.8;
        blocksPerFrame_ = std::max(1, int(std::lround(sampleRate_ / n_ / fps_)));
    }
    // Sample-clocked emission (the live console): a frame every N blocks of
    // SAMPLES, not of wall time. The IQ reaches this in ~43 ms USB bursts, so
    // wall-time gating gave one frame per burst — 23 fps however high the
    // setting (measured 2026-10-16). The GUI paces the frames back out
    // evenly (framePeriodMs). Off = the old wall-clock gate (skimreplay).
    void setSampleClocked(bool on) { sampleClocked_ = on; }
    void setSampleRateHz(double hz) { sampleRate_ = hz; setFrameRateHz(fps_); }
    double framePeriodMs() const { return blocksPerFrame_ * n_ * 1000.0 / sampleRate_; }
    int  bins() const { return n_; }

    void addSamples(const IqBlock& iq);

private:
    static double now_ms();

    int n_;
    Fft fft_;
    std::vector<float> window_;                 // Hann
    std::vector<std::complex<float>> buf_;      // pending samples (consumed in blocks)
    std::vector<std::complex<float>> frame_;
    std::vector<float> psAcc_;                  // Welch power accumulator
    int    psCount_     = 0;                    // spectra accumulated this frame
    std::vector<float> outDb_;                  // smoothed output, fftshifted
    std::function<void(const std::vector<float>&)> out_;
    double minInterval_ = 50.0;                 // ~20 fps
    double lastEmitMs_  = 0.0;
    bool   sampleClocked_ = false;
    double sampleRate_  = 1e6;
    int    fps_         = 20;
    int    blocksPerFrame_ = 3;
    void emitFrame(double dtMs);
};

} // namespace ttc
