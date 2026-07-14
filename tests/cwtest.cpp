// SPDX-License-Identifier: GPL-2.0-or-later
// CwDecoder quality matrix: synthetic CW at various speeds, timing jitter
// and QSB depths; scores how many intact copies of the callsign survive.
//   cmake --build build --target cwtest && ./build/cwtest
// Run before decoder changes for a baseline, after for the comparison.
#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "cw/CwDecoder.h"

namespace {

const QHash<QChar, QString>& morse() {
    static const QHash<QChar, QString> t = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
        {'E', "."},    {'F', "..-."}, {'G', "--."},  {'H', "...."},
        {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
        {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"}, {'Z', "--.."}, {'0', "-----"},{'1', ".----"},
        {'2', "..---"},{'3', "...--"},{'4', "....-"},{'5', "....."},
        {'6', "-...."},{'7', "--..."},{'8', "---.."},{'9', "----."},
    };
    return t;
}

struct Case {
    const char* name;
    int    wpm;
    double jitter;      // sigma of lognormal element-length scatter
    double snrAmp;      // signal amplitude over noise sigma 0.004 (0 = off)
    double qsbHz;       // 0 = no fade
    double qsbDepth;    // 0..1
    double qrnPerSec;   // static-crash rate (impulses, 1-15 ms, strong)
    double freqOffHz;   // station sits this far off the assigned freq
    double driftHzSec;  // and drifts (AFC must follow)
};

// Element-timed keying with optional per-element jitter.
std::vector<bool> keying(const QString& text, int wpm, double jitter,
                         std::mt19937& rng, double rate) {
    const double dit = 1.2 / wpm;
    std::lognormal_distribution<double> j(0.0, jitter);
    const auto scale = [&] { return jitter > 0.0 ? j(rng) : 1.0; };
    std::vector<bool> k;
    const auto add = [&](double secs, bool on) {
        k.insert(k.end(), size_t(secs * rate), on);
    };
    for (const QChar ch : text) {
        if (ch == ' ') { add(7 * dit * scale(), false); continue; }
        const QString pat = morse().value(ch.toUpper());
        for (const QChar e : pat) {
            add((e == '.' ? dit : 3 * dit) * scale(), true);
            add(dit * scale(), false);
        }
        add(2 * dit * scale(), false);
    }
    return k;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // --quick: four sentinel cases (~90 s) for the inner edit loop; run the
    // full matrix before any commit that touches the decoder.
    const bool quick = argc > 1 && QString(argv[1]) == "--quick";
    const QStringList quickSet = {"20 wpm clean", "45 wpm clean",
                                  "20 wpm qsb 97%", "dead channel (quiet?)"};
    constexpr double kRate = 500000.0;
    constexpr double kOffset = -60000.0;   // where the tuned reader listens
    const QString story = "CQ CQ DE W1AW W1AW K  ";

    const Case cases[] = {
        {"12 wpm clean",   12, 0.00, 0.20, 0.0, 0.0},
        {"20 wpm clean",   20, 0.00, 0.20, 0.0, 0.0},
        {"28 wpm clean",   28, 0.00, 0.20, 0.0, 0.0},
        {"35 wpm clean",   35, 0.00, 0.20, 0.0, 0.0},
        {"40 wpm clean",   40, 0.00, 0.20, 0.0, 0.0},
        {"45 wpm clean",   45, 0.00, 0.20, 0.0, 0.0},
        {"50 wpm clean",   50, 0.00, 0.20, 0.0, 0.0},
        {"20 wpm weak",    20, 0.00, 0.02, 0.0, 0.0},
        {"28 wpm jit .12", 28, 0.12, 0.20, 0.0, 0.0},
        {"28 wpm jit .20", 28, 0.20, 0.20, 0.0, 0.0},
        {"20 wpm qsb 90%", 20, 0.00, 0.20, 0.15, 0.90},
        {"20 wpm qsb 97%", 20, 0.00, 0.20, 0.15, 0.97},
        {"25 wpm qsb 90% weak", 25, 0.05, 0.06, 0.25, 0.90},
        // Real-band cases (live-found on 40 m): static crashes over a
        // medium signal, and a channel with NO signal at all — where the
        // decoder must stay quiet instead of babbling noise copy.
        {"20 wpm + qrn",   20, 0.05, 0.10, 0.0, 0.0, 2.0},
        {"dead channel (quiet?)", 0, 0.0, 0.0, 0.0, 0.0, 1.0},
        // AFC cases: assignment error and sender drift.
        {"25 wpm +70 Hz off",  25, 0.00, 0.15, 0.0, 0.0, 0.0, 70.0, 0.0},
        {"25 wpm drift 1.5Hz/s",25, 0.00, 0.15, 0.0, 0.0, 0.0, 10.0, 1.5},
    };

    int overallScore = 0, overallMax = 0;
    for (const Case& c : cases) {
        if (quick && !quickSet.contains(c.name)) continue;
        std::mt19937 rng(11);
        std::normal_distribution<float> noise(0.0f, 0.004f);
        ttc::CwDecoder dec(kRate, kOffset);
        dec.setEnabled(true);
        QString got;
        QObject::connect(&dec, &ttc::CwDecoder::textDecoded, &dec,
                         [&](const QString& t) { got += t; },
                         Qt::DirectConnection);
        const std::vector<bool> key =
            keying(story, c.wpm > 0 ? c.wpm : 20, c.jitter, rng, kRate);
        // ~14 repetitions or 60 s, whichever is less.
        const size_t total =
            std::min(key.size() * 14, size_t(60.0 * kRate));
        const int expected =
            c.wpm > 0 ? int(total / key.size()) * 2 : 0;  // 2 calls/cycle
        std::complex<double> lo(1.0, 0.0);
        std::complex<double> step;         // recomputed per block (drift)
        std::vector<std::complex<float>> iq(16384);
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        size_t pos = 0, qrnLeft = 0;
        double t = 0.0, qrnAmp = 0.0;
        for (size_t done = 0; done < total; done += iq.size()) {
            const double f = kOffset + c.freqOffHz + c.driftHzSec * t;
            const double inc = 2.0 * M_PI * f / kRate;
            step = {std::cos(inc), std::sin(inc)};
            for (size_t i = 0; i < iq.size(); ++i) {
                double amp = c.snrAmp;
                if (c.qsbHz > 0.0) {
                    const double f =
                        0.5 * (1.0 + std::sin(2.0 * M_PI * c.qsbHz * t));
                    amp *= 1.0 - c.qsbDepth * f;
                }
                std::complex<double> s(noise(rng), noise(rng));
                if (c.qrnPerSec > 0.0) {   // static crash: strong wideband
                    if (qrnLeft == 0 && uni(rng) < c.qrnPerSec / kRate) {
                        qrnLeft = size_t((0.001 + 0.014 * uni(rng)) * kRate);
                        qrnAmp = 0.3 + 1.2 * uni(rng);
                    }
                    if (qrnLeft > 0) {
                        --qrnLeft;
                        s += std::complex<double>(qrnAmp * noise(rng) / 0.004,
                                                  qrnAmp * noise(rng) / 0.004)
                             * 0.2;      // crashes rival or exceed signals
                    }
                }
                if (c.snrAmp > 0.0 && key[pos]) s += amp * lo;
                lo *= step;
                if (++pos >= key.size()) pos = 0;
                t += 1.0 / kRate;
                iq[i] = std::complex<float>(float(s.real()),
                                            float(s.imag()));
            }
            lo /= std::abs(lo);
            dec.processIq(iq.data(), iq.size());
        }
        if (expected == 0) {
            // Dead channel: PASS is silence. Score 1 point for staying
            // under a whisper of garbage over the whole minute.
            const int chars = int(got.remove(' ').size());
            const bool quiet = chars <= 10;
            overallScore += quiet ? 1 : 0;
            overallMax += 1;
            printf("%-22s  %s  (%d garbage chars)\n", c.name,
                   quiet ? "PASS" : "FAIL", chars);
            continue;
        }
        const int hits = got.count("W1AW");
        const int pct = expected ? 100 * hits / expected : 0;
        overallScore += std::min(hits, expected);
        overallMax += expected;
        printf("%-22s  %3d%%  (%d/%d intact calls)   tail: %s\n",
               c.name, pct, hits, expected,
               qPrintable(got.right(40)));
    }
    printf("TOTAL: %d%% (%d/%d)\n", 100 * overallScore / overallMax,
           overallScore, overallMax);
    return 0;
}
