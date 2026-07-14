// SPDX-License-Identifier: GPL-2.0-or-later
// Offline band-recording workbench: runs a .tciq capture (SDR menu ->
// "Record band IQ", or TTC_RECIQ) through the skimmer bank and a tuned
// reader exactly the way the live console does, printing what got heard.
// This is how the decoder gets tuned against REAL signals instead of
// synthetic ones.
//   cmake --build build --target skimreplay
//   ./build/skimreplay <file.tciq> [readerOffsetHzFromDial]
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"
#include "dsp/SpectrumComputer.h"
#include "util/CtyLookup.h"

using namespace ttc;

namespace {
// --gen: writes a synthetic 2-station capture (one drifting) so the whole
// record->replay chain can be verified without an SDR or a band.
int generate(const char* path) {
    const double rate = 500000.0, loAbs = 14090000.0;   // dial 14.030
    QFile f(QString::fromLocal8Bit(path));
    if (!f.open(QIODevice::WriteOnly)) return 2;
    char hdr[32] = {};
    std::memcpy(hdr, "TTCIQ01", 8);
    std::memcpy(hdr + 8, &rate, 8);
    std::memcpy(hdr + 16, &loAbs, 8);
    const qint64 ep = QDateTime::currentMSecsSinceEpoch();
    std::memcpy(hdr + 24, &ep, 8);
    f.write(hdr, 32);
    struct St {
        const char* text; int wpm; double hz, amp, drift;
        std::vector<bool> key; size_t pos = 0;
        std::complex<double> lo{1.0, 0.0};
    };
    const auto keying = [&](const char* text, int wpm) {
        static const struct { char c; const char* p; } tab[] = {
            {'A', ".-"}, {'C', "-.-."}, {'D', "-.."}, {'E', "."},
            {'Q', "--.-"}, {'W', ".--"}, {'K', "-.-"}, {'1', ".----"},
            {'8', "---.."}, {'N', "-."}, {'M', "--"}, {'U', "..-"},
            {'S', "..."},
        };
        const double dit = 1.2 / wpm;
        std::vector<bool> k;
        const auto add = [&](double s, bool on) {
            k.insert(k.end(), size_t(s * rate), on);
        };
        for (const char* c = text; *c; ++c) {
            if (*c == ' ') { add(7 * dit, false); continue; }
            for (const auto& e : tab)
                if (e.c == *c) {
                    for (const char* p = e.p; *p; ++p) {
                        add(*p == '.' ? dit : 3 * dit, true);
                        add(dit, false);
                    }
                    add(2 * dit, false);
                    break;
                }
        }
        return k;
    };
    std::vector<St> st = {
        {"CQ CQ DE W1AW W1AW K  ", 25, 14012000.0, 0.15, 0.0},
        {"CQ CQ DE N8MUS N8MUS K  ", 18, 14041500.0, 0.06, 1.0},
    };
    for (auto& s : st) s.key = keying(s.text, s.wpm);
    // Shape the keying with ~5 ms raised-cosine edges like a real rig —
    // hard edges splatter kHz-wide and nothing on the air looks like that.
    std::vector<std::vector<float>> shaped(st.size());
    for (size_t si = 0; si < st.size(); ++si) {
        const auto& k = st[si].key;
        auto& e = shaped[si];
        e.resize(k.size());
        const double eSecs = 0.005;
        const int eN = int(eSecs * rate);
        float v = 0.0f;
        for (size_t i = 0; i < k.size(); ++i) {
            const float target = k[i] ? 1.0f : 0.0f;
            v += (target - v) * std::min(1.0, 3.0 / eN);
            e[i] = v;
        }
    }
    srand(7);
    const auto nz = [] { return ((rand() / double(RAND_MAX)) - 0.5) * 0.016; };
    std::vector<int16_t> buf(16384 * 2);
    double t = 0.0;
    for (size_t done = 0; done < size_t(25.0 * rate); done += 16384) {
        for (size_t i = 0; i < 16384; ++i) {
            std::complex<double> acc(nz(), nz());
            for (size_t si = 0; si < st.size(); ++si) {
                auto& s = st[si];
                const double inc =
                    2.0 * M_PI * (s.hz - loAbs + s.drift * t) / rate;
                s.lo *= std::complex<double>(std::cos(inc), std::sin(inc));
                acc += s.amp * double(shaped[si][s.pos]) * s.lo;
                if (++s.pos >= s.key.size()) s.pos = 0;
            }
            t += 1.0 / rate;
            buf[2 * i] = int16_t(std::clamp(acc.real(), -1.0, 1.0) * 32767);
            buf[2 * i + 1] =
                int16_t(std::clamp(acc.imag(), -1.0, 1.0) * 32767);
        }
        for (auto& s : st) s.lo /= std::abs(s.lo);
        f.write(reinterpret_cast<const char*>(buf.data()),
                qint64(buf.size() * 2));
    }
    printf("generated %s\n", path);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        printf("usage: skimreplay <file.tciq> [readerOffsetHzFromDial]\n"
               "       skimreplay --gen <out.tciq>\n");
        return 2;
    }
    if (QString(argv[1]) == "--gen" && argc > 2) return generate(argv[2]);
    // --peek <file> <offsetHzFromDial>: dump the narrowband envelope at
    // one frequency, 100 ms per line — keying rhythm and SNR by eye.
    if (QString(argv[1]) == "--peek" && argc > 3) {
        QFile pf(QString::fromLocal8Bit(argv[2]));
        char ph[32];
        if (!pf.open(QIODevice::ReadOnly) || pf.read(ph, 32) != 32) return 2;
        double pRate = 0.0;
        std::memcpy(&pRate, ph + 8, 8);
        const double off = atof(argv[3]) - 60000.0;   // rel. to the LO
        const double inc = -2.0 * M_PI * off / pRate;
        const std::complex<double> step(std::cos(inc), std::sin(inc));
        std::complex<double> lo(1.0, 0.0), acc(0.0, 0.0);
        std::complex<float> lp1{0, 0}, lp2{0, 0};
        const float a = 1.0f - std::exp(-2.0f * float(M_PI) * 60.0f / 2000.0f);
        std::vector<int16_t> raw(16384 * 2);
        int accN = 0, col = 0;
        double t = 0.0;
        float mx = 0.0f;
        printf("100 ms per char: '.'<0.002 ':'<0.01 '+'<0.05 '#'>=0.05\n");
        while (pf.read(reinterpret_cast<char*>(raw.data()),
                       qint64(raw.size() * 2)) == qint64(raw.size() * 2)) {
            for (size_t i = 0; i < raw.size() / 2; ++i) {
                const std::complex<double> z(raw[2 * i] / 32767.0,
                                             raw[2 * i + 1] / 32767.0);
                acc += z * lo;
                lo *= step;
                if (++accN < 250) continue;
                std::complex<float> zz(float(acc.real() / 250),
                                       float(acc.imag() / 250));
                acc = {0, 0};
                accN = 0;
                lp1 += a * (zz - lp1);
                lp2 += a * (lp1 - lp2);
                mx = std::max(mx, std::abs(lp2));
                t += 0.5;
                if (t >= 100.0) {
                    t = 0.0;
                    putchar(mx < 0.002f ? '.' : mx < 0.01f ? ':'
                            : mx < 0.05f ? '+' : '#');
                    mx = 0.0f;
                    if (++col % 100 == 0) putchar('\n');
                }
            }
            lo /= std::abs(lo);
        }
        putchar('\n');
        return 0;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    char hdr[32];
    if (!f.open(QIODevice::ReadOnly) || f.read(hdr, 32) != 32
        || std::memcmp(hdr, "TTCIQ01", 7) != 0) {
        printf("bad .tciq file\n");
        return 2;
    }
    double rate = 0.0, center = 0.0;
    qint64 epoch = 0;
    std::memcpy(&rate, hdr + 8, 8);
    std::memcpy(&center, hdr + 16, 8);
    std::memcpy(&epoch, hdr + 24, 8);
    constexpr int kLoOff = 60000;
    const qint64 dial = qint64(center) - kLoOff;
    const double secs = (f.size() - 32) / (rate * 4.0);
    printf("capture: %.1f s at %.0f ksps, dial %.4f MHz, taken %s\n",
           secs, rate / 1000.0, dial / 1e6,
           qPrintable(QDateTime::fromMSecsSinceEpoch(epoch)
                          .toUTC().toString("yyyy-MM-dd hh:mm 'UTC'")));

    CtyLookup cty;
    cty.load();                            // :/cty.dat from the resources
    SkimmerEngine skim(rate, 24);
    skim.setCallValidator([&cty](const QString& c) {
        double la, lo;
        return cty.lookup(c, la, lo);
    });
    skim.setEnabled(true);
    QObject::connect(&skim, &SkimmerEngine::spotFound, &skim,
                     [](const QString& call, qint64 hz, int wpm) {
                         printf("  SPOT %-10s %9.1f kHz  %2d WPM\n",
                                qPrintable(call), hz / 1000.0, wpm);
                     });

    // Tuned reader at the dial (or a chosen offset from it).
    const int rdOff = argc > 2 ? atoi(argv[2]) : 0;
    CwDecoder reader(rate, double(rdOff - kLoOff));
    reader.setEnabled(true);
    QString readerText;
    QObject::connect(&reader, &CwDecoder::textDecoded, &reader,
                     [&](const QString& t) { readerText += t; },
                     Qt::DirectConnection);

    SpectrumComputer spectrum(8192);
    std::vector<float> lastDb;
    spectrum.setOutput([&](const std::vector<float>& db) { lastDb = db; });

    std::vector<int16_t> raw(16384 * 2);
    IqBlock blk(16384);
    double t = 0.0, nextAssign = 0.5;
    while (f.read(reinterpret_cast<char*>(raw.data()),
                  qint64(raw.size() * 2)) == qint64(raw.size() * 2)) {
        for (size_t i = 0; i < blk.size(); ++i)
            blk[i] = {raw[2 * i] / 32767.0f, raw[2 * i + 1] / 32767.0f};
        spectrum.addSamples(blk);
        reader.processIq(blk.data(), blk.size());
        skim.processIq(blk.data(), blk.size());
        t += blk.size() / rate;
        if (t >= nextAssign && !lastDb.empty()) {
            nextAssign = t + 2.5;
            skim.updateFromSpectrum(lastDb, int(rate), dial, kLoOff);
        }
        QCoreApplication::processEvents();
    }
    QCoreApplication::processEvents();

    printf("\n--- channels after %.0f s ---\n", t);
    for (const auto& c : skim.channelInfo()) {
        if (!c.active) continue;
        printf("  %9.1f kHz  %2d WPM  %-10s |%s|\n", c.hz / 1000.0, c.wpm,
               c.call.isEmpty() ? "?" : qPrintable(c.call),
               qPrintable(c.text.right(48)));
    }
    printf("--- tuned reader (offset %+d Hz from dial) ---\n%s\n", rdOff,
           qPrintable(readerText));
    return 0;
}
