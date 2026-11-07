// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ttc {

// ITA2 (Baudot) — the 5-bit alphabet of amateur RTTY. Two shift cases,
// LTRS and FIGS, each selected by its own code; the US figures case (' for
// 0x0B, $ for 0x09, # for 0x14... the "US TTY" variant fldigi and MMTTY
// default to) because that is what the stations he works send.
//
// Pure tables, no Qt: the modulator, the demodulator and the test share
// them.
namespace ita2 {

constexpr uint8_t kNull = 0x00;
constexpr uint8_t kLtrs = 0x1F;
constexpr uint8_t kFigs = 0x1B;
constexpr uint8_t kSpace = 0x04;
constexpr uint8_t kCr = 0x08;
constexpr uint8_t kLf = 0x02;

// code (0..31) -> character in each case; 0 = nothing printable (shift,
// null, CR handled by the caller).
char letter(uint8_t code);
char figure(uint8_t code);

// A printable character -> (code, needsFigs). Unknown characters map to
// code 0 with ok=false. Lower case is folded to upper.
struct Enc { uint8_t code; bool figs; bool ok; };
Enc encode(char c);

// Text -> the codes a sender keys, shifts inserted where the case changes
// (and re-asserted after a space when usos is on, matching receivers that
// unshift on space). Starts in LTRS; a leading shift is always sent.
std::vector<uint8_t> encodeText(const std::string& text, bool usos);

// A receiver's case tracker: feed codes, get characters ('\0' = nothing).
class Decoder {
public:
    explicit Decoder(bool usos = true) : usos_(usos) {}
    void setUsos(bool on) { usos_ = on; }
    void reset() { figs_ = false; }
    char feed(uint8_t code);
    bool figs() const { return figs_; }
private:
    bool usos_;
    bool figs_ = false;
};

}  // namespace ita2
}  // namespace ttc
