// SPDX-License-Identifier: GPL-3.0-or-later
// SDR receive chain, end to end on synthetic IQ (no audio device, no SDR):
//   cmake --build build --target sdrrxtest && ./build/sdrrxtest
// IQ at 1 MS/s with the dial 260 kHz below the LO, as on the air:
// - USB: a tone 1 kHz above the dial comes out at 1 kHz
// - LSB: the same tone is rejected (opposite sideband)
// - CWU: a carrier ON the dial comes out at the sidetone pitch (600 Hz)
// - AM: a 1 kHz-modulated carrier on the dial demodulates to 1 kHz
// - NR2 runs on noise without falling over, and lowers it
// - RADE (when built): rade_c's off-air recording on the dial, USB through
//   WDSP, decodes to speech and syncs
// - RX EQ: a +6 dB peak at 1 kHz lifts the USB tone ~6 dB; EQ off = bypass
// - RX dynamics: the final limiter holds -1 dBFS, MUTE is silence, BOOST
//   is +6 dB, the compressor narrows a 20 dB step, the gate turns a tone
//   under its threshold down by its depth and leaves one above it alone
#include <QCoreApplication>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "sdr/SdrRx.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); ++fails; }
}

constexpr double kFs = 1e6, kLoOff = 260000.0;

// Run `secs` of IQ through a fresh receiver; return the last second of audio.
template <class Gen>
static std::vector<float> run(Mode m, int lo, int hi, const SdrRx::Settings& st,
                              double secs, Gen gen, SdrRx::RadeStatus* rade = nullptr) {
    SdrRx rx;
    std::mutex mx;
    std::vector<float> audio;
    rx.setAudioTap([&](const float* x, int n) {
        std::lock_guard<std::mutex> lk(mx);
        audio.insert(audio.end(), x, x + n);
    });
    rx.setDemod(m, lo, hi, 600);
    rx.setSettings(st);
    rx.retune(-kLoOff);
    rx.startTapOnly();
    while (!rx.ready()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const size_t blk = 10000;                       // 10 ms
    std::vector<std::complex<float>> iq(blk);
    const size_t total = size_t(secs * kFs);
    for (size_t o = 0; o < total; o += blk) {
        for (size_t i = 0; i < blk; ++i) iq[i] = gen(double(o + i) / kFs);
        rx.processIq(iq.data(), blk);
        std::this_thread::sleep_for(std::chrono::microseconds(1500));   // ~6x real time
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (rade) *rade = rx.radeStatus();
    rx.stop();
    std::lock_guard<std::mutex> lk(mx);
    if (audio.size() > 48000) audio.erase(audio.begin(), audio.end() - 48000);
    return audio;
}

static double toneDb(const std::vector<float>& x, double hz) {
    double re = 0, im = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        re += x[i] * std::cos(2 * M_PI * hz * double(i) / 48000.0);
        im += x[i] * std::sin(2 * M_PI * hz * double(i) / 48000.0);
    }
    return 20 * std::log10(std::hypot(re, im) / double(x.size()) * 2 + 1e-12);
}
static double rmsDb(const std::vector<float>& x) {
    double a = 0;
    for (float v : x) a += double(v) * v;
    return 10 * std::log10(a / double(std::max<size_t>(1, x.size())) + 1e-20);
}

// A signal at `rfHz` above the dial, in the SDR's IQ (LO = dial + 260 kHz).
static std::complex<float> at(double t, double rfHz, double amp) {
    const double ph = 2 * M_PI * (rfHz - kLoOff) * t;
    return {float(amp * std::cos(ph)), float(amp * std::sin(ph))};
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    SdrRx::Settings st;
    st.agc = SdrRx::Agc::Fixed;                     // levels comparable
    st.volumeDb = -40;                              // fixed gain: keep under the limiter

    auto usb = run(Mode::USB, 0, 2800, st, 2.0, [](double t) { return at(t, 1000, 0.01); });
    const double u1k = toneDb(usb, 1000), u2k = toneDb(usb, 2000);
    printf("  USB: 1 kHz tone %.1f dB, 2 kHz (nothing) %.1f dB\n", u1k, u2k);
    check(usb.size() >= 40000, "USB produced audio");
    check(u1k - u2k > 30, "USB: +1 kHz tone demodulates at 1 kHz");

    auto lsb = run(Mode::LSB, -2800, 0, st, 2.0, [](double t) { return at(t, 1000, 0.01); });
    printf("  LSB: same +1 kHz tone %.1f dB (USB had %.1f)\n", toneDb(lsb, 1000), u1k);
    check(u1k - toneDb(lsb, 1000) > 30, "LSB rejects the upper-sideband tone");

    auto cw = run(Mode::CWU, -250, 250, st, 2.0, [](double t) { return at(t, 0, 0.01); });
    const double c600 = toneDb(cw, 600), c400 = toneDb(cw, 400);
    printf("  CWU: carrier on the dial -> 600 Hz %.1f dB, 400 Hz %.1f dB\n", c600, c400);
    check(c600 - c400 > 25, "CW carrier on the dial plays at the 600 Hz pitch");

    auto am = run(Mode::AM, -4000, 4000, st, 2.0, [](double t) {
        const double env = 0.01 * (1.0 + 0.5 * std::sin(2 * M_PI * 1000 * t));
        return at(t, 0, env);
    });
    const double a1k = toneDb(am, 1000), a1k5 = toneDb(am, 1500);
    printf("  AM: 1 kHz modulation %.1f dB, 1.5 kHz %.1f dB\n", a1k, a1k5);
    check(a1k - a1k5 > 25, "AM demodulates its 1 kHz modulation");

    // RX EQ: one +6 dB peak on the tone; the same bands with EQ off leave
    // the tone where it was.
    SdrRx::Settings eq = st;
    for (auto& b : eq.eq) b.enabled = false;
    eq.eq[3] = {1000.f, 6.f, 1.0f, ClientEq::FilterType::Peak, true, 12};
    SdrRx::Settings eqOff = eq;
    eq.eqOn = true;
    auto ue = run(Mode::USB, 0, 2800, eq, 2.0, [](double t) { return at(t, 1000, 0.01); });
    auto uo = run(Mode::USB, 0, 2800, eqOff, 2.0, [](double t) { return at(t, 1000, 0.01); });
    printf("  RX EQ +6 dB @ 1 kHz: tone %.1f dB (EQ off %.1f, none %.1f)\n",
           toneDb(ue, 1000), toneDb(uo, 1000), u1k);
    check(std::fabs(toneDb(ue, 1000) - u1k - 6.0) < 1.0, "RX EQ peak lifts the tone 6 dB");
    check(std::fabs(toneDb(uo, 1000) - u1k) < 0.5, "RX EQ off is a bypass");

    // RX dynamics. Fixed AGC is a straight gain, so the tone's amplitude
    // sets its level into the dynamics: 0.0001 -> about -14 dBFS there.
    const auto peakDb = [](const std::vector<float>& x) {
        float p = 0;
        for (float v : x) p = std::max(p, std::fabs(v));
        return 20 * std::log10(std::max(p, 1e-9f));
    };
    const auto tone = [](double amp) {
        return [amp](double t) { return at(t, 1000, amp); };
    };
    SdrRx::Settings loud = st;
    loud.volumeDb = 0;                              // +26 dBFS into the limiter
    auto lim = run(Mode::USB, 0, 2800, loud, 2.0, tone(0.01));
    printf("  final limiter: a +26 dBFS tone comes out at %.2f dBFS peak\n", peakDb(lim));
    check(peakDb(lim) <= SdrRx::kCeilingDb + 0.3, "final limiter holds the ceiling");
    SdrRx::Settings mu = st;
    mu.mute = true;
    auto muted = run(Mode::USB, 0, 2800, mu, 1.5, tone(0.01));
    check(!muted.empty() && peakDb(muted) < -150, "MUTE is silence");
    SdrRx::Settings bo = st;
    bo.boost = true;
    auto boosted = run(Mode::USB, 0, 2800, bo, 2.0, tone(0.01));
    printf("  BOOST: %.1f dB (plain %.1f)\n", toneDb(boosted, 1000), u1k);
    check(std::fabs(toneDb(boosted, 1000) - u1k - SdrRx::kBoostDb) < 0.5, "BOOST adds its 6 dB");

    SdrRx::Settings d = st;
    d.volumeDb = 0;
    const double hi0 = toneDb(run(Mode::USB, 0, 2800, d, 2.0, tone(0.0001)), 1000);
    const double lo0 = toneDb(run(Mode::USB, 0, 2800, d, 2.0, tone(0.00001)), 1000);
    SdrRx::Settings cp = d;
    cp.compOn = true;
    cp.compThrDb = -40;
    cp.compRatio = 2;
    cp.compMakeupDb = 0;
    const double hiC = toneDb(run(Mode::USB, 0, 2800, cp, 2.0, tone(0.0001)), 1000);
    const double loC = toneDb(run(Mode::USB, 0, 2800, cp, 2.0, tone(0.00001)), 1000);
    printf("  compressor 2:1 @ -40: step %.1f dB -> %.1f dB (loud %.1f, weak %.1f)\n",
           hi0 - lo0, hiC - loC, hi0, lo0);
    check(std::fabs((hi0 - lo0) - 20) < 1, "the test step is 20 dB uncompressed");
    check(hiC - loC < 13 && hiC - loC > 7, "2:1 compressor about halves it");
    SdrRx::Settings gt = d;
    gt.gateOn = true;
    gt.gateThrDb = -24;
    gt.gateFloorDb = -20;
    const double hiG = toneDb(run(Mode::USB, 0, 2800, gt, 2.0, tone(0.0001)), 1000);
    const double loG = toneDb(run(Mode::USB, 0, 2800, gt, 2.0, tone(0.00001)), 1000);
    printf("  gate @ -24, depth -20: loud %.1f (was %.1f), weak %.1f (was %.1f)\n",
           hiG, hi0, loG, lo0);
    check(std::fabs(hiG - hi0) < 0.5, "gate leaves a tone above threshold alone");
    check(lo0 - loG > 15, "gate turns a tone under threshold down by its depth");

    // NR2 on white noise in the passband: runs, and turns it down.
    unsigned seed = 7;
    auto noise = [&](double) {
        auto u = [&] { seed = seed * 1103515245u + 12345u; return ((seed >> 9) & 0x3fff) / 8192.0 - 1.0; };
        return std::complex<float>(float(0.002 * u()), float(0.002 * u()));
    };
    auto n0 = run(Mode::USB, 0, 2800, st, 3.0, noise);
    SdrRx::Settings nr2 = st;
    nr2.nr = SdrRx::NoiseReduction::NR2;
    seed = 7;
    auto n2 = run(Mode::USB, 0, 2800, nr2, 3.0, noise);
    printf("  NR2 on noise: %.1f dB -> %.1f dB\n", rmsDb(n0), rmsDb(n2));
    check(!n2.empty() && std::isfinite(rmsDb(n2)), "NR2 runs");
    check(rmsDb(n0) - rmsDb(n2) > 3, "NR2 lowers steady noise");

#ifdef HAVE_RADE
    {
        // The recording (48 kHz real audio) as a DSB signal on the dial:
        // USB demod hands back exactly the audio, which is RADE's modem.
        std::ifstream f(RADE_WAV_DIR "/FDV_offair.wav", std::ios::binary);
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<float> wav;
        for (size_t p = 12; p + 8 <= b.size();) {
            uint32_t len;
            std::memcpy(&len, &b[p + 4], 4);
            if (std::memcmp(&b[p], "data", 4) == 0) {
                const size_t n = std::min<size_t>(len, b.size() - p - 8) / 2;
                wav.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    int16_t v;
                    std::memcpy(&v, &b[p + 8 + 2 * i], 2);
                    wav[i] = v / 32768.f;
                }
                break;
            }
            p += 8 + len;
        }
        check(wav.size() > 48000 * 40, "RADE: off-air recording loaded");
        SdrRx::Settings rs = st;
        rs.agc = SdrRx::Agc::Medium;
        rs.volumeDb = 0;
        rs.rade = true;
        SdrRx::RadeStatus rst;
        auto speech = run(Mode::USB, 100, 2900, rs, 40.0, [&wav](double t) {
            const double x = t * 48000.0;
            const size_t i = size_t(x);
            const float a = i + 1 < wav.size()
                ? float(wav[i] + (wav[i + 1] - wav[i]) * (x - double(i))) : 0.f;
            return at(t, 0, 0.002 * a);            // real audio x carrier = DSB
        }, &rst);
        printf("  RADE via WDSP USB: sync %d, SNR %.1f dB, foff %.1f Hz, last second %.1f dBFS rms\n",
               int(rst.sync), rst.snrDb, rst.freqOffsetHz, rmsDb(speech));
        check(rst.available && rst.sync, "RADE syncs through the SDR chain");
        check(rmsDb(speech) > -45, "RADE speech comes out of the SDR chain");
    }
#endif

    if (fails == 0) printf("sdrrxtest: all checks passed\n");
    return fails ? 1 : 0;
}
