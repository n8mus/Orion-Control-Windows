// RTTY core ruler: ITA2 round trip, modulator -> demodulator loopback at
// 45.45 / 50 / 75 baud, swept SNR (AWGN in a 3 kHz channel), reversed
// tones, the IQ feed (tones at +-85 Hz about 0), and a dead channel that
// must stay quiet. Prints a matrix; exits non-zero on a regression.
#include "rtty/Ita2.h"
#include "rtty/RttyDemod.h"
#include "rtty/RttyMod.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace ttc;

static int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = int(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = int(i);
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

static double accuracy(const std::string& sent, const std::string& got) {
    const int d = editDistance(sent, got);
    return 100.0 * std::max(0, int(sent.size()) - d) / double(sent.size());
}

static const char* kMsg =
    "CQ CQ CQ DE N8EM N8EM N8EM K "
    "W1AW DE N8EM UR 599 599 OH OH NAME JON JON QTH TOLEDO BT "
    "RIG ORION 565 100W ANT DIPOLE UP 40FT HW? W1AW DE N8EM K";

struct Run {
    std::string got;
    unsigned chars = 0;
};

// Modulate the message, add noise at snrDb (in a 3 kHz bandwidth, 0 dB =
// tone power equals the noise power in 3 kHz), run the demod.
static Run loopback(double baud, double snrDb, bool reverseTx, bool reverseRx,
                    bool iq, unsigned seed = 1) {
    RttyMod::Params mp;
    mp.baud = baud;
    mp.reverse = reverseTx;
    mp.stopBits = baud > 60 ? 2.0 : 1.5;
    RttyMod mod(mp);
    RttyDemod::Params dp;
    dp.baud = baud;
    dp.reverse = reverseRx;
    if (iq) { dp.markHz = 85.0; dp.spaceHz = -85.0; }
    RttyDemod dem(dp);
    Run r;
    dem.onChar = [&](char c) { r.got += c; };

    // 1.5 s of mark lead-in (the receiver's peaks and floor settle), the
    // text, then a mark tail.
    const size_t fs = 48000;
    std::vector<float> lead(size_t(1.5 * fs));
    mod.renderMark(lead.data(), lead.size());
    mod.queue(kMsg);
    std::vector<float> body;
    float blk[4800];
    while (mod.busy()) { mod.render(blk, 4800); body.insert(body.end(), blk, blk + 4800); }
    mod.renderMark(blk, 4800); body.insert(body.end(), blk, blk + 4800);
    std::vector<float> all(lead);
    all.insert(all.end(), body.begin(), body.end());

    // Noise: tone amplitude 0.5 -> power 0.125. White noise variance for
    // SNR in 3 kHz at a 48 kHz rate: N3k = P/10^(snr/10); total N = N3k *
    // (24000/3000) (real) — the demod's bit-wide filter sees only its share.
    std::mt19937 rng(seed);
    const double P = 0.5 * 0.5 / 2.0;
    const double n3k = P / std::pow(10.0, snrDb / 10.0);
    const double sigma = std::sqrt(n3k * (24000.0 / 3000.0));
    std::normal_distribution<double> g(0.0, sigma);
    if (iq) {
        // Complex baseband: tones at +-85 Hz. Make it from the audio by
        // mixing 2210 Hz down (what the skimmer's channel mixer would do),
        // noise complex with the same per-Hz density.
        std::vector<std::complex<float>> z(all.size());
        const double w = 2.0 * 3.14159265358979 * 2210.0 / fs;
        const double sigC = std::sqrt(n3k * (48000.0 / 3000.0) / 2.0);
        std::normal_distribution<double> gc(0.0, sigC);
        for (size_t i = 0; i < all.size(); ++i) {
            // LSB: audio 2125 (mark) is RF dial-2125, 170 Hz ABOVE space.
            // Shifting the real audio UP by 2210 takes its -2125 image to
            // +85 (mark) and -2295 to -85 (space): the skimmer's channel
            // convention, mark = +shift/2.
            const std::complex<double> lo(std::cos(w * i), std::sin(w * i));
            const std::complex<double> v = std::complex<double>(all[i] * 2.0, 0.0) * lo;
            z[i] = {float(v.real() + gc(rng)), float(v.imag() + gc(rng))};
        }
        dem.process(z.data(), z.size());
    } else {
        for (float& v : all) v += float(g(rng));
        dem.processReal(all.data(), all.size());
    }
    r.chars = dem.charsDecoded();
    return r;
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    int fails = 0;

    // ITA2 round trip through the sender's and receiver's case trackers.
    {
        const std::string text = "RYRY DE N8EM 599 OH ?? TU 73 ES GL (1) $5 7/8 ABC 123 ABC";
        std::string back;
        ita2::Decoder d(true);
        for (uint8_t c : ita2::encodeText(text, true)) {
            const char ch = d.feed(c);
            if (ch) back += ch;
        }
        std::printf("ITA2 round trip: %s\n", back == text ? "OK" : "FAIL");
        if (back != text) { std::printf("  got '%s'\n", back.c_str()); ++fails; }
        // Non-USOS: FIGS case survives a space.
        ita2::Decoder d2(false);
        std::string b2;
        for (uint8_t c : ita2::encodeText("599 599", false)) { const char ch = d2.feed(c); if (ch) b2 += ch; }
        std::printf("ITA2 no-USOS:    %s\n", b2 == "599 599" ? "OK" : "FAIL");
        if (b2 != "599 599") ++fails;
    }

    // Loopback matrix.
    struct Row { const char* name; double baud; double snr; bool rtx, rrx, iq; double need; };
    const std::vector<Row> rows = {
        {"45.45 clean",        45.45,  40, false, false, false, 100},
        {"45.45 +10 dB",       45.45,  10, false, false, false, 100},
        {"45.45  +3 dB",       45.45,   3, false, false, false,  99},
        {"45.45   0 dB",       45.45,   0, false, false, false,  97},
        {"45.45  -3 dB",       45.45,  -3, false, false, false,  90},
        {"45.45  -6 dB",       45.45,  -6, false, false, false,   0},   // reported, not gated
        {"50    +3 dB",        50.0,    3, false, false, false,  99},
        {"75    +3 dB",        75.0,    3, false, false, false,  99},
        {"45.45 reversed both", 45.45,  10, true,  true,  false, 100},
        {"45.45 IQ +10 dB",    45.45,  10, false, false, true,  100},
        {"45.45 IQ   0 dB",    45.45,   0, false, false, true,   97},
    };
    std::printf("%-22s %7s\n", "case", "copy%");
    for (const Row& r : rows) {
        if (quick && r.snr < 0) continue;
        const Run run = loopback(r.baud, r.snr, r.rtx, r.rrx, r.iq);
        const double acc = accuracy(kMsg, run.got);
        const bool ok = acc >= r.need;
        std::printf("%-22s %6.1f%% %s\n", r.name, acc, ok ? "" : "  FAIL");
        if (!ok) { std::printf("  got: %s\n", run.got.c_str()); ++fails; }
    }
    // Wrong-sense receive must NOT read the message (it reads garbage or
    // nothing) — a sanity check that reverse does something.
    {
        const Run run = loopback(45.45, 10, true, false, false);
        const double acc = accuracy(kMsg, run.got);
        std::printf("%-22s %6.1f%% %s\n", "mismatched sense", acc, acc < 30 ? "(garbage, as it should)" : "  FAIL");
        if (acc >= 30) ++fails;
    }
    // Dead channel: 30 s of noise alone, the squelch must hold.
    {
        RttyDemod dem;
        unsigned n = 0;
        dem.onChar = [&](char) { ++n; };
        std::mt19937 rng(7);
        std::normal_distribution<double> g(0.0, 0.05);
        std::vector<float> noise(48000 * 30);
        for (float& v : noise) v = float(g(rng));
        dem.processReal(noise.data(), noise.size());
        std::printf("%-22s %u chars in 30 s %s\n", "dead channel", n, n <= 3 ? "" : "  FAIL");
        if (n > 3) ++fails;
    }
    std::printf(fails ? "RTTY: %d FAIL\n" : "RTTY: all OK\n", fails);
    return fails ? 1 : 0;
}
