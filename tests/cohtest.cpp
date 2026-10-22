// SPDX-License-Identifier: GPL-2.0-or-later
// Coherent-detection experiment (replay-only; RSCW-inspired). Question on
// trial: how many dB does coherent bit integration buy over the console's
// envelope decoder on a MACHINE-SENT signal?
//
// Method: both decoders eat the SAME capture with increasing amounts of
// injected complex white noise. Copy quality is scored against the clean-
// capture decode (Levenshtein similarity). The horizontal gap between the
// two accuracy-vs-noise cliffs IS the coherent gain in dB — independent of
// any absolute SNR calibration.
//
// Coherent path (per RSCW's published algorithm, reimplemented):
//   exact carrier from a long FFT -> mix to true DC -> narrow LPF ->
//   integrate I/Q coherently over each dit-length bit -> magnitude per bit
//   -> balanced threshold -> runs of bits -> Morse elements -> text.
//   Dit length is auto-estimated from the envelope autocorrelation (or
//   forced with --wpm). Assumes machine timing, exactly like RSCW.
//
//   cmake --build build-rel --target cohtest
//   ./build-rel/cohtest <file.tciq> <offsetHzFromDial> [--wpm N]
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <QFile>
#include <QHash>
#include <QString>

#include "cw/CwDecoder.h"
#include "dsp/Fft.h"

using namespace ttc;

namespace {
constexpr int kLoOff = 60000;

struct Capture {
    double rate = 0.0;
    std::vector<std::complex<float>> iq;   // full file, normalized
};

bool load(const char* path, Capture& c) {
    QFile f(QString::fromLocal8Bit(path));
    char hdr[32];
    if (!f.open(QIODevice::ReadOnly) || f.read(hdr, 32) != 32
        || std::memcmp(hdr, "TTCIQ01", 7) != 0) return false;
    std::memcpy(&c.rate, hdr + 8, 8);
    std::vector<int16_t> raw(1 << 16);
    while (true) {
        const qint64 got = f.read(reinterpret_cast<char*>(raw.data()),
                                  qint64(raw.size() * 2));
        if (got <= 0) break;
        const size_t n = size_t(got) / 4;
        for (size_t i = 0; i < n; ++i)
            c.iq.push_back({raw[2 * i] / 32767.0f, raw[2 * i + 1] / 32767.0f});
    }
    return c.iq.size() > size_t(c.rate);   // at least a second
}

// Exact carrier frequency near the nominal offset: average high-resolution
// FFT power over the file, parabolic-interpolate the peak. Coherence needs
// the mixer within a fraction of 1/(dit) Hz of the truth.
double exactCarrier(const Capture& c, double nominalRelHz) {
    constexpr int kN = 1 << 20;              // ~0.48 Hz/bin at 500 ksps
    Fft fft(kN);
    std::vector<std::complex<float>> frame(kN);
    std::vector<double> acc(kN, 0.0);
    int frames = 0;
    for (size_t base = 0; base + kN <= c.iq.size() && frames < 6;
         base += kN, ++frames) {
        for (int i = 0; i < kN; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / kN);
            frame[i] = c.iq[base + i] * w;
        }
        fft.forward(frame);
        for (int i = 0; i < kN; ++i)
            acc[i] += std::norm(frame[i]);
    }
    const double binHz = c.rate / kN;
    // search ±80 Hz around nominal (AFC-bound sized window)
    int best = -1;
    for (int i = 0; i < kN; ++i) {
        const double hz = (i < kN / 2 ? i : i - kN) * binHz;
        if (std::abs(hz - nominalRelHz) > 80.0) continue;
        if (best < 0 || acc[i] > acc[best]) best = i;
    }
    if (best <= 0 || best >= kN - 1) return nominalRelHz;
    const double ym = acc[(best - 1 + kN) % kN], y0 = acc[best],
                 yp = acc[(best + 1) % kN];
    const double den = ym - 2.0 * y0 + yp;
    const double d = std::abs(den) > 1e-12
        ? std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5) : 0.0;
    const double hz = ((best < kN / 2 ? best : best - kN) + d) * binHz;
    return hz;
}

