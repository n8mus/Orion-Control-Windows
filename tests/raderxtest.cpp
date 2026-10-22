// SPDX-License-Identifier: GPL-3.0-or-later
// RADE receive on rade_c's own off-air recording (FDV_offair.wav: 48 kHz
// mono, real RADE V1 over HF), through RadeRx exactly as SdrRx feeds it:
//   cmake --build build --target raderxtest && ./build/raderxtest [wav]
// Expect: it syncs, speech comes out (48 kHz), at least one end-of-over
// callsign decodes, and pure noise never syncs.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "sdr/RadeRx.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

// Minimal PCM16 mono WAV reader (finds the "data" chunk).
static std::vector<float> readWav(const char* path, int& rate) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    rate = 0;
    std::vector<float> x;
    if (b.size() < 44) return x;
    std::memcpy(&rate, &b[24], 4);
    size_t p = 12;
    while (p + 8 <= b.size()) {
        uint32_t len;
        std::memcpy(&len, &b[p + 4], 4);
        if (std::memcmp(&b[p], "data", 4) == 0) {
            const size_t n = std::min<size_t>(len, b.size() - p - 8) / 2;
            x.resize(n);
            for (size_t i = 0; i < n; ++i) {
                int16_t s;
                std::memcpy(&s, &b[p + 8 + 2 * i], 2);
                x[i] = s / 32768.f;
            }
            break;
        }
        p += 8 + len;
    }
    return x;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : RADE_WAV_DIR "/FDV_offair.wav";
    int rate = 0;
    const std::vector<float> in = readWav(path, rate);
    std::printf("  %s: %.1f s at %d Hz\n", path, in.size() / double(std::max(rate, 1)), rate);
    check(rate == 48000 && in.size() > 48000 * 10, "off-air recording loaded (48 kHz)");

    RadeRx rx;
    check(rx.ok(), "rade_open");
    std::vector<float> out;
    int syncBlocks = 0, blocks = 0;
    float bestSnr = -99;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t o = 0; o + 480 <= in.size(); o += 480) {   // 10 ms blocks
        rx.process(&in[o], 480, out);
        ++blocks;
        if (rx.sync()) { ++syncBlocks; bestSnr = std::max(bestSnr, rx.snrDb()); }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double e = 0;
    for (float v : out) e += double(v) * v;
    const double rmsDb = 10 * std::log10(e / std::max<size_t>(1, out.size()) + 1e-20);
    std::printf("  in sync %.0f%% of the time, best SNR %.1f dB, %.1f s of speech at %.1f dBFS\n",
                100.0 * syncBlocks / std::max(blocks, 1), bestSnr, out.size() / 48000.0, rmsDb);
    std::printf("  callsigns: %d (last \"%s\"); decode took %.1f s for %.1f s of air (%.1f%% of a core)\n",
                rx.callsignCount(), rx.lastCallsign().c_str(), secs, in.size() / 48000.0,
                100.0 * secs / (in.size() / 48000.0));
    check(syncBlocks > blocks / 4, "syncs on real RADE");
    check(out.size() > 48000 * 10 && rmsDb > -40, "speech comes out");
    check(rx.callsignCount() >= 1, "an end-of-over callsign decodes");

    RadeRx nz;
    std::vector<float> noise(48000 * 20), junk;
    unsigned seed = 1;
    for (float& v : noise) { seed = seed * 1103515245u + 12345u; v = 0.1f * (((seed >> 9) & 0xffff) / 32768.f - 1.f); }
    int nsync = 0;
    for (size_t o = 0; o + 480 <= noise.size(); o += 480) {
        nz.process(&noise[o], 480, junk);
        nsync += nz.sync();
    }
    std::printf("  noise: %d of %zu blocks in sync, %zu samples out\n", nsync, noise.size() / 480, junk.size());
    check(nsync == 0 && junk.empty(), "noise never syncs, nothing comes out");

    if (fails == 0) std::printf("raderxtest: all checks passed\n");
    return fails ? 1 : 0;
}
