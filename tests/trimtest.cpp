// SPDX-License-Identifier: GPL-2.0-or-later
// PitchTrim servo driven against a simulated station: measurements arrive
// at the real 4 Hz cadence with lag (the pitch FFT + parec latency mean a
// sample can describe the note from ~400 ms ago), gaussian noise, keying
// gaps, and stations that die — including the instant the zap is hit.
// Asserts the operator's spec: small quantized moves, converging staircase
// (no 400<->650 rocking), and STOP when there is no signal.
//   cmake --build build --target trimtest && ./build/trimtest
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "dsp/PitchTrim.h"

using ttc::PitchTrim;

static int fails = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++fails;
    }
}

namespace {
constexpr double kTarget = 550.0;
constexpr int64_t kCadenceMs = 250;   // AudioCwSource pitch emission rate
constexpr int64_t kLagMs = 400;       // sample describes the note this old

struct Run {
    double finalErr = 0.0;
    int moves = 0;
    int maxStep = 0;
    int signFlips = 0;
    bool done = false, failed = false;
    const char* why = "";
    int64_t endMs = 0;
    std::vector<int> deltas;
};

// Drive one station through the servo. pitch(t) = target + err0 minus the
// moves applied so far (as of t - lag). aliveUntil / deadFrom carve out
// when the station is keying; gapEvery/gapLen simulate word gaps.
Run drive(double err0, double noiseHz, int64_t aliveFromMs, int64_t deadFromMs,
          int64_t gapEveryMs = 0, int64_t gapLenMs = 0, unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, noiseHz);
    std::vector<std::pair<int64_t, int>> moves;   // time, cumulative shift
    PitchTrim trim;
    trim.arm(kTarget, 0);
    Run r;
    int lastSign = 0, cum = 0;
    for (int64_t t = kCadenceMs; t < 30000; t += kCadenceMs) {
        double hz = -1.0;
        const bool alive = t >= aliveFromMs && t < deadFromMs
            && !(gapEveryMs > 0 && (t % gapEveryMs) < gapLenMs);
        if (alive) {
            int shiftedBy = 0;                    // world as of t - lag
            for (const auto& m : moves)
                if (m.first <= t - kLagMs) shiftedBy = m.second;
            hz = kTarget + err0 - shiftedBy + noise(rng);
        }
        const auto d = trim.feed(hz, t);
        if (d.what == PitchTrim::Decision::Move) {
            ++r.moves;
            r.deltas.push_back(d.deltaHz);
            r.maxStep = std::max(r.maxStep, std::abs(d.deltaHz));
            const int s = d.deltaHz > 0 ? 1 : -1;
            if (lastSign != 0 && s != lastSign) ++r.signFlips;
            lastSign = s;
            cum += d.deltaHz;
            moves.emplace_back(t, cum);
            check(std::abs(d.deltaHz) <= PitchTrim::kMaxStepHz,
                  "move never exceeds the 30 Hz cap");
            if (std::abs(d.deltaHz) > PitchTrim::kVernierHz)
                check(d.deltaHz % PitchTrim::kStepHz == 0,
                      "coarse moves are 10 Hz multiples");
        } else if (d.what == PitchTrim::Decision::Done) {
            r.done = true;
            r.endMs = t;
            break;
        } else if (d.what == PitchTrim::Decision::Fail) {
            r.failed = true;
            r.why = d.why;
            r.endMs = t;
            break;
        }
    }
    r.finalErr = err0 - cum;
    return r;
}
} // namespace

int main() {
    {   // High note, big error: coarse staircase hands off to the vernier.
        const Run r = drive(+85.0, 1.5, 0, 30000);
        check(r.done, "err +85: reaches Done");
        check(std::abs(r.finalErr) <= 4.0, "err +85: lands within tolerance");
        check(r.maxStep <= PitchTrim::kMaxStepHz, "err +85: steps capped at 30");
        check(r.signFlips == 0, "err +85: staircase never reverses");
    }
    {   // The OM2VL case: reads 532 for a 550 target (18 Hz low) — ONE
        // exact vernier throw, dead on (operator's spec).
        const Run r = drive(-18.0, 1.0, 0, 30000);
        check(r.done && r.moves == 1, "err -18: one vernier move");
        check(!r.deltas.empty() && r.deltas[0] <= -16 && r.deltas[0] >= -20,
              "err -18: the move IS the error");
        check(std::abs(r.finalErr) <= 3.0, "err -18: lands dead on");
    }
    {   // Low note, moderate error, mirrored.
        const Run r = drive(-47.0, 1.5, 0, 30000);
        check(r.done && std::abs(r.finalErr) <= 4.0, "err -47: converges");
        check(r.signFlips == 0, "err -47: no rocking");
    }
    {   // Noisy measurement: still converges, at most one grid-hunt flip.
        const Run r = drive(+62.0, 3.5, 0, 30000, 0, 0, 42);
        check(r.done && std::abs(r.finalErr) <= 8.0, "noisy: converges");
        check(r.signFlips <= 1, "noisy: no oscillation");
    }
    {   // Keying gaps (600 ms silence every 2 s): gaps stall, never abort.
        const Run r = drive(+38.0, 1.5, 0, 30000, 2000, 600);
        check(r.done && std::abs(r.finalErr) <= 6.0, "keying gaps: converges");
    }
    {   // Station stops the instant the zap lands: zero moves, quiet stop.
        const Run r = drive(+50.0, 1.5, 30000, 30000);
        check(r.failed && std::string_view(r.why) == "no signal",
              "dead at arm: gives up as no-signal");
        check(r.moves == 0, "dead at arm: never touches the dial");
        check(r.endMs <= 4000, "dead at arm: gives up promptly");
    }
    {   // Station dies mid-trim: stops within the no-signal window.
        const Run r = drive(+85.0, 1.5, 0, 2600);
        check(r.failed && std::string_view(r.why) == "no signal",
              "dies mid-trim: stops trying");
        check(r.endMs <= 2600 + 3300, "dies mid-trim: stops within ~3 s");
    }
    {   // Comes back after a 2 s pause (inside the window): finishes the job.
        const Run r = drive(+44.0, 1.5, 2100, 30000);
        check(r.done && std::abs(r.finalErr) <= 6.0, "late starter: converges");
    }
    {   // Already on the note: no dial motion at all.
        const Run r = drive(+2.0, 1.0, 0, 30000);
        check(r.done && r.moves == 0, "on pitch already: zero moves");
    }
    {   // A different station's note (way off): refuse to chase it.
        const Run r = drive(+300.0, 1.5, 0, 30000);
        check(r.failed && r.moves == 0, "300 Hz off: not our note, no moves");
    }
    printf(fails ? "TRIMTEST: %d FAILURE(S)\n" : "TRIMTEST: PASS\n", fails);
    return fails ? 1 : 0;
}