// Mix to DC + LPF + decimate to 2 ksps complex baseband.
std::vector<std::complex<float>> baseband(const std::vector<std::complex<float>>& iq,
                                          double rate, double relHz) {
    const int decim = int(rate / 2000.0);
    const double inc = -2.0 * M_PI * relHz / rate;
    const std::complex<double> step(std::cos(inc), std::sin(inc));
    std::complex<double> lo(1.0, 0.0), acc(0.0, 0.0);
    std::vector<std::complex<float>> out;
    out.reserve(iq.size() / decim + 1);
    std::complex<float> lp1{0, 0}, lp2{0, 0};
    const float a = 1.0f - std::exp(-2.0f * float(M_PI) * 50.0f / 2000.0f);
    int n = 0, renorm = 0;
    for (const auto& z : iq) {
        acc += std::complex<double>(z) * lo;
        lo *= step;
        if (++renorm >= 4096) { renorm = 0; lo /= std::abs(lo); }
        if (++n < decim) continue;
        std::complex<float> s(float(acc.real() / decim), float(acc.imag() / decim));
        acc = {0, 0};
        n = 0;
        lp1 += a * (s - lp1);
        lp2 += a * (lp1 - lp2);
        out.push_back(lp2);
    }
    return out;
}

// Dit estimate from the envelope autocorrelation: Morse keying's
// fundamental periodicity is 2 dits (dit+gap). Search 20..140 ms.
double estimateDitMs(const std::vector<std::complex<float>>& bb) {
    std::vector<float> env(bb.size());
    for (size_t i = 0; i < bb.size(); ++i) env[i] = std::abs(bb[i]);
    float mean = 0.0f;
    for (float v : env) mean += v;
    mean /= env.size();
    for (float& v : env) v -= mean;
    double bestScore = -1.0, bestLag = 120.0;   // lag in samples (0.5 ms each)
    for (int lag = 80; lag <= 560; lag += 2) {  // 2*dit of 20..140 ms
        double s = 0.0;
        for (size_t i = 0; i + lag < env.size(); i += 3)
            s += env[i] * env[i + lag];
        if (s > bestScore) { bestScore = s; bestLag = lag; }
    }
    return bestLag * 0.5 / 2.0;                 // lag = 2 dits
}

// Coherent bit decoder: integrate complex baseband over dit-length bits at
// the best clock phase, magnitude per bit, balanced threshold, then element
// run-lengths -> Morse.
// Bit-power sequence at a given (fractional) bit length and phase: coherent
// I/Q sum inside each bit, magnitude out. Fractional boundaries matter — an
// integer stride at 18 WPM (true dit 133.33 samples) forces a ~2% clock
// error that walks through every window (found the hard way: clean W1AW
// decoded to E/T soup twice before this).
std::vector<double> bitPowers(const std::vector<std::complex<float>>& bb,
                              double bitSamples, double phase,
                              size_t from, size_t to) {
    std::vector<double> out;
    for (double p = from + phase; p + bitSamples <= double(to);
         p += bitSamples) {
        const size_t b0 = size_t(p), b1 = size_t(p + bitSamples);
        std::complex<double> acc(0, 0);
        for (size_t i = b0; i < b1; ++i) acc += std::complex<double>(bb[i]);
        out.push_back(std::abs(acc));
    }
    return out;
}

double bitVariance(const std::vector<double>& v) {
    if (v.size() < 8) return 0.0;
    double s = 0, s2 = 0;
    for (double x : v) { s += x; s2 += x * x; }
    const double m = s / v.size();
    // Normalized contrast (CV²): raw variance grows with bit length no
    // matter the alignment, so an unnormalized score drags the clock
    // search toward longer dits (live-found: refine pinned at +8%).
    if (m <= 0.0) return 0.0;
    return (s2 / v.size() - m * m) / (m * m);
}

// Refine the dit to ~0.1%: 2-D grid over (dit, phase), maximizing bit-power
// contrast over the first ~40 s. RSCW's lesson in one line: the clock IS
// the decoder.
double refineDit(const std::vector<std::complex<float>>& bb, double ditMs0) {
    const size_t to = std::min(bb.size(), size_t(40 * 2000));
    double best = ditMs0, bestScore = -1.0;
    for (double dit = ditMs0 * 0.92; dit <= ditMs0 * 1.08; dit += ditMs0 * 0.001) {
        const double bitS = dit * 2.0;
        for (int ph = 0; ph < 8; ++ph) {
            const auto v = bitPowers(bb, bitS, ph * bitS / 8.0, 0, to);
            const double score = bitVariance(v);
            if (score > bestScore) { bestScore = score; best = dit; }
        }
    }
    return best;
}

