// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "rtty/Ita2.h"
#include <cstddef>
#include <deque>
#include <string>

namespace ttc {

// AFSK RTTY modulator: text in, continuous-phase two-tone audio out, for
// the SignaLink (the radio on LSB with mark 2125 / space 2295 by
// convention). 1 start bit, 5 data bits LSB first, 1.5 stop bits (the
// 45.45 baud standard; 2 at 75 baud for the receivers that want it).
// While keyed with nothing to say it sends LTRS ("diddle") so the far
// end's receiver stays in sync, or holds mark if diddle is off; the
// transmit side keeps PTT up until pending() says the last stop bit has
// gone out, then plays a short mark tail.
class RttyMod {
public:
    struct Params {
        double sampleRate = 48000.0;
        double markHz = 2125.0;
        double spaceHz = 2295.0;
        double baud = 45.45;
        double stopBits = 1.5;
        bool reverse = false;
        bool usos = true;
        bool diddle = true;
        float level = 0.5f;       // peak amplitude, -6 dBFS
    };
    RttyMod();
    explicit RttyMod(const Params& p);
    void setParams(const Params& p);
    const Params& params() const { return p_; }

    void queue(const std::string& text);   // append to the send queue
    void clear();                          // drop what has not started
    // Characters still to be keyed (not counting the one on the air).
    size_t pending() const { return codes_.size(); }
    // True while a character's bits are still going out.
    bool busy() const { return !bits_.empty() || !codes_.empty(); }

    // Render n samples: queued characters, then diddle or mark idle.
    void render(float* out, size_t n);
    // Render n samples of plain mark (the tail after the last character).
    void renderMark(float* out, size_t n);

private:
    void loadNext();                   // next code -> its bit list
    Params p_;
    std::deque<uint8_t> codes_;
    std::deque<std::pair<bool, double>> bits_;   // (mark?, samples)
    double phase_ = 0.0;
    double bitLeft_ = 0.0;             // samples left in the current bit
    bool curMark_ = true;
    bool figs_ = false;                // the sender's case (as keyed)
};

}  // namespace ttc
