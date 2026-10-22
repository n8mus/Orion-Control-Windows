// SPDX-License-Identifier: GPL-2.0-or-later
#include "dsp/Fft.h"

#include <cmath>

namespace ttc {

Fft::Fft(int size) : size_(size), rev_(size) {
    int logn = 0;
    while ((1 << logn) < size_) ++logn;
    for (int i = 0; i < size_; ++i) {
        int r = 0;
        for (int j = 0; j < logn; ++j)
            if (i & (1 << j)) r |= 1 << (logn - 1 - j);
        rev_[i] = r;
    }
}

void Fft::forward(std::vector<std::complex<float>>& a) const {
    for (int i = 0; i < size_; ++i)
        if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);

    for (int len = 2; len <= size_; len <<= 1) {
        const float ang = -2.0f * 3.14159265358979323846f / len;
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < size_; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                const std::complex<float> u = a[i + j];
                const std::complex<float> v = a[i + j + len / 2] * w;
                a[i + j]           = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace ttc
