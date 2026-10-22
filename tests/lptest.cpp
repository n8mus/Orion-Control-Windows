// SPDX-License-Identifier: GPL-2.0-or-later
//
// TelePost LP-100A wire-format test. No meter and no serial port needed —
// this pins the frame layout, which is the one thing that silently breaks
// everything downstream if it drifts.
//
// The "live" frames below were captured off the operator's own LP-100A on
// /dev/ttyS5 (2026-08-01). If a future firmware changes the format, this
// test is where you find out.

#include "radio/LpMeter.h"

#include <QByteArray>

#include <cmath>
#include <cstdio>

using ttc::LpMeter;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL  %s\n", what); ++failures; }
}

static void near(double got, double want, double eps, const char* what) {
    if (std::fabs(got - want) > eps) {
        std::printf("  FAIL  %s: got %g want %g\n", what, got, want);
        ++failures;
    }
}

int main() {
    std::printf("LP-100A frame decode\n");

    // 1. The real idle frame. Note SWR 1.00 and 52.1 ohms at 83.6 degrees
    //    with ZERO watts — the impedance engine free-runs without a
    //    carrier, so zValid must be false and nothing may trust R/X here.
    {
        LpMeter::Reading r;
        const QByteArray f = ";0000.00,052.1,083.6,0,N8EM  ,2,1,-2.3,1.00";
        check(f.size() == 43, "idle frame is 43 bytes");
        check(LpMeter::parseFrame(f, r), "idle frame parses");
        near(r.watts, 0.0, 1e-9, "idle watts");
        near(r.swr, 1.00, 1e-9, "idle swr");
        near(r.dbm, -2.3, 1e-9, "idle dbm");
        check(!r.zValid, "idle frame must NOT claim valid impedance");
        near(r.rOhm, 0.0, 1e-9, "idle R suppressed");
        near(r.xOhm, 0.0, 1e-9, "idle X suppressed");
        check(r.callsign == "N8EM", "callsign trimmed");
        check(r.range == 2, "range 2 = 25 W (lowest, autoranged at idle)");
        check(r.mode == int(LpMeter::Mode::PeakHold), "mode 1 = peak hold");
    }

    // 2. A carrier up into a purely resistive 50 ohm load: X must come out
    //    at zero and R at 50, which is the case the sweep's resonance
    //    report hinges on.
    {
        LpMeter::Reading r;
        check(LpMeter::parseFrame(";0035.00,050.0,000.0,0,N8EM  ,1,2,45.4,1.00", r),
              "resistive frame parses");
        check(r.zValid, "35 W is a valid impedance measurement");
        near(r.rOhm, 50.0, 1e-6, "R of 50+j0");
        near(r.xOhm, 0.0, 1e-6, "X of 50+j0");
        check(r.mode == int(LpMeter::Mode::Tune), "mode 2 = tune");
        check(r.range == 1, "range 1 = 250 W");
    }

    // 3. Signed phase: a capacitive load must give NEGATIVE reactance.
    //    Getting this sign backwards would put the antenna's resonance on
    //    the wrong side of the dial.
    {
        LpMeter::Reading r;
        check(LpMeter::parseFrame(";0100.00,070.7,-45.0,0,N8EM  ,0,0,50.0,1.90", r),
              "capacitive frame parses");
        check(r.zValid, "100 W is valid");
        near(r.rOhm, 50.0, 0.05, "R of 70.7 ohms at -45 deg");
        near(r.xOhm, -50.0, 0.05, "X negative for a capacitive load");
        check(r.mode == int(LpMeter::Mode::Average), "mode 0 = average");
        check(r.range == 0, "range 0 = 2500 W");
    }

    // 4. Rejects. Every one of these has been seen or is one bit-slip away:
    //    a truncated read, a frame with no leading ';', a field count that
    //    does not match, and numeric junk.
    {
        LpMeter::Reading r;
        check(!LpMeter::parseFrame(";0000.00,052.1,083.6,0,N8EM  ,2,1,-2.3", r),
              "short frame rejected");
        check(!LpMeter::parseFrame("00000.00,052.1,083.6,0,N8EM ,2,1,-2.3,1.00", r),
              "frame without leading ';' rejected");
        check(!LpMeter::parseFrame(";0000.00 052.1 083.6 0 N8EM   2 1 -2.3 1.00", r),
              "wrong field count rejected");
        check(!LpMeter::parseFrame(";0000.00,052.1,083.6,0,N8EM  ,2,1,--.-,1.00", r),
              "non-numeric field rejected");
        check(!LpMeter::parseFrame(QByteArray(), r), "empty frame rejected");
    }

    if (failures == 0) std::printf("all frame checks passed\n");
    else               std::printf("%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
