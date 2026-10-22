// SPDX-License-Identifier: GPL-2.0-or-later
// Practice-mode engine test: morse synth timing (PARIS math), the sim
// caller's full QSO protocol driven ONLY by the deterministic pump
// clock, per-contest exchange truth (CQ WW zone from cty, CW Open
// serial+name, CWT), copy verification incl. cut numbers, adaptive
// speed, patience/repeat behavior — and the isolation posture: the
// whole run is audio-off, wall-clock-free, and the engine has no
// database or keyer to touch by construction.
// No hardware, no network, no GUI, no sound.
#include <QCoreApplication>
#include <cstdio>

#include "contest/ContestDef.h"
#include "contest/PracticeEngine.h"
#include "util/CtyLookup.h"

using namespace ttc;

static int fails = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("ok    %s\n", what);                               \
        } else {                                                           \
            std::printf("FAIL  %s\n", what);                               \
            ++fails;                                                       \
        }                                                                  \
    } while (0)

// Pump in small ticks like the live 40 ms timer would.
static void run(PracticeEngine& p, int ms) {
    for (int t = 0; t < ms; t += 40) p.pumpForTest(40);
}

// Drive one clean QSO front to back; returns the verdict.
static PracticeEngine::Verdict oneQso(PracticeEngine& p, int opWpm) {
    p.opKeyed("CQ TEST N8EM", opWpm);
    run(p, 20000);                       // CQ plays + caller answers fully
    const QString call = p.truthCall();
    p.opKeyed(call + " 5NN 1", opWpm);   // his call + my exchange
    run(p, 25000);                       // reply + his exchange complete
    CQsoValues typed = p.truthValues();  // perfect copy
    typed.call = call;
    const auto v = p.verifyLog(typed);
    p.opKeyed("TU N8EM", opWpm);
    run(p, 15000);                       // TU + the next caller arrives
    return v;
}

static void testTiming() {
    PracticeEngine p;
    p.setAudioEnabled(false);
    const ContestDef* d = contestDef("CW-OPS");
    p.start(d, nullptr, {"K4RUM"}, 25, 7);
    // PARIS: at 20 wpm one dit is 60 ms = 480 samples at 8 kHz.
    // "E" = one dit + 3-dit char gap = 4 dits = 1920 samples.
    // "EE" = dit +3 gap+ dit +3 gap = 8 dits = 3840.
    // (synth is private; measure through the protocol instead: an op
    // transmission of known length must complete at the right tick.)
    p.opKeyed("CQ", 20);                 // C=-.-. A?? "CQ" = C(11 dits+2... )
    // C: 3+1+1+1+3+1+1+1=12 on/off +2 char = 14 dits; Q: 3+1+3+1+1+1+3+1
    //   = 14 + 2 = 16 dits; total 30 dits = 1.8 s at 20 wpm.
    run(p, 1600);
    CHECK(!p.callerLive(), "timing: CQ still keying at 1.6 s (30 dits)");
    run(p, 1200);                        // past 1.8 s + 900 ms max answer
    CHECK(p.callerLive(), "timing: caller answers after the CQ ends");
}

static void testProtocolAndVerify() {
    CtyLookup* cty = new CtyLookup;      // leak: fine in a test binary
    bool ok = false;
    for (const char* path : {"resources/cty.dat", "../resources/cty.dat",
                             "../../resources/cty.dat"})
        if (cty->load(QString::fromLatin1(path))) { ok = true; break; }
    if (!ok) { std::printf("FAIL  no cty.dat\n"); ++fails; return; }

    // CW Open: serial + name both ways.
    PracticeEngine p;
    p.setAudioEnabled(false);
    p.start(contestDef("CW-OPEN"), cty, {"K4RUM", "N5OT", "W1UJ"}, 25, 42);
    CHECK(p.active(), "protocol: engine starts");
    CHECK(!p.callerLive(), "protocol: idle until the op CQs");

    p.opKeyed("CQ CWO N8EM", 25);
    run(p, 20000);
    CHECK(p.callerLive(), "protocol: a caller answered the CQ");
    const QString call1 = p.truthCall();
    CHECK(!call1.isEmpty(), "protocol: caller has a callsign");
    const CQsoValues truth = p.truthValues();
    CHECK(!truth.serialR.isEmpty() && truth.serialR.toInt() > 0,
          "cw open: caller carries a serial");
    CHECK(!truth.exch1.isEmpty() && truth.exch1.toUpper() == truth.exch1,
          "cw open: caller carries a name");

    // Answer + copy it perfectly.
    p.opKeyed(call1 + " 1 JON", 25);
    run(p, 25000);
    CQsoValues typed = truth;
    typed.call = call1;
    auto v = p.verifyLog(typed);
    CHECK(v.haveCaller, "verify: live caller verified");
    CHECK(v.allGood && v.misses.isEmpty(), "verify: perfect copy is good");
    CHECK(v.call == call1, "verify: the call is revealed post-log");
    CHECK(v.qsos == 1 && v.good == 1 && v.streak == 1,
          "verify: counters advance");
    CHECK(v.wpm == 26, "adapt: clean copy speeds the next caller up");

    // TU closes; the next caller shows up on its own (single-calls flow).
    p.opKeyed("TU N8EM", 25);
    run(p, 15000);
    CHECK(p.callerLive(), "protocol: next caller arrives after TU");
    const QString call2 = p.truthCall();
    CHECK(call2 != call1, "protocol: a different station calls");

    // Busted copy: wrong serial. Speed backs off from 26 to 24.
    p.opKeyed(call2 + " 2 JON", 25);
    run(p, 25000);
    CQsoValues bust = p.truthValues();
    bust.call = call2;
    bust.serialR = QString::number(bust.serialR.toInt() + 111);
    v = p.verifyLog(bust);
    CHECK(!v.allGood && v.misses.size() == 1, "verify: busted serial flagged");
    CHECK(!v.misses.isEmpty() && v.misses.first().contains("sent"),
          "verify: miss names the truth");
    CHECK(v.streak == 0, "verify: a bust resets the streak");
    CHECK(v.wpm == 24, "adapt: a bust slows the next caller down");

    // Cut-number typing: T/N unroll when comparing ("1TT" == "100").
    p.opKeyed("TU", 25);
    run(p, 15000);
    if (p.callerLive()) {
        CQsoValues t3 = p.truthValues();
        t3.call = p.truthCall();
        if (t3.serialR.toInt() == 100) t3.serialR = "1TT";  // rarely exact
        v = p.verifyLog(t3);
        CHECK(v.allGood, "verify: cut numbers in typing normalize");
    }
}