QString coherentDecode(const std::vector<std::complex<float>>& bb, double ditMs) {
    const double bitS = ditMs * 2.0;         // samples per bit, fractional
    // Per-window phase re-lock + per-window balanced threshold (QSB and
    // long pauses must not skew a global one); ~60 bits per window.
    const size_t win = size_t(bitS * 60.0);
    std::vector<double> bits;
    for (size_t w0 = 0; w0 + size_t(bitS * 8) < bb.size(); w0 += win) {
        const size_t w1 = std::min(bb.size(), w0 + win);
        double bestPh = 0.0, bestVar = -1.0;
        for (int ph = 0; ph < 16; ++ph) {
            const auto v = bitPowers(bb, bitS, ph * bitS / 16.0, w0, w1);
            const double var = bitVariance(v);
            if (var > bestVar) { bestVar = var; bestPh = ph * bitS / 16.0; }
        }
        auto wb = bitPowers(bb, bitS, bestPh, w0, w1);
        if (wb.empty()) continue;
        // Balanced threshold (RSCW): mean distance above == below.
        double th = 0.0;
        for (double v : wb) th += v;
        th /= wb.size();
        for (int it = 0; it < 24; ++it) {
            double up = 0, dn = 0;
            int nu = 0, nd = 0;
            for (double v : wb) {
                if (v > th) { up += v - th; ++nu; }
                else        { dn += th - v; ++nd; }
            }
            if (!nu || !nd) break;
            th += 0.5 * (up / nu - dn / nd);
        }
        if (th <= 0.0) continue;
        for (double v : wb) bits.push_back(v / th);
    }
    const double th = 1.0;
    // Runs of bits -> elements -> characters.
    static const QHash<QString, QChar> table = [] {
        QHash<QString, QChar> t;
        const char* codes[] = {
            ".-A", "-...B", "-.-.C", "-..D", ".E", "..-.F", "--.G", "....H",
            "..I", ".---J", "-.-K", ".-..L", "--M", "-.N", "---O", ".--.P",
            "--.-Q", ".-.R", "...S", "-T", "..-U", "...-V", ".--W", "-..-X",
            "-.--Y", "--..Z", "-----0", ".----1", "..---2", "...--3",
            "....-4", ".....5", "-....6", "--...7", "---..8", "----.9",
            "-..-./", "..--..?", ".-.-.-.", "--..--,", "-...-=", ".-.-.+",
            nullptr };
        for (int i = 0; codes[i]; ++i) {
            const QString s = QString::fromLatin1(codes[i]);
            t.insert(s.left(s.size() - 1), s[s.size() - 1]);
        }
        return t;
    }();
    QString out, sym;
    int run = 0;
    bool cur = false;
    const auto flushSym = [&] {
        if (sym.isEmpty()) return;
        const auto it = table.constFind(sym);
        out += it != table.constEnd() ? it.value() : QChar('*');
        sym.clear();
    };
    for (double v : bits) {
        const bool on = v > th;
        if (on == cur) { ++run; continue; }
        if (cur) sym += run >= 2 ? '-' : '.';         // mark ended
        else {                                        // gap ended
            if (run >= 5) { flushSym(); out += ' '; } // word gap
            else if (run >= 2) flushSym();            // char gap
        }
        cur = on;
        run = 1;
    }
    if (cur) sym += run >= 2 ? '-' : '.';
    flushSym();
    return out.simplified();
}

// Envelope side: the production CwDecoder fed the same samples.
QString envelopeDecode(const std::vector<std::complex<float>>& iq, double rate,
                       double relHz) {
    CwDecoder dec(rate, relHz);
    dec.setEnabled(true);
    QString text;
    QObject::connect(&dec, &CwDecoder::textDecoded, &dec,
                     [&](const QString& t) { text += t; },
                     Qt::DirectConnection);
    constexpr size_t kBlk = 16384;
    for (size_t i = 0; i + kBlk <= iq.size(); i += kBlk)
        dec.processIq(iq.data() + i, kBlk);
    return text.simplified();
}

