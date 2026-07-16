// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ttc {

// 0-BEAT final stage as a damped stepped servo. The first version applied
// the full measured pitch error in one throw off a single sample — which
// may have been measured before (or straddling) the previous dial move —
// so it rocked the dial in big alternating swings (live report: "I can
// see 400 then 650"). This policy replaces it:
//
//   coarse    : errors beyond 25 Hz walk in multiples of the radio's
//               10 Hz CW step, capped at 30 Hz per throw and damped to
//               ~0.6 of the error (operator's spec: "smaller ones") —
//               big errors carry big measurement uncertainty, the
//               staircase protects against chasing a bad number.
//   vernier   : once within 25 Hz the pitch meter is trustworthy
//               (~±1-2 Hz), so ONE exact 1 Hz-resolution throw of the
//               full remaining error lands the note (operator's spec:
//               "reads 532 -> tune 18 and to 550 would be amazing").
//               CAT tuning is 1 Hz-capable — the 10 Hz step is only the
//               front-panel knob's granularity.
//   evidence  : a move needs TWO consecutive measurements agreeing within
//               8 Hz, both taken after a settle blackout past the last
//               move (the pitch FFT window + parec latency mean samples
//               just after a retune still describe the OLD note).
//   gives up  : no tone for 3 s -> stop trying (station stopped, possibly
//               the instant the zap was hit); estimate >120 Hz off or
//               budget exhausted (6 moves / 120 Hz total / 15 s) -> stop.
//
// Pure logic with injected time, like AutoGain — trimtest drives it
// against a simulated station (lag, noise, keying gaps, mid-trim death).
// The owner feeds every pitchMeasured() emission (invalid ticks included)
// and applies Move deltas to the dial with the sideband's sign.
class PitchTrim {
public:
    struct Decision {
        enum What { None, Move, Done, Fail };
        What what = None;
        int  deltaHz = 0;      // Move: positive = note is high, lower it
        const char* why = "";
    };

    void arm(double targetHz, int64_t nowMs) {
        target_ = targetHz;
        armMs_ = moveMs_ = lastValidMs_ = nowMs;
        moves_ = 0;
        totalHz_ = 0;
        pendHz_ = -1.0;
        active_ = true;
    }

    void cancel() { active_ = false; }
    bool active() const { return active_; }

    Decision feed(double pitchHz, int64_t nowMs) {
        if (!active_) return {};
        if (nowMs - armMs_ > kMaxMs) return fail("timed out");
        if (pitchHz <= 0.0) {
            if (nowMs - lastValidMs_ > kNoSigMs) return fail("no signal");
            return {};
        }
        lastValidMs_ = nowMs;
        if (nowMs - moveMs_ < kSettleMs) {
            pendHz_ = -1.0;                // stale audio: restart the pair
            return {};
        }
        if (pendHz_ < 0.0 || std::abs(pitchHz - pendHz_) > kPairHz) {
            pendHz_ = pitchHz;             // first look, or disagreement:
            return {};                     // slide the window forward
        }
        const double est = 0.5 * (pendHz_ + pitchHz);
        pendHz_ = -1.0;
        const double err = est - target_;
        if (std::abs(err) <= kTolHz) {
            active_ = false;
            return {Decision::Done, 0, "on pitch"};
        }
        if (std::abs(err) > kMaxErrHz) return fail("not our note");
        if (moves_ >= kMaxMoves || totalHz_ >= kMaxTotalHz)
            return fail("walked too far");
        int mag;
        if (std::abs(err) <= kVernierHz) {        // exact final throw, 1 Hz res
            mag = int(std::lround(std::abs(err)));
        } else {
            mag = int(std::lround(std::abs(err) * kGain / kStepHz)) * kStepHz;
            mag = std::clamp(mag, kStepHz, kMaxStepHz);
        }
        ++moves_;
        totalHz_ += mag;
        moveMs_ = nowMs;
        return {Decision::Move, err > 0 ? mag : -mag, ""};
    }

    static constexpr double  kGain      = 0.6;
    static constexpr double  kTolHz     = 3.0;    // vernier lands within noise
    static constexpr double  kVernierHz = 25.0;   // below this: one exact throw
    static constexpr double  kPairHz    = 8.0;
    static constexpr double  kMaxErrHz  = 120.0;  // beyond this: different note
    static constexpr int     kStepHz    = 10;     // the radio's CW tune step
    static constexpr int     kMaxStepHz = 30;
    static constexpr int     kMaxMoves  = 6;
    static constexpr int     kMaxTotalHz = 120;
    static constexpr int64_t kSettleMs  = 700;
    static constexpr int64_t kNoSigMs   = 3000;
    static constexpr int64_t kMaxMs     = 15000;

private:
    Decision fail(const char* why) {
        active_ = false;
        return {Decision::Fail, 0, why};
    }

    bool    active_ = false;
    double  target_ = 550.0;
    double  pendHz_ = -1.0;
    int     moves_ = 0;
    int     totalHz_ = 0;
    int64_t armMs_ = 0;
    int64_t moveMs_ = 0;
    int64_t lastValidMs_ = 0;
};

} // namespace ttc
