// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>


namespace ttc {

// Panadapter trace shaping, ported from AetherSDR (GPL-3,
// src/gui/FftTraceSmoothing.h, src/gui/FftTraceDecomb.h,
// src/core/dsp/SpectralEnvelope.*): display-only, applied to the drawn
// per-column levels, never to the waterfall or anything that decodes.

// FFT Smooth: each point becomes the mean of its ±halfWidth neighbours in
// LINEAR power, then back to dB, so the noise does not drag the trace down
// the way a dB mean would. Window shrinks at the edges; 0 = unchanged.
// Summed directly, not as a prefix sum (a 150 dB trace is beyond double
// precision for P[hi] - P[lo] next to a strong carrier).
void smoothTraceLinearPower(std::vector<float>& db, int halfWidth,
                            std::vector<double>& scratch);

// Cepstral "true envelope" of a dB trace (Röbel & Rodet, DAFx 2005): DCT,
// keep coefficients coarser than cutoffPoints (raised-cosine taper), inverse,
// iterated A = V + 3·(max(X, V) - V) until V rides the peaks. AetherSDR uses
// FFTW's DCT; here the DCT is the FFT of the mirrored trace (same
// coefficients) on the console's own radix-2 FFT, so the GUI thread never
// waits on FFTW's process-wide planner while WDSP plans.
class SpectralEnvelope {
public:
    bool compute(const std::vector<float>& in, std::vector<float>& out, double cutoffPoints);

private:
    // Split-complex radix-2 FFT with a twiddle table: runs every frame, 24
    // transforms deep, so it skips std::complex's NaN-checked multiply that
    // the shared dsp/Fft (decoders) keeps.
    void fft(std::vector<float>& re, std::vector<float>& im) const;
    int n2_ = 0;
    std::vector<int> rev_;
    std::vector<float> twRe_, twIm_, re_, im_;
    std::vector<double> x_, a_, v_;
    std::vector<float> g_;
};

// FFT De-comb: removes the comb a zoomed-in panadapter draws across an SSB
// voice (pitch harmonics 100-250 Hz apart) by blending in the true envelope
// where it rises clear of the noise. The input is clipped from below at the
// 20th percentile + 12 dB so the envelope cannot ride noise peaks and lift
// the floor; over noise the trace is left as it was. windowPoints = the
// widest harmonic spacing to remove, in trace points (< 2 = off).
class Decomb {
public:
    bool apply(std::vector<float>& db, double windowPoints);

private:
    SpectralEnvelope env_;
    std::vector<float> sorted_, clipped_, envelope_;
};

} // namespace ttc
