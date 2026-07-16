// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QtGlobal>

namespace ttc {

// Auto LNA policy: keep the wideband ADC peaks in the sweet spot the
// operator found by hand (LNA 6 was 15 dB deaf; LNA 2 clipped and bred
// mutant skimmer calls; LNA 3 at night on 40 m = 2-6 dB headroom and
// three calls a minute). Pure logic with injected time so the state
// machine is unit-testable; the owner samples the IQ peak ~1/s and
// applies the returned steps to the hardware.
//
//   attack : peaks hotter than -4 dBFS -> one step MORE attenuation,
//            fast (clipping must never persist).
//   release: peaks colder than -16 dBFS for a sustained 30 s -> one
//            step LESS attenuation, slow (band pauses must not pump).
//   lockout: after an attack, releases won't go back below the level
//            that clipped for 10 minutes (no ping-pong when one loud
//            broadcaster fades in and out).
//   floor  : releases never go below minLna_ (default 3, override via
//            setMinLna / the sdr/minLna setting). History, one day long:
//            the floor was added when a quiet band released the LNA to 0
//            and an RFI comb rose across the span; after the USB hubs
//            behind most of the RFI were removed and 20 m checked clean
//            at LNA 0, the floor was dropped — and 40 m promptly grew
//            spikes at LNA 1 the same evening (zap snapped to them).
//            One clean band does not acquit the house: the floor is a
//            per-station RF fact, so it is policy default 3 and a
//            setting, not a rebuild.
class AutoGain {
public:
    struct Decision {
        bool step = false;
        int  lna = 0;                      // new state when step is true
        const char* why = "";
    };

    void reset(int lnaState, qint64 nowMs) {
        lna_ = clampLna(lnaState);
        lastStepMs_ = nowMs;
        quietSinceMs_ = -1;
        attackFloor_ = -1;
        attackAtMs_ = 0;
    }

    void setMinLna(int v) { minLna_ = clampLna(v); }
    int lna() const { return lna_; }

    Decision tick(double peakDbfs, qint64 nowMs) {
        if (nowMs - lastStepMs_ < kHoldMs) return {};
        if (lna_ < minLna_) {
            // Below the floor (stored state from a manual session or an
            // old policy): auto mode climbs back into the safe zone —
            // the floor is a fact about the shack, not a suggestion.
            ++lna_;
            lastStepMs_ = nowMs;
            return {true, lna_, "below the RFI floor"};
        }
        if (peakDbfs > kAttackDb) {
            quietSinceMs_ = -1;
            if (lna_ >= 8) return {};
            ++lna_;
            lastStepMs_ = nowMs;
            attackFloor_ = lna_;           // don't release past this soon
            attackAtMs_ = nowMs;
            return {true, lna_, "peaks near clipping"};
        }
        if (peakDbfs < kReleaseDb) {
            if (quietSinceMs_ < 0) quietSinceMs_ = nowMs;
            int floorLna =
                (attackFloor_ >= 0 && nowMs - attackAtMs_ < kLockoutMs)
                    ? attackFloor_ : minLna_;
            if (floorLna < minLna_) floorLna = minLna_;
            if (nowMs - quietSinceMs_ >= kReleaseMs && lna_ > floorLna) {
                --lna_;
                lastStepMs_ = nowMs;
                quietSinceMs_ = nowMs;     // earn the next step separately
                return {true, lna_, "headroom unused"};
            }
            return {};
        }
        quietSinceMs_ = -1;                // sweet spot: sit still
        return {};
    }

    static constexpr double kAttackDb  = -4.0;
    static constexpr double kReleaseDb = -16.0;
    static constexpr qint64 kReleaseMs = 30000;
    static constexpr qint64 kHoldMs    = 2500;
    static constexpr qint64 kLockoutMs = 600000;
    static constexpr int    kMinLna    = 3;   // default release floor (RFI guard)

private:
    static int clampLna(int v) { return v < 0 ? 0 : (v > 8 ? 8 : v); }

    int    lna_ = 3;
    int    minLna_ = kMinLna;
    qint64 lastStepMs_ = 0;
    qint64 quietSinceMs_ = -1;
    int    attackFloor_ = -1;              // -1 = no recent attack
    qint64 attackAtMs_ = 0;
};

} // namespace ttc
