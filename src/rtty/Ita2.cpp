// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/Ita2.h"
#include <cctype>

namespace ttc::ita2 {

namespace {
// Index = the 5-bit code, LSB first on the wire. '\0' = unprintable.
const char kLetters[32] = {
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r', 'D', 'R',  'J', 'N', 'F', 'C', 'K',
    'T',  'Z', 'L',  'W', 'H', 'Y', 'P', 'Q',
    'O',  'B', 'G',  '\0', 'M', 'X', 'V', '\0',
};
const char kFigures[32] = {
    '\0', '3', '\n', '-', ' ', '\a', '8', '7',
    '\r', '$', '4',  '\'', ',', '!', ':', '(',
    '5',  '"', ')',  '2', '#', '6', '0', '1',
    '9',  '?', '&',  '\0', '.', '/', ';', '\0',
};
}  // namespace

char letter(uint8_t code) { return code < 32 ? kLetters[code] : '\0'; }
char figure(uint8_t code) { return code < 32 ? kFigures[code] : '\0'; }

Enc encode(char c) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
    if (c == ' ')  return {kSpace, false, true};
    if (c == '\n') return {kLf, false, true};
    if (c == '\r') return {kCr, false, true};
    for (uint8_t i = 1; i < 32; ++i)
        if (kLetters[i] == c && i != kLtrs && i != kFigs) return {i, false, true};
    for (uint8_t i = 1; i < 32; ++i)
        if (kFigures[i] == c && i != kLtrs && i != kFigs) return {i, true, true};
    return {0, false, false};
}

std::vector<uint8_t> encodeText(const std::string& text, bool usos) {
    std::vector<uint8_t> out;
    out.reserve(text.size() + 8);
    bool figs = false;
    bool first = true;
    for (char c : text) {
        const Enc e = encode(c);
        if (!e.ok) continue;
        const bool shared = (e.code == kSpace || e.code == kCr || e.code == kLf);
        if (!shared && (first || e.figs != figs)) {
            out.push_back(e.figs ? kFigs : kLtrs);
            figs = e.figs;
            first = false;
        }
        out.push_back(e.code);
        // A space unshifts the listener (USOS); say FIGS again before the
        // next figure so both kinds of receiver read it right.
        if (usos && e.code == kSpace && figs) figs = false;
    }
    return out;
}

char Decoder::feed(uint8_t code) {
    code &= 0x1F;
    if (code == kLtrs) { figs_ = false; return '\0'; }
    if (code == kFigs) { figs_ = true;  return '\0'; }
    const char c = figs_ ? kFigures[code] : kLetters[code];
    if (code == kSpace && usos_) figs_ = false;
    return c;
}

}  // namespace ttc::ita2
