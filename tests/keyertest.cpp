// SPDX-License-Identifier: GPL-2.0-or-later
// OrionKeyer against a stub radio: proves the queue and the pacing
// without keying anything. The pacing IS the safety mechanism (the real
// radio buffers and *TU cannot abort), so it deserves a test that runs
// on every build rather than an on-air trial.
#include "cw/OrionKeyer.h"
#include "radio/RadioController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

using namespace ttc;

namespace {

class StubRadio : public RadioController {
public:
    StubRadio() { caps_.catCwKeying = true; }
    const CapabilityProfile& caps() const override { return caps_; }
    bool connected() const override { return true; }
    bool open(const std::string&) override { return true; }

    void sendCwChar(char c) override {
        chars.push_back(c);
        atMs.push_back(clock.isValid() ? clock.elapsed() : 0);
    }
    void setCwKeyerEnabled(bool on) override { keyerOn = on; }
    void setCwKeyerSpeed(int wpm) override { speed = wpm; }
    void setPtt(bool on) override { ptt = on; }

    CapabilityProfile caps_;
    std::vector<char> chars;
    std::vector<qint64> atMs;
    QElapsedTimer clock;
    bool keyerOn = false, ptt = false;
    int speed = 0;
};

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// Drive the event loop for ms so the release timer actually fires.
void pump(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // The model is the RADIO'S MEASURED behaviour, not textbook Morse:
    // char gap kCharGapUnits (~4.2) instead of 3, plus kCmdOverheadMs.
    // Modelling the textbook 3 units is what silently dropped macro text.
    const double U = 60.0;                       // 20 wpm: 60 ms per unit
    const double G = OrionKeyer::kCharGapUnits;
    const int    O = OrionKeyer::kCmdOverheadMs;
    std::printf("character timing at 20 wpm (60 ms/unit, gap %.1f units,"
                " +%d ms)\n", G, O);
    check(OrionKeyer::charMs('E', 20) == int((1 + G) * U) + O,
          "E is one dit plus the rig's real gap and overhead");
    check(OrionKeyer::charMs('O', 20) == int((11 + G) * U) + O,
          "O is three dahs, two in-char gaps, then the same");
    check(OrionKeyer::charMs('e', 20) == OrionKeyer::charMs('E', 20),
          "lowercase times the same as upper");
    check(OrionKeyer::charMs('E', 40) == int((1 + G) * U / 2) + O,
          "halving the unit does NOT halve it — the overhead is fixed");
    // The rig already emits its gap after the word's last letter, so a
    // space only makes up the rest of a 7-unit word gap.
    check(OrionKeyer::charMs(' ', 20) == int((7.0 - G) * U),
          "a space adds only the REMAINDER of the word gap");
    check(OrionKeyer::charMs(' ', 20) < int(4 * U),
          "which is less than a full word gap (that made words sound long)");
    check(OrionKeyer::charMs('#', 20) == 0,   "unsendable character is 0 ms");
    std::printf("      (E %d ms, O %d ms, space %d ms)\n",
                OrionKeyer::charMs('E', 20), OrionKeyer::charMs('O', 20),
                OrionKeyer::charMs(' ', 20));

    {
        std::printf("\na whole word is handed over as one burst\n");
        StubRadio r; OrionKeyer k(&r);
        check(k.open(), "open() succeeds on a CAT-keying radio");
        check(r.keyerOn, "open() enables the radio's internal keyer");
        k.setSpeed(20);
        r.clock.start();
        k.send("PARIS IS A WORD");
        pump(60);
        // THE invariant. Metering letter-by-letter let the rig's queue run
        // dry between characters and it restarted each time — heard as
        // "5 n n m i". A word must arrive complete so the rig's own keyer
        // owns the spacing inside it.
        std::string first(r.chars.begin(), r.chars.end());
        check(first == "PARIS", "the first WHOLE word goes out immediately");
        std::printf("      (rig received \"%s\" within 60 ms)\n",
                    first.c_str());
        check(r.atMs.size() >= 5 && r.atMs[4] - r.atMs[0] < 40,
              "and as one burst, not spread over the air time");
        pump(1200);
        check(std::string(r.chars.begin(), r.chars.end()) == "PARIS",
              "the NEXT word waits — it is not dumped too");
    }
    {
        std::printf("\na word gap is real time, not just bookkeeping\n");
        StubRadio r; OrionKeyer k(&r);
        k.open(); k.setSpeed(20);
        r.clock.start();
        k.send("E E");
        pump(2000);
        check(r.chars.size() == 2, "the space itself is never sent to the rig");
        const qint64 gap = r.atMs.size() > 1 ? r.atMs[1] - r.atMs[0] : 0;
        check(gap >= OrionKeyer::charMs('E', 20),
              "the second word waits for a real word gap");
        std::printf("      (gap %lld ms; E %d + word-gap remainder %d)\n",
                    static_cast<long long>(gap), OrionKeyer::charMs('E', 20),
                    OrionKeyer::charMs(' ', 20));
    }
    {
        std::printf("\nstop() is the abort the radio itself cannot do\n");
        StubRadio r; OrionKeyer k(&r);
        k.open(); k.setSpeed(20);
        k.send("PARIS PARIS");
        pump(50);
        k.stop();
        const size_t sent = r.chars.size();
        pump(1200);
        check(r.chars.size() == sent,
              "nothing further is committed after stop()");
        // The abort budget is the buffer depth: whatever the radio already
        // holds still goes out, because *TU cannot flush it.
        check(sent <= 5, "only the word already handed over had left");
        std::printf("      (%zu of 10 characters had left)\n", sent);
    }
    {
        std::printf("\nbackspace edits the queue, but is not promised\n");
        check(!OrionKeyer::kCaps.backspace,
              "caps do NOT claim backspace (the burst outruns it)");
        StubRadio r; OrionKeyer k(&r);
        k.open(); k.setSpeed(20);
        // Long enough that the tail is still queued behind the cushion.
        k.send("PARIS PARIS PARIS PARIS X");
        pump(30);
        k.backspace();               // drop the trailing X, still queued
        pump(9000);
        std::string got(r.chars.begin(), r.chars.end());
        check(got.find('X') == std::string::npos,
              "a character still queued is dropped by backspace");
        std::printf("      (sent \"%s\")\n", got.c_str());
    }
    {
        std::printf("\nrefusals and plumbing\n");
        StubRadio r; r.caps_.catCwKeying = false;
        OrionKeyer k(&r);
        check(!k.open(), "refuses a radio that cannot key over CAT");
        check(k.lastError().contains("cannot key"), "and says why");

        StubRadio r2; OrionKeyer k2(&r2);
        k2.open();
        k2.setSpeed(99);
        check(r2.speed == 60, "speed clamps to the radio's 10-60 range");
        k2.tune(true);  check(r2.ptt, "tune() keys a steady carrier");
        k2.tune(false); check(!r2.ptt, "and drops it");
        k2.send("~~~");
        pump(60);
        check(r2.chars.empty(), "unsendable characters are dropped, not keyed");
        // The rig's own table (prg guide p34): * AA, + AR, $ SK, ^ KN.
        for (char ps : {'*', '+', '$', '^'})
            check(OrionKeyer::charMs(QChar(ps), 20) > 0,
                  ps == '*' ? "prosign * (AA) is sendable"
                  : ps == '+' ? "prosign + (AR) is sendable"
                  : ps == '$' ? "prosign $ (SK) is sendable"
                              : "prosign ^ (KN) is sendable");
        check(OrionKeyer::charMs('$', 20) > OrionKeyer::charMs('+', 20),
              "SK (6 elements) outlasts AR (5)");
        k2.close();
        check(!r2.keyerOn, "close() puts the internal keyer back off");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
