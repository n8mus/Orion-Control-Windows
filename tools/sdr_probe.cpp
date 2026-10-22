// SPDX-License-Identifier: GPL-2.0-or-later
// Headless RSP2 streaming smoke test: opens the SDRplay device, streams for a few
// seconds, and reports the achieved sample rate plus mean/peak power (dBFS). Proves
// the IQ path end-to-end without needing the GUI. Built only with -DBUILD_SDRPLAY=ON.
//
//   ./sdr_probe [centerHz] [sampleRate] [seconds]
//   ./sdr_probe 7150000 2000000 2
#include "sdr/SdrPlaySource.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <thread>
#include <chrono>

using namespace ttc;

int main(int argc, char** argv) {
    const double center = argc > 1 ? std::atof(argv[1]) : 7150000.0;
    const double fs     = argc > 2 ? std::atof(argv[2]) : 2000000.0;
    const double secs   = argc > 3 ? std::atof(argv[3]) : 2.0;
    const int    gRdB   = argc > 4 ? std::atoi(argv[4]) : 40;
    const int    lna    = argc > 5 ? std::atoi(argv[5]) : 0;

    SdrPlaySource src;
    if (!src.apiOk()) { std::printf("API not open: %s\n", src.lastError().c_str()); return 1; }
    src.setGain(gRdB, lna);

    std::mutex m;
    uint64_t count = 0;
    double sumPow = 0.0, peak = 0.0;

    src.setIqCallback([&](const IqBlock& b) {
        double s = 0.0, p = 0.0;
        for (const auto& c : b) {
            const double mag2 = double(c.real()) * c.real() + double(c.imag()) * c.imag();
            s += mag2; if (mag2 > p) p = mag2;
        }
        std::lock_guard<std::mutex> lk(m);
        count += b.size(); sumPow += s; if (p > peak) peak = p;
    });

    if (!src.start(center, fs)) { std::printf("start failed: %s\n", src.lastError().c_str()); return 1; }
    std::this_thread::sleep_for(std::chrono::milliseconds((long)(secs * 1000)));
    src.stop();

    const double meanPow = count ? sumPow / count : 0.0;
    std::printf("\n--- RSP2 streaming result ---\n");
    std::printf("center      : %.4f MHz   requested fs: %.0f S/s\n", center / 1e6, fs);
    std::printf("samples     : %llu in %.1fs  =>  %.3f MS/s achieved\n",
                (unsigned long long)count, secs, count / secs / 1e6);
    std::printf("mean power  : %.1f dBFS\n", 10.0 * std::log10(meanPow + 1e-20));
    std::printf("peak power  : %.1f dBFS\n", 10.0 * std::log10(peak + 1e-20));
    std::printf("%s\n", count > 0 ? "OK: IQ is flowing." : "NO SAMPLES received.");
    return count > 0 ? 0 : 2;
}