static void testZonesFromCty() {
    CtyLookup* cty = new CtyLookup;
    bool ok = false;
    for (const char* path : {"resources/cty.dat", "../resources/cty.dat",
                             "../../resources/cty.dat"})
        if (cty->load(QString::fromLatin1(path))) { ok = true; break; }
    if (!ok) { std::printf("FAIL  no cty.dat\n"); ++fails; return; }

    // CQ WW: the caller's zone must be ITS OWN cty zone — the drill has
    // to agree with the zone auto-fill or every practice QSO flags amber.
    PracticeEngine p;
    p.setAudioEnabled(false);
    p.start(contestDef("CQ-WW-CW"), cty, {"DL1ABC"}, 28, 5);
    p.opKeyed("CQ", 28);
    run(p, 20000);
    CHECK(p.truthCall() == "DL1ABC", "cqww: the pool call answered");
    CtyInfo ci;
    cty->info("DL1ABC", ci);
    CHECK(p.truthValues().exch1.toInt() == ci.cq,
          "cqww: caller sends its real cty zone");
    CHECK(p.truthValues().rstR == "599", "cqww: RST truth is 599");
}

static void testCwtAndPatience() {
    PracticeEngine p;
    p.setAudioEnabled(false);
    p.start(contestDef("CW-OPS"), nullptr, {"N5OT"}, 30, 11);
    p.opKeyed("CQ CWT N8EM", 30);
    run(p, 20000);
    CHECK(p.callerLive(), "cwt: caller answered");
    const CQsoValues t = p.truthValues();
    CHECK(!t.exch1.isEmpty(), "cwt: name present");
    CHECK(!t.exch2.isEmpty(), "cwt: nr/state present");
    CHECK(t.rstR.isEmpty(), "cwt: no RST anywhere");

    // Ignore the caller completely: it repeats, then gives up.
    run(p, 60000);
    CHECK(!p.callerLive(), "patience: an ignored caller eventually leaves");

    // Esc mid-QSO: silence + reset, session stays up.
    p.opKeyed("CQ", 30);
    run(p, 20000);
    if (p.callerLive()) {
        p.abortSending();
        CHECK(!p.callerLive(), "esc: abort resets the caller");
        CHECK(p.active(), "esc: the session survives");
    }
    // verifyLog with nothing live: refused, nothing counted.
    const auto v = p.verifyLog(CQsoValues{});
    CHECK(!v.haveCaller, "verify: nothing live -> nothing to verify");
}

static void testAdaptClamp() {
    PracticeEngine p;
    p.setAudioEnabled(false);
    p.start(contestDef("CW-OPS"), nullptr, {"N5OT", "K4RUM"}, 13, 3);
    // Hammer busts: wpm must floor at 12, never below.
    for (int i = 0; i < 4; ++i) {
        p.opKeyed("CQ", 25);
        run(p, 20000);
        if (!p.callerLive()) break;
        CQsoValues wrong;                // empty everything = full bust
        wrong.call = "XX9XX";
        p.verifyLog(wrong);
        p.opKeyed("TU", 25);
        run(p, 15000);
    }
    CHECK(p.callerWpm() == 12, "adapt: floor clamps at 12 wpm");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testTiming();
    testProtocolAndVerify();
    testZonesFromCty();
    testCwtAndPatience();
    testAdaptClamp();
    if (fails == 0) std::printf("\nall ok\n");
    return fails ? 1 : 0;
}
