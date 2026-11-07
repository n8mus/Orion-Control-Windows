// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rtty/Ita2.h"
#include <complex>
#include <cstddef>
#include <functional>
#include <vector>

namespace ttc {

// RTTY (FSK/AFSK) demodulator. One instance = one channel, fed either
//   - real audio (the Orion's ear via the SignaLink): mark/space at the
//     audio tones, 2125/2295 Hz on LSB by convention, or
//   - complex IQ already mixed so the channel sits near 0 Hz (the SDR
//     skimmer's business): mark/space at +-shift/2.
// Both go through process(): a real sample is complex with imag 0, and
// the mixers' negative-frequency image lands 4 kHz away, outside the
// matched filter. No Qt here so the test can drive it bare and a bank of
// them stays cheap.
//
// Chain: two recurrence-phasor mixers (mark, space) -> 1 ms boxcar ->
// decimate to 1 kHz -> one-bit boxcar per tone (the non-coherent FSK
// matched filter) -> magnitudes -> per-tone peak normalisation (fldigi's
// ATC idea: a selective fade on one tone must not pull every decision
// its way) -> UART bit sync (start edge, sample at bit centres, stop bit
// checked) -> ITA2 with USOS. A noise-floor squelch keeps a dead channel
// quiet: nothing prints unless a tone stands above the floor.
class RttyDemod {
public:
    struct Params {
        double sampleRate = 48000.0;
        double markHz = 2125.0;
        double spaceHz = 2295.0;
        double baud = 45.45;
        bool reverse = false;     // swap the tones (the other sideband)
        bool usos = true;         // unshift on space
        // Squelch on tone CONTRAST, |mark-space|/(mark+space) averaged
        // over ~1 s: keying reads 0.6-1.0 (one tone on, the other off),
        // noise alone ~0.4 whatever its level. Opens above `squelch`,
        // closes 0.05 under it; 0 = always open.
        double squelch = 0.5;
    };
    RttyDemod();
    explicit RttyDemod(const Params& p);
    void setParams(const Params& p);
    const Params& params() const { return p_; }
    void reset();

    void process(const std::complex<float>* s, size_t n);
    void processReal(const float* s, size_t n);

    // Decoded characters, on the calling thread, one at a time ('\n' for
    // LF; CR and shifts are swallowed).
    std::function<void(char)> onChar;

    // For a tuning indicator / status line, updated once per decimated
    // sample: normalised tone levels (0..1 of their recent peaks), the
    // signal's peak over the noise floor in dB, the keying contrast the
    // squelch judges by, and whether it is open.
    struct Status { float mark = 0, space = 0, snrDb = 0, quality = 0; bool open = false; };
    Status status() const { return st_; }
    unsigned framingErrors() const { return frameErr_; }
    unsigned charsDecoded() const { return chars_; }

private:
    void decimated(float m, float s);   // one 1 kHz sample of each tone
    void bit(bool mark);                // one 1 kHz hard decision

    Params p_;
    // Mixers (recurrence phasors) + 1 ms accumulators.
    std::complex<double> phM_{1, 0}, phS_{1, 0}, rotM_{1, 0}, rotS_{1, 0};
    std::complex<double> accM_{0, 0}, accS_{0, 0};
    int decim_ = 48, accN_ = 0;
    // One-bit boxcars at 1 kHz.
    std::vector<std::complex<double>> ringM_, ringS_;
    std::complex<double> sumM_{0, 0}, sumS_{0, 0};
    size_t ringPos_ = 0;
    // Peak trackers + noise floor.
    double pkM_ = 1e-3, pkS_ = 1e-3, floor_ = 1e-3;
    double quality_ = 0.0;              // contrast EMA
    unsigned warm_ = 0;                 // decimated samples since reset
    // UART.
    double bitLen_ = 22.0;              // decimated samples per bit
    enum class Uart { Idle, Start, Data };
    Uart uart_ = Uart::Idle;
    double t_ = 0;                      // samples since the start edge
    int dataBits_ = 0;
    uint8_t code_ = 0;
    int lastBit_ = 1;
    ita2::Decoder ita_;
    Status st_;
    unsigned frameErr_ = 0, chars_ = 0;
};

}  // namespace ttc
