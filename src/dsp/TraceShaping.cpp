// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AetherSDR (GPL-3): FftTraceSmoothing.h, FftTraceDecomb.h,
// core/dsp/SpectralEnvelope.cpp. Constants are theirs.
#include "dsp/TraceShaping.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ttc {

void smoothTraceLinearPower(std::vector<float>& db, int halfWidth,
                            std::vector<double>& lin) {
    const int n = static_cast<int>(db.size());
    if (halfWidth <= 0 || n < 2) return;
    constexpr double kLn10Over10 = 0.23025850929940457;
    lin.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        lin[size_t(i)] = std::isfinite(db[size_t(i)]) ? std::exp(double(db[size_t(i)]) * kLn10Over10) : 0.0;
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - halfWidth), hi = std::min(n - 1, i + halfWidth);
        double sum = 0.0;
        for (int j = lo; j <= hi; ++j) sum += lin[size_t(j)];
        db[size_t(i)] = float(10.0 * std::log10(std::max(sum / double(hi - lo + 1), 1e-30)));
    }
}

namespace {
// Converged when the envelope is within this of every input point; a steep
// filter skirt is finer than the lifter can follow, so the cap usually ends it.
constexpr double kConvergedDb = 1.0;
constexpr int kMaxIterations = 12;
// Over-relaxation: plain true envelope (1) sits ~4 dB under an 18 dB voice
// comb after 6 passes; 3 is within ~1-1.5 dB in 12.
constexpr double kRelaxation = 3.0;
constexpr double kTaperStart = 0.7;            // lifter taper, fraction of cutoff
}  // namespace

void SpectralEnvelope::fft(std::vector<float>& re, std::vector<float>& im) const {
    const int n = n2_;
    for (int i = 0; i < n; ++i)
        if (i < rev_[size_t(i)]) {
            std::swap(re[size_t(i)], re[size_t(rev_[size_t(i)])]);
            std::swap(im[size_t(i)], im[size_t(rev_[size_t(i)])]);
        }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; ++j) {
                const float wr = twRe_[size_t(j * step)], wi = twIm_[size_t(j * step)];
                const size_t a = size_t(i + j), b = a + size_t(half);
                const float vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
            }
    }
}

bool SpectralEnvelope::compute(const std::vector<float>& in, std::vector<float>& out,
                               double cutoffPoints) {
    const int n = static_cast<int>(in.size());
    if (n < 4 || !(cutoffPoints >= 2.0)) return false;
    int m = 4;
    while (m < n) m <<= 1;
    const int n2 = 2 * m;
    if (n2_ != n2) {
        n2_ = n2;
        int logn = 0;
        while ((1 << logn) < n2) ++logn;
        rev_.resize(size_t(n2));
        for (int i = 0; i < n2; ++i) {
            int r = 0;
            for (int j = 0; j < logn; ++j) if (i & (1 << j)) r |= 1 << (logn - 1 - j);
            rev_[size_t(i)] = r;
        }
        twRe_.resize(size_t(n2 / 2));
        twIm_.resize(size_t(n2 / 2));
        for (int k = 0; k < n2 / 2; ++k) {
            twRe_[size_t(k)] = float(std::cos(-2.0 * std::numbers::pi * k / n2));
            twIm_[size_t(k)] = float(std::sin(-2.0 * std::numbers::pi * k / n2));
        }
        re_.resize(size_t(n2));
        im_.resize(size_t(n2));
    }
    float lowest = INFINITY;
    for (float v : in) if (std::isfinite(v)) lowest = std::min(lowest, v);
    if (!std::isfinite(lowest)) return false;
    // Pad by holding the last value: the mirrored extension is even at both
    // ends, so a flat extension adds no edge of its own.
    x_.resize(size_t(m));
    for (int i = 0; i < n; ++i) x_[size_t(i)] = std::isfinite(in[size_t(i)]) ? in[size_t(i)] : lowest;
    std::fill(x_.begin() + n, x_.end(), x_[size_t(n - 1)]);

    // Bin k of the 2M-point mirrored trace is a cosine of period 2M/k points
    // (the DCT-II coefficient k); the comb sits at kc = 2M/cutoff. Keep the
    // coarser ones, taper into the cutoff, drop the rest; 1/(2M) folded in.
    const double kc = double(n2) / cutoffPoints;
    g_.assign(size_t(n2), 0.0f);
    for (int k = 0; k <= m; ++k) {
        const double f = k / kc;
        double g = 0.0;
        if (f <= kTaperStart) g = 1.0;
        else if (f < 1.0) g = 0.5 * (1.0 + std::cos(std::numbers::pi * (f - kTaperStart) / (1.0 - kTaperStart)));
        g_[size_t(k)] = float(g / n2);
        if (k > 0 && k < m) g_[size_t(n2 - k)] = g_[size_t(k)];
    }

    a_ = x_;
    v_.resize(size_t(m));
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        for (int i = 0; i < m; ++i) {
            re_[size_t(i)] = re_[size_t(n2 - 1 - i)] = float(a_[size_t(i)]);
            im_[size_t(i)] = im_[size_t(n2 - 1 - i)] = 0.0f;
        }
        fft(re_, im_);
        // Lifter, then inverse via conj(FFT(conj(.))): only the real part is
        // wanted, which is Re(FFT(conj(.))).
        for (int k = 0; k < n2; ++k) {
            re_[size_t(k)] *= g_[size_t(k)];
            im_[size_t(k)] *= -g_[size_t(k)];
        }
        fft(re_, im_);
        double worst = 0.0;
        for (int i = 0; i < m; ++i) {
            v_[size_t(i)] = re_[size_t(i)];
            if (i < n) worst = std::max(worst, x_[size_t(i)] - v_[size_t(i)]);
        }
        if (worst < kConvergedDb) break;
        for (int i = 0; i < m; ++i)
            a_[size_t(i)] = v_[size_t(i)] + kRelaxation * (std::max(x_[size_t(i)], v_[size_t(i)]) - v_[size_t(i)]);
    }
    out.resize(size_t(n));
    for (int i = 0; i < n; ++i) out[size_t(i)] = float(v_[size_t(i)]);
    return true;
}

bool Decomb::apply(std::vector<float>& trace, double windowPoints) {
    constexpr float kClipDb = 12.0f;
    constexpr float kRampStartDb = 1.0f;
    constexpr float kRampSpanDb = 5.0f;
    if (windowPoints < 2.0) return false;
    sorted_.clear();
    for (float v : trace) if (std::isfinite(v)) sorted_.push_back(v);
    if (sorted_.size() < 4) return false;
    const auto k = std::ptrdiff_t((sorted_.size() - 1) / 5);
    std::nth_element(sorted_.begin(), sorted_.begin() + k, sorted_.end());
    const float clip = sorted_[size_t(k)] + kClipDb;
    clipped_.resize(trace.size());
    for (size_t i = 0; i < trace.size(); ++i)
        clipped_[i] = std::isfinite(trace[i]) ? std::max(trace[i], clip) : clip;
    if (!env_.compute(clipped_, envelope_, windowPoints)) return false;
    for (size_t i = 0; i < trace.size(); ++i) {
        if (!std::isfinite(trace[i])) continue;
        const float w = std::clamp((envelope_[i] - clip - kRampStartDb) / kRampSpanDb, 0.0f, 1.0f);
        trace[i] += w * (envelope_[i] - trace[i]);
    }
    return true;
}

}  // namespace ttc
