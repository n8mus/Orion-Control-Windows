// SPDX-License-Identifier: GPL-2.0-or-later
// Noise-reduction ruler for the RADIO-audio CW path: keyed 550 Hz tone in
// noise (white + QRN impulses) at swept SNR, decoded by the production
// audio-path decoder (CwDecoder at 48 kHz tuned to +550) after each NR
// contender. Contenders: none, RNNoise (speech-trained, system lib), and
// our SpectralNr tone-oriented gate. Accuracy = Levenshtein similarity to
// the known text. Build the ruler before believing anyone's marketing —
// including our own.
//   cmake --build build-rel --target nrtest && ./build-rel/nrtest
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include <QCoreApplication>
#include <QString>

#include <rnnoise.h>

#include "audio/SpectralNr.h"
#include "cw/CwDecoder.h"

using namespace ttc;

namespace {
constexpr double kRate = 48000.0;
constexpr double kTone = 550.0;
const char* kText = "CQ CQ DE N8EM N8EM K ";

const char* morse(char c) {
    switch (c) {
        case 'C': return "-.-."; case 'Q': return "--.-"; case 'D': return "-..";
        case 'E': return "."; case 'N': return "-."; case '8': return "---..";
        case 'M': return "--"; case 'K': return "-.-";
    }
    return "";
}

std::vector<float> keyedTone(int wpm, int reps) {
    const int dit = int(kRate * 1.2 / wpm);
    std::vector<float> key;
    auto add = [&](int n, float v) { key.insert(key.end(), size_t(n), v); };
    add(int(kRate), 0.0f);
    for (int r = 0; r < reps; ++r)
        for (const char* c = kText; *c; ++c) {
            if (*c == ' ') { add(7 * dit, 0.0f); continue; }
            for (const char* p = morse(*c); *p; ++p) {
                add(*p == '.' ? dit : 3 * dit, 1.0f);
                add(dit, 0.0f);
            }
            add(2 * dit, 0.0f);
        }
    std::vector<float> audio(key.size());
    double ph = 0.0;
    float env = 0.0f;
    for (size_t i = 0; i < key.size(); ++i) {
        env += (key[i] - env) * 0.003f;    // ~5 ms edges
        ph += 2.0 * M_PI * kTone / kRate;
        audio[i] = 0.35f * env * float(std::sin(ph));
    }
    return audio;
}

QString decode(const std::vector<float>& audio) {
    CwDecoder dec(kRate, kTone);
    dec.setEnabled(true);
    QString out;
    QObject::connect(&dec, &CwDecoder::textDecoded, &dec,
                     [&](const QString& t) { out += t; },
                     Qt::DirectConnection);
    std::vector<std::complex<float>> blk(4800);
    for (size_t b = 0; b + blk.size() <= audio.size(); b += blk.size()) {
        for (size_t i = 0; i < blk.size(); ++i)
            blk[i] = {audio[b + i], 0.0f};
        dec.processIq(blk.data(), blk.size());
    }
    return out.simplified();
}

int lev(const QString& a, const QString& b) {
    const int n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[m];
}

double sim(const QString& ref, const QString& got) {
    if (ref.isEmpty()) return 0.0;
    return std::max(0.0,
                    1.0 - double(lev(ref, got)) / std::max(ref.size(), got.size()));
}

// RNNoise expects 480-sample frames of floats in s16 range.
std::vector<float> rnnoisePath(const std::vector<float>& in) {
    DenoiseState* st = rnnoise_create(nullptr);
    std::vector<float> out(in.size(), 0.0f);
    const int fs = rnnoise_get_frame_size();
    std::vector<float> fin(fs), fout(fs);
    for (size_t b = 0; b + size_t(fs) <= in.size(); b += size_t(fs)) {
        for (int i = 0; i < fs; ++i) fin[i] = in[b + i] * 32768.0f;
        rnnoise_process_frame(st, fout.data(), fin.data());
        for (int i = 0; i < fs; ++i) out[b + i] = fout[i] / 32768.0f;
    }
    rnnoise_destroy(st);
    return out;
}

std::vector<float> spectralPath(const std::vector<float>& in) {
    SpectralNr nr;
    std::vector<float> out = in;
    nr.process(out.data(), int(out.size()));
    return out;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString ref =
        QString::fromLatin1(kText).repeated(4).simplified();

    // Sanity: SpectralNr must be ~unity gain on a clean tone before any
    // of its scores mean anything.
    {
        auto clean = keyedTone(20, 1);
        auto proc = spectralPath(clean);
        double ei = 0, eo = 0;
        for (size_t i = 48000; i < clean.size(); ++i) {   // skip latency
            ei += clean[i] * clean[i];
            eo += proc[i] * proc[i];
        }
        printf("SpectralNr clean-tone gain: %.2f dB (want ~0)\n\n",
               10.0 * std::log10(eo / ei));
    }

    printf("%6s  %-26s %-26s %-26s\n", "SNR", "no NR", "RNNoise", "SpectralNr");
    std::mt19937 rng(11);
    for (double snrDb = 12.0; snrDb >= -6.0; snrDb -= 3.0) {
        auto audio = keyedTone(20, 4);
        // White noise to target SNR (tone RMS ~0.35/sqrt2), plus QRN
        // impulses because band noise is never polite gaussian.
        const float sig = 0.35f / std::sqrt(2.0f);
        const float ns = sig / std::pow(10.0f, float(snrDb) / 20.0f);
        std::normal_distribution<float> g(0.0f, ns);
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (auto& v : audio) {
            v += g(rng);
            if (u(rng) < 1e-4f) v += (u(rng) - 0.5f) * 8.0f * ns;  // QRN pip
        }
        const QString d0 = decode(audio);
        const QString d1 = decode(rnnoisePath(audio));
        const QString d2 = decode(spectralPath(audio));
        const auto cell = [&](const QString& d) {
            return QString("%1 |%2|").arg(sim(ref, d), 4, 'f', 2)
                                     .arg(d.left(18));
        };
        printf("%+4.0f dB  %-26s %-26s %-26s\n", snrDb,
               qPrintable(cell(d0)), qPrintable(cell(d1)),
               qPrintable(cell(d2)));
    }
    return 0;
}
