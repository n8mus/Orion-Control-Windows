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

    std::printf("character timing (PARIS units at 20 wpm, 60 ms/unit)\n");
    // E = 1 dit + 3 gap = 4 units = 240 ms;  O = --- = 3*3+2+3 = 14 = 840
    check(OrionKeyer::charMs('E', 20) == 240, "E at 20 wpm is 240 ms");
    check(OrionKeyer::charMs('O', 20) == 840, "O at 20 wpm is 840 ms");
    check(OrionKeyer::charMs('e', 20) == OrionKeyer::charMs('E', 20),
          "lowercase times the same as upper");
    check(OrionKeyer::charMs('E', 40) == 120, "doubling wpm halves the time");
    check(OrionKeyer::charMs(' ', 20) == 240, "space is the extra word gap");
    check(OrionKeyer::charMs('#', 20) == 0,   "unsendable character is 0 ms");

    {
        std::printf("\nqueue primes the rig's buffer, then paces\n");
        StubRadio r; OrionKeyer k(&r);
        check(k.open(), "open() succeeds on a CAT-keying radio");
        check(r.keyerOn, "open() enables the radio's internal keyer");
        k.setSpeed(20);
        r.clock.start();
        k.send("PARIS PARIS PARIS");
        pump(80);
        check(r.chars.size() < 17,
              "does NOT dump the whole macro (abort survives)");
        std::printf("      (%zu of 17 characters out at 80 ms)\n",
                    r.chars.size());
        pump(4000);
        // THE property, and the one the operator heard break: with
        // break-in the rig drops to receive the instant its buffer
        // empties, and the next character then has to switch it back —
        // audible as a space between every letter. So every character
        // must be handed over BEFORE the audio already buffered runs out.
        qint64 air = 0;
        int starved = 0;
        for (size_t i = 0; i < r.chars.size(); ++i) {
            if (r.atMs[i] > air + 5) ++starved;
            air = std::max(air, r.atMs[i])
                  + OrionKeyer::charMs(QChar(r.chars[i]), 20);
        }
        check(starved == 0, "the rig's buffer never runs dry mid-macro");
        std::printf("      (%zu characters, %d starved hand-offs)\n",
                    r.chars.size(), starved);
    }
    {
        std::printf("\nstop() is the abort the radio itself cannot do\n");
        StubRadio r; OrionKeyer k(&r);
        k.open(); k.setSpeed(20);
        k.send("PARISPARIS");
        pump(50);
        k.stop();
        const size_t sent = r.chars.size();
        pump(1200);
        check(r.chars.size() == sent,
              "nothing further is committed after stop()");
        // The abort budget is the buffer depth: whatever the radio already
        // holds still goes out, because *TU cannot flush it.
        check(sent <= 4, "only the buffered cushion had left, not the macro");
        std::printf("      (%zu of 10 characters had left; depth %d ms)\n",
                    sent, OrionKeyer::kDepthMs);
    }
    {
        std::printf("\nbackspace edits the queue before commitment\n");
        StubRadio r; OrionKeyer k(&r);
        k.open(); k.setSpeed(20);
        k.send("PARISE");
        pump(30);
        k.backspace();               // drop the trailing E, still queued
        pump(2500);
        std::string got(r.chars.begin(), r.chars.end());
        check(got == "PARIS", "backspaced character never went out");
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
        k2.close();
        check(!r2.keyerOn, "close() puts the internal keyer back off");
    }

    std::printf("\n%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}
