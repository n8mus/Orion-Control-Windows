// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <complex>
#include <vector>

namespace ttc {

// Minimal in-place iterative radix-2 complex FFT. Self-contained (no external DSP
// dep) — sufficient for the panadapter until WDSP is vendored. Size must be a
// power of two.
class Fft {
public:
    explicit Fft(int size);
    void forward(std::vector<std::complex<float>>& data) const;  // in-place, data.size()==size()
    int size() const { return size_; }

private:
    int size_;
    std::vector<int> rev_;
};

} // namespace ttc