int levenshtein(const QString& a, const QString& b) {
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

double similarity(const QString& ref, const QString& got) {
    if (ref.isEmpty()) return 0.0;
    return std::max(0.0, 1.0 - double(levenshtein(ref, got))
                             / std::max(ref.size(), got.size()));
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: cohtest <file.tciq> <offsetHzFromDial> [--wpm N]\n");
        return 2;
    }
    Capture cap;
    if (!load(argv[1], cap)) { printf("bad .tciq\n"); return 2; }
    const double nominal = atof(argv[2]) - kLoOff;
    double wpmForce = 0.0;
    for (int i = 3; i + 1 < argc + 1; ++i)
        if (QString(argv[i]) == "--wpm" && i + 1 < argc)
            wpmForce = atof(argv[i + 1]);

    printf("capture: %.1f s @ %.0f ksps\n", cap.iq.size() / cap.rate,
           cap.rate / 1000.0);
    const double carrier = exactCarrier(cap, nominal);
    printf("carrier: %+.2f Hz from dial (nominal %+.0f)\n",
           carrier + kLoOff, nominal + kLoOff);

    // Signal power at the carrier for noise scaling: mean |baseband|^2
    // during the top-quartile (key-down) samples.
    auto bbClean = baseband(cap.iq, cap.rate, carrier);
    std::vector<float> mags(bbClean.size());
    for (size_t i = 0; i < bbClean.size(); ++i) mags[i] = std::abs(bbClean[i]);
    std::vector<float> sorted = mags;
    std::sort(sorted.begin(), sorted.end());
    const float keyMag = sorted[size_t(sorted.size() * 0.9)];
    printf("key-down level (p90 baseband): %.5f\n", keyMag);

    double ditMs = wpmForce > 0.0 ? 1200.0 / wpmForce
                                  : estimateDitMs(bbClean);
    ditMs = refineDit(bbClean, ditMs);
    printf("dit: %.2f ms (%.1f WPM)%s + refined\n", ditMs, 1200.0 / ditMs,
           wpmForce > 0 ? " [forced]" : " [autocorr]");

    // Reference transcripts from the clean capture.
    const QString refCoh = coherentDecode(bbClean, ditMs);
    const QString refEnv = envelopeDecode(cap.iq, cap.rate, carrier);
    printf("\nclean coherent : %s\n", qPrintable(refCoh.left(120)));
    printf("clean envelope : %s\n\n", qPrintable(refEnv.left(120)));

    // Noise sweep: add complex white noise, 3 dB steps. Sigma is expressed
    // relative to the key-down baseband level mapped back to full rate
    // (the baseband boxcar+LPF passes noise in ~100 Hz of the 500 kHz, so
    // full-rate sigma = keyMag * 10^(-snr/20) * sqrt(rate/2/100) roughly —
    // the absolute calibration doesn't matter, only the SWEEP: both
    // decoders see identical samples at every step).
    printf("%8s  %-28s %-28s\n", "noise", "envelope decoder", "coherent decoder");
    std::mt19937 rng(7);
    for (int stepDb = 0; stepDb <= 21; stepDb += 3) {
        const float sigma = keyMag * std::pow(10.0f, stepDb / 20.0f) * 2.0f;
        std::normal_distribution<float> g(0.0f, sigma);
        std::vector<std::complex<float>> noisy(cap.iq.size());
        for (size_t i = 0; i < cap.iq.size(); ++i)
            noisy[i] = cap.iq[i] + std::complex<float>(g(rng), g(rng));
        const auto bbN = baseband(noisy, cap.rate, carrier);
        const QString ce = coherentDecode(bbN, ditMs);
        const QString ee = envelopeDecode(noisy, cap.rate, carrier);
        printf("  +%2d dB  sim %.2f |%-18s|  sim %.2f |%-18s|\n", stepDb,
               similarity(refEnv, ee), qPrintable(ee.left(18)),
               similarity(refCoh, ce), qPrintable(ce.left(18)));
    }
    printf("\n(cliff gap in dB between the two columns = coherent gain)\n");
    return 0;
}
