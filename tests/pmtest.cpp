// SPDX-License-Identifier: GPL-2.0-or-later
//
// Array Solutions PowerMaster wire-format test — CRC and payload decode,
// no hardware needed. The idle frames were captured off the operator's
// own meter on /dev/ttyS5 (2026-08-02); the 128 W frame is the published
// community capture (IW0FFK). The CRC algorithm (poly 0xB1, init 0x00,
// final XOR 0xFF, payload only) was brute-forced against all three and
// was the unique solution — if a frame here ever fails, the firmware
// changed dialect and PmMeter needs a fresh look.

#include "radio/PmMeter.h"

#include <cstdio>

using ttc::PmMeter;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL  %s\n", what); ++failures; }
}

int main() {
    std::printf("PowerMaster frame decode\n");

    // 1. CRC over the three known frames.
    check(PmMeter::crc8(QByteArray("A")) == 0x8F, "ACK frame CRC");
    check(PmMeter::crc8(QByteArray("D,    0.0,    0.0, 0.00,0;0;0;0;0"))
              == 0x06, "live idle frame CRC");
    check(PmMeter::crc8(QByteArray("D,128,0, 1.11,0;0;0;0;0")) == 0x4C,
          "IW0FFK 128 W frame CRC");

    // 2. Idle payload: parses, but valid must be FALSE — no carrier, and
    //    the meter reports SWR 0.00 which means nothing.
    {
        PmMeter::Reading r;
        check(PmMeter::parsePayload(
                  QByteArray("D,    0.0,    0.0, 0.00,0;0;0;0;0"), r),
              "idle payload parses");
        check(!r.valid, "idle reading must not claim a carrier");
        check(!r.zValid, "scalar meter never claims impedance");
    }

    // 3. Carrier payload: 128 W forward, SWR 1.11 — valid.
    {
        PmMeter::Reading r;
        check(PmMeter::parsePayload(QByteArray("D,128,0, 1.11,0;0;0;0;0"), r),
              "carrier payload parses");
        check(r.valid, "128 W is a valid reading");
        check(r.watts == 128.0, "forward watts");
        check(r.refWatts == 0.0, "reflected watts");
        check(r.swr == 1.11, "swr");
        check(!r.zValid, "still no impedance from a scalar meter");
    }

    // 4. Rejects: the ACK payload is not data; junk stays out.
    {
        PmMeter::Reading r;
        check(!PmMeter::parsePayload(QByteArray("A"), r),
              "ACK payload is not a reading");
        check(!PmMeter::parsePayload(QByteArray("D,x,y,z"), r),
              "non-numeric payload rejected");
        check(!PmMeter::parsePayload(QByteArray(), r), "empty rejected");
        check(!PmMeter::parsePayload(QByteArray("Q,1,2,3"), r),
              "unknown frame type rejected");
    }

    if (failures == 0) std::printf("all frame checks passed\n");
    else               std::printf("%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
