// SPDX-License-Identifier: GPL-2.0-or-later
// Offline skimmer verification: synthesize a 500 ksps capture with several
// CW stations calling CQ inside the 20 m CW segment, hand the engine the
// kind of averaged-spectrum frames MainWindow would, and check that every
// real callsign is spotted on the right frequency — and nothing else is.
//
//   cmake --build build --target skimtest && ./build/skimtest
#include <QCoreApplication>
#include <QHash>
#include <QString>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"

using ttc::SkimmerEngine;

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

// Text -> on/off keying envelope at the sample rate.
std::vector<bool> keying(const QString& text, int wpm, double rate) {
    const double dit = 1.2 / wpm;                  // seconds
    const auto samples = [&](double s) { return size_t(s * rate); };
    std::vector<bool> k;
    for (const QChar ch : text) {
        if (ch == ' ') {
            k.insert(k.end(), samples(7 * dit), false);
            continue;
        }
        const QString pat = morse().value(ch.toUpper());
        for (const QChar e : pat) {
            k.insert(k.end(), samples(e == '.' ? dit : 3 * dit), true);
            k.insert(k.end(), samples(dit), false);
        }
        k.insert(k.end(), samples(2 * dit), false); // char gap tops up to 3
    }
    return k;
}

struct Station {
    QString call;
    qint64  hz;          // absolute
    int     wpm;
    float   amp;
    QString text;        // sent loop; empty = the stock "CQ CQ DE ..." form
    std::vector<bool> key;
    size_t  pos = 0;
    std::complex<double> lo{1.0, 0.0}, step;
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    constexpr double kRate = 500000.0;
    constexpr qint64 kDial = 14030000;
    constexpr int    kLoOff = 60000;
    constexpr qint64 kLoAbs = kDial + kLoOff;
    constexpr int    kN = 8192;                    // spectrum bins
    const double binHz = kRate / kN;

    std::vector<Station> st = {
        {"W1AW",  14012000, 22, 0.20f},
        {"DL1ABC",14025500, 28, 0.12f},
        {"JA2XYZ",14040100, 17, 0.06f},
        {"VE3KI", 14055700, 25, 0.10f},
        // Not a callsign — must never be spotted.
        {"TEST",  14070300, 20, 0.15f},
        // Second shift, exercising the wider bank (and 45 WPM post-v2).
        {"K1TTT", 14003200, 45, 0.15f},
        // Contest cadence, NO "DE": exercises the distinct-sightings
        // confirm path and the junk gate on a T/E-heavy call — the old
        // per-character junk metric scored this exact loop 0.65 and would
        // have blocked it once sighting counts became honest (2026-08-08).
        {"N8TT",  14008700, 33, 0.12f, "CQ TEST N8TT N8TT  "},
        {"VE7CC", 14018700, 30, 0.08f},
        {"DL8AAM",14033300, 24, 0.10f},
        {"JA7QQQ",14047900, 20, 0.07f},
        {"W9RE",  14061100, 35, 0.12f},
    };
    for (auto& s : st) {
        s.key = keying(s.text.isEmpty()
                           ? QString("CQ CQ DE %1 %1 K  ").arg(s.call)
                           : s.text,
                       s.wpm, kRate);
        const double inc = 2.0 * M_PI * double(s.hz - kLoAbs) / kRate;
        s.step = {std::cos(inc), std::sin(inc)};
    }

    if (argc > 1 && QString(argv[1]) == "probe") {
        // Single-decoder sanity check on station 0, direct connection.
        ttc::CwDecoder dec(kRate, double(st[0].hz - kLoAbs));
        dec.setEnabled(true);
        QObject::connect(&dec, &ttc::CwDecoder::textDecoded, &dec,
                         [](const QString& t) {
                             printf("%s", qPrintable(t));
                             fflush(stdout);
                         },
                         Qt::DirectConnection);
        std::mt19937 prng(7);
        std::normal_distribution<float> pn(0.0f, 0.004f);
        std::vector<std::complex<float>> piq(16384);
        for (size_t done = 0; done < size_t(20.0 * kRate); done += piq.size()) {
            for (size_t i = 0; i < piq.size(); ++i) {
                std::complex<double> acc(pn(prng), pn(prng));
                for (auto& s : st) {
                    if (s.key[s.pos]) acc += double(s.amp) * s.lo;
                    s.lo *= s.step;
                    if (++s.pos >= s.key.size()) s.pos = 0;
                }
                piq[i] = std::complex<float>(float(acc.real()),
                                             float(acc.imag()));
            }
            for (auto& s : st) s.lo /= std::abs(s.lo);
            dec.processIq(piq.data(), piq.size());
        }
        printf("\n");
        return 0;
    }

