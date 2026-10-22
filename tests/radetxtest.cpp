// SPDX-License-Identifier: GPL-3.0-or-later
// RADE transmit, offline loopback — nothing near a radio:
//   cmake --build build --target radetxtest && ./build/radetxtest
// G8SEZ's speech (rade_c's input_sample.wav, 16 kHz) at 48 kHz through
// RadeTx, the end of over carrying "N8EM", into RadeRx. Checks: RX syncs
// and speaks, the callsign decodes, the modem fits an SSB TX passband with
// peaks near -6 dBFS and no clipping, and the over's tail is 200 ms of
// silence before PTT may drop.
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "sdr/RadeRx.h"
#include "sdr/RadeTx.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static std::vector<float> readWav16(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    std::vector<float> x;
    for (size_t p = 12; p + 8 <= b.size();) {
        uint32_t len;
        std::memcpy(&len, &b[p + 4], 4);
        if (std::memcmp(&b[p], "data", 4) == 0) {
            for (size_t i = 0; i + 1 < len && p + 9 + i < b.size(); i += 2) {
                int16_t v;
                std::memcpy(&v, &b[p + 8 + i], 2);
                x.push_back(v / 32768.f);
            }
            break;
        }
        p += 8 + len;
    }
    return x;
}

// Fraction of power in [lo, hi] Hz (48 kHz, Hann-windowed DFT probed
// every 10 Hz — unwindowed, the in-band carriers' leakage alone read as
// 4% out of band).
static double inBand(const std::vector<float>& x, double lo, double hi) {
    double in = 0, all = 0;
    const size_t N = std::min<size_t>(x.size(), 48000 * 4);
    for (double f = 50; f < 6000; f += 10) {
        std::complex<double> acc = 0;
        for (size_t i = 0; i < N; i += 1)
            acc += double(x[i]) * (0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(N - 1)))
                 * std::polar(1.0, -2 * M_PI * f * double(i) / 48000.0);
        const double p = std::norm(acc);
        all += p;
        if (f >= lo && f <= hi) in += p;
    }
    return in / all;
}

int main() {
    const std::vector<float> sp16 = readWav16(RADE_WAV_DIR "/input_sample.wav");
    check(sp16.size() > 16000 * 5, "speech loaded");
    std::vector<float> mic;                        // 48 kHz, linear interp x3
    for (int rep = 0; rep < 2; ++rep)
        for (size_t i = 0; i + 1 < sp16.size(); ++i)
            for (int k = 0; k < 3; ++k) mic.push_back(sp16[i] + (sp16[i + 1] - sp16[i]) * k / 3.f);

    RadeTx tx("N8EM");
    check(tx.ok(), "rade_open (tx)");
    std::vector<float> modem;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o + 480 <= mic.size(); o += 480) tx.process(&mic[o], 480, modem);
    const size_t speechEnd = modem.size();
    tx.endOver(modem);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    float pk = 0;
    for (float v : modem) pk = std::max(pk, std::fabs(v));
    // The standard V1 waveform: rade_c ships its V1 TX bandpass OFF (as
    // freedv-gui sends it), so the OFDM sidelobes are everyone's; what
    // counts is that it fits an SSB TX passband (measured 98.6% in
    // 300-3000 Hz, 95.8% in 600-2400) — the radio's TX filter does the rest.
    const double frac = inBand(std::vector<float>(modem.begin() + 48000, modem.end()), 300, 3000);
    float tailPk = 0;
    for (size_t i = modem.size() - 9600; i < modem.size(); ++i) tailPk = std::max(tailPk, std::fabs(modem[i]));
    std::printf("  modem: %.1f s for %.1f s of speech (%.1f%% of a core), peak %.1f dBFS, "
                "%.1f%% of power in 300-3000 Hz\n",
                modem.size() / 48000.0, mic.size() / 48000.0, 100 * secs / (mic.size() / 48000.0),
                20 * std::log10(pk), 100 * frac);
    check(pk < 0.9f && pk > 0.2f, "peaks near -6 dBFS, never clipping");
    check(frac > 0.98, "fits an SSB transmit passband (300-3000 Hz)");
    check(tailPk == 0.f && modem.size() - speechEnd > 9600, "EOO frame, then 200 ms of silence");

    RadeRx rx;
    std::vector<float> lead(48000, 0.f), speech;
    rx.process(lead.data(), int(lead.size()), speech);
    int syncBlocks = 0, blocks = 0;
    for (size_t o = 0; o + 480 <= modem.size(); o += 480) {
        rx.process(&modem[o], 480, speech);
        ++blocks;
        syncBlocks += rx.sync();
    }
    rx.process(lead.data(), int(lead.size()), speech);
    double e = 0;
    for (float v : speech) e += double(v) * v;
    std::printf("  loopback: in sync %.0f%%, SNR %.0f dB, %.1f s of speech at %.1f dBFS rms, "
                "callsign \"%s\" (%d)\n",
                100.0 * syncBlocks / std::max(blocks, 1), rx.snrDb(), speech.size() / 48000.0,
                10 * std::log10(e / std::max<size_t>(speech.size(), 1) + 1e-20),
                rx.lastCallsign().c_str(), rx.callsignCount());
    check(syncBlocks > blocks / 2, "the receiver syncs on our modem");
    check(speech.size() > 48000 * 10, "speech comes back out");
    check(rx.lastCallsign() == "N8EM", "end of over carries N8EM");

    if (fails == 0) std::printf("radetxtest: all checks passed\n");
    return fails ? 1 : 0;
}
