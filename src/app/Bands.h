// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <iterator>
#include "radio/RadioController.h"

namespace ttc {

// Band stack registers, Orion-style: 4 per band (A-D). Pressing a band button
// recalls that band's last-used register; pressing it again cycles A->B->C->D.
// Each register stores frequency + mode + filter (bw/pbt) and is persisted in
// QSettings once used; the seeds below follow Jon's scheme:
//   A = CW segment, CW mode, 500 Hz filter
//   B = FT8 frequency, USB, 3 kHz, PBT 0
//   C = SSB low in the Extra phone segment, 3 kHz, sideband per band
//   D = SSB higher in the General phone segment, 3 kHz
struct StackDef {
    uint64_t hz;
    Mode     mode;
    int      bwHz;
    int      pbtHz;
};

struct BandDef {
    const char* label;      // button text
    uint64_t loHz, hiHz;    // band edges (used to highlight the current band)
    StackDef stack[4];      // A, B, C, D seeds
};

inline constexpr int kStackCount = 4;
inline constexpr char kStackNames[kStackCount] = {'A', 'B', 'C', 'D'};

inline constexpr BandDef kBands[] = {
    {"160", 1800000, 2000000,
     {{1825000, Mode::CWU, 500, 0},  {1840000, Mode::USB, 3000, 0},
      {1850000, Mode::LSB, 3000, 0}, {1900000, Mode::LSB, 3000, 0}}},
    {"80",  3500000, 4000000,
     {{3525000, Mode::CWU, 500, 0},  {3573000, Mode::USB, 3000, 0},
      {3610000, Mode::LSB, 3000, 0}, {3850000, Mode::LSB, 3000, 0}}},
    {"60",  5250000, 5450000,       // channelized + locked: these seeds are
                                    // bypassed — recall60m uses kUs60mChans
     {{5357000, Mode::USB, 3000, 0}, {5357000, Mode::USB, 3000, 0},
      {5357000, Mode::USB, 3000, 0}, {5357000, Mode::USB, 3000, 0}}},
    {"40",  7000000, 7300000,
     {{7025000, Mode::CWU, 500, 0},  {7074000, Mode::USB, 3000, 0},
      {7130000, Mode::LSB, 3000, 0}, {7200000, Mode::LSB, 3000, 0}}},
    {"30",  10100000, 10150000,     // CW/data only
     {{10110000, Mode::CWU, 500, 0}, {10136000, Mode::USB, 3000, 0},
      {10120000, Mode::CWU, 500, 0}, {10130000, Mode::CWU, 500, 0}}},
    {"20",  14000000, 14350000,
     {{14025000, Mode::CWU, 500, 0},  {14074000, Mode::USB, 3000, 0},
      {14160000, Mode::USB, 3000, 0}, {14250000, Mode::USB, 3000, 0}}},
    {"17",  18068000, 18168000,     // no Extra/General phone split
     {{18080000, Mode::CWU, 500, 0},  {18100000, Mode::USB, 3000, 0},
      {18120000, Mode::USB, 3000, 0}, {18140000, Mode::USB, 3000, 0}}},
    {"15",  21000000, 21450000,
     {{21025000, Mode::CWU, 500, 0},  {21074000, Mode::USB, 3000, 0},
      {21210000, Mode::USB, 3000, 0}, {21300000, Mode::USB, 3000, 0}}},
    {"12",  24890000, 24990000,
     {{24900000, Mode::CWU, 500, 0},  {24915000, Mode::USB, 3000, 0},
      {24940000, Mode::USB, 3000, 0}, {24960000, Mode::USB, 3000, 0}}},
    {"10",  28000000, 29700000,
     {{28025000, Mode::CWU, 500, 0},  {28074000, Mode::USB, 3000, 0},
      {28320000, Mode::USB, 3000, 0}, {28400000, Mode::USB, 3000, 0}}},
    {"6",   50000000, 54000000,
     {{50090000, Mode::CWU, 500, 0},  {50313000, Mode::USB, 3000, 0},
      {50125000, Mode::USB, 3000, 0}, {50200000, Mode::USB, 3000, 0}}},
};
inline constexpr int kBandCount = static_cast<int>(std::size(kBands));

// US 60 m runs channelized and LOCKED: pressing the 60 button cycles these
// five hard-coded channels; dial moves never stamp them and QSettings never
// overrides them (see MainWindow::recall60m). Modes follow the on-air
// conventions — CH3 (5357.0 dial) is the worldwide FT8 channel, now inside
// the FCC 25-60 contiguous band so 15 W EIRP applies there; the four 100 W
// ERP channels are USB voice, CH5 being the international DX channel. Each
// entry carries its acceptable transmit profile (power, TX filter, speech
// processor, mic vs line-in).
struct Chan60 {
    uint64_t dialHz;     // USB suppressed-carrier dial (center - 1.5 kHz)
    Mode     mode;
    int      bwHz;       // RX filter
    const char* name;    // stack readout + status text
    int      txPwrPct;   // -- transmit profile --
    int      txBwHz;
    int      procLvl;
    bool     digital;    // line-in (FT8) instead of mic
};
inline constexpr Chan60 kUs60mChans[] = {
    {5330500, Mode::USB, 2800, "CH1 voice", 100, 2800, 3, false},
    {5346500, Mode::USB, 2800, "CH2 voice", 100, 2800, 3, false},
    {5357000, Mode::USB, 3000, "CH3 FT8",    15, 3000, 0, true},
    {5371500, Mode::USB, 2800, "CH4 voice", 100, 2800, 3, false},
    {5403500, Mode::USB, 2800, "CH5 DX",    100, 2800, 3, false},
};
inline constexpr int kChan60Count = static_cast<int>(std::size(kUs60mChans));

inline bool is60m(int bandIdx) {
    return bandIdx >= 0 && bandIdx < kBandCount
           && kBands[bandIdx].label[0] == '6' && kBands[bandIdx].label[1] == '0';
}

inline int bandIndexOf(uint64_t hz) {
    for (int i = 0; i < kBandCount; ++i)
        if (hz >= kBands[i].loHz && hz <= kBands[i].hiHz) return i;
    return -1;
}

// ---- Mode follows the band plan -------------------------------------------
// The dial's segment decides the mode: CW windows are CWU (operator's rule:
// CW is always upper), everything else is USB above 10 MHz, and below 10 MHz
// the FT8/data islands are USB with the remaining phone segment LSB. Only
// console-commanded tunes consult this (see tuneAbsolute) — dial-follow from
// the radio's own knob never does, so the radio can always overrule.
struct PlanSeg { uint64_t lo, hi; };
inline constexpr PlanSeg kPlanCw[] = {          // same windows the skimmer uses
    {1800000, 1840000},  {3500000, 3570000},  {7000000, 7070000},
    {10100000, 10130000},{14000000, 14070000},{18068000, 18095000},
    {21000000, 21070000},{24890000, 24912000},{28000000, 28070000},
    {50000000, 50100000}};
inline constexpr PlanSeg kPlanDataLow[] = {     // sub-10 MHz USB data islands
    {1840000, 1850000},  {3570000, 3600000},  {7070000, 7125000}};

// known=false: out of band, or 60 m (channel logic owns mode there).
inline Mode planModeForFreq(uint64_t hz, bool& known) {
    known = false;
    const int band = bandIndexOf(hz);
    if (band < 0 || is60m(band)) return Mode::USB;
    for (const PlanSeg& s : kPlanCw)
        if (hz >= s.lo && hz < s.hi) { known = true; return Mode::CWU; }
    known = true;
    if (hz >= 29520000 && hz <= 29700000) return Mode::FM;  // 10 m repeaters
    if (hz >= 10000000) return Mode::USB;
    for (const PlanSeg& s : kPlanDataLow)
        if (hz >= s.lo && hz < s.hi) return Mode::USB;
    return Mode::LSB;
}

} // namespace ttc