    SkimmerEngine eng(kRate, 24);
    // Prefix validator standing in for cty.dat: accept the known-real ones.
    eng.setCallValidator([](const QString& c) {
        for (const char* p : {"W1", "W9", "DL", "JA", "VE", "K1", "N8"})
            if (c.startsWith(p)) return true;
        return false;
    });
    eng.setEnabled(true);

    QHash<QString, qint64> found;                  // call -> reported hz
    QObject::connect(&eng, &SkimmerEngine::spotFound, &eng,
                     [&](const QString& call, qint64 hz, int wpm) {
                         printf("  spot: %-7s %8.1f kHz  %d WPM\n",
                                qPrintable(call), hz / 1000.0, wpm);
                         found[call] = hz;
                     });

    // The averaged spectrum MainWindow would hold: flat floor + peaks.
    std::vector<float> db(kN, -120.0f);
    for (const auto& s : st) {
        const int bin = kN / 2
            + int(std::lround(double(s.hz - kLoAbs) / binHz));
        for (int k = -2; k <= 2; ++k)
            db[bin + k] = std::max(db[bin + k],
                                   -75.0f + 20.0f * std::log10(s.amp / 0.2f)
                                       - 6.0f * std::abs(k));
    }

    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.004f);
    constexpr size_t kBlock = 16384;
    std::vector<std::complex<float>> iq(kBlock);
    const size_t total = size_t(45.0 * kRate);     // 45 s on the air
    size_t sinceSpec = 0;
    for (size_t done = 0; done < total; done += kBlock) {
        for (size_t i = 0; i < kBlock; ++i) {
            std::complex<double> acc(noise(rng), noise(rng));
            for (auto& s : st) {
                if (s.key[s.pos]) acc += double(s.amp) * s.lo;
                s.lo *= s.step;
                if (++s.pos >= s.key.size()) s.pos = 0;
            }
            iq[i] = std::complex<float>(float(acc.real()), float(acc.imag()));
        }
        // Renormalize the station phasors now and then.
        for (auto& s : st) s.lo /= std::abs(s.lo);
        eng.processIq(iq.data(), kBlock);
        sinceSpec += kBlock;
        if (sinceSpec >= size_t(2.0 * kRate)) {    // every 2 simulated secs
            sinceSpec = 0;
            eng.updateFromSpectrum(db, int(kRate), kDial, kLoOff);
            QCoreApplication::processEvents();     // queued decoder signals
        }
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();

    int fails = 0;
    for (const auto& s : st) {
        if (s.call == "TEST") {
            if (found.contains("TEST")) {
                printf("FAIL: bogus TEST was spotted\n");
                ++fails;
            }
            continue;
        }
        if (!found.contains(s.call)) {
            printf("FAIL: %s never spotted\n", qPrintable(s.call));
            ++fails;
        } else if (std::llabs(found[s.call] - s.hz) > 60) {
            printf("FAIL: %s off frequency: %lld vs %lld\n",
                   qPrintable(s.call),
                   (long long)found[s.call], (long long)s.hz);
            ++fails;
        }
    }
    for (auto it = found.constBegin(); it != found.constEnd(); ++it) {
        bool real = false;
        for (const auto& s : st)
            if (s.call != "TEST" && s.call == it.key()) real = true;
        if (!real) {
            printf("FAIL: phantom spot %s\n", qPrintable(it.key()));
            ++fails;
        }
    }
    printf(fails ? "SKIMTEST: %d FAILURE(S)\n" : "SKIMTEST: PASS (all %d "
                                                 "stations, no phantoms)\n",
           fails ? fails : int(st.size()) - 1);
    return fails ? 1 : 0;
}
