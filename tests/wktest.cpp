// SPDX-License-Identifier: GPL-2.0-or-later
//
// WinKeyer wire-format test. Every byte asserted here comes from the K1EL
// WinKeyer3 IC Interface & Operation Manual Rev 1.3 (3/19/2019), and where
// the datasheet prints a worked example we use ITS numbers so a failure
// here means we drifted from the document, not from a guess:
//
//   p8   Set Weighting     <03><32> for weight=50
//   p13  Set Key Comp      <11><B4> sets key comp to 180 mSecs
//   p14  Set Dit/Dah Ratio <17><nn>, nn 33-66, 50 = 1:3
//   p12  Set 1st Extension <10><nn>, nn 0-250 x 1 mSec
//   p8   Set PTT Lead/Tail <04><01><A0> lead-in=1, tail=160
//   p10  Set Farns WPM     <0D><12> for Farnsworth=18 WPM
//   p13  Set Paddle Swpt   <12><nn>, nn 10-90%
//
// NO HARDWARE: the test opens a pty and plays the keyer itself, answering
// the echo test and the host-open revision byte. It must never be pointed
// at /dev/cwkeyer — that is a real keyer wired to a transmitter.

#include "cw/WinKeyer.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

using ttc::WinKeyer;

static int failures = 0;

static QByteArray hex(const QByteArray& b) {
    QByteArray out;
    for (unsigned char c : b) out += QByteArray::number(c, 16).rightJustified(2, '0').toUpper() + " ";
    return out.trimmed();
}

static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// The pty master, played by us as if it were the keyer.
static int ptyFd = -1;
static std::atomic<bool> fakeRun{true};
static QByteArray captured;               // everything the host wrote
static std::mutex capMu;

// Fake WinKeyer: echo test answers with the byte that follows it, host
// open answers with a firmware revision. Everything else is recorded.
// Major firmware revision the fake keyer claims. 31 = WK3 (rev 31.03 in
// the manual); 23 stands in for WK2 silicon.
static std::atomic<int> fakeRev{31};

static void fakeKeyer() {
    QByteArray pend;
    while (fakeRun.load()) {
        char buf[256];
        const ssize_t n = ::read(ptyFd, buf, sizeof buf);
        if (n <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        {
            std::lock_guard<std::mutex> lk(capMu);
            captured.append(buf, int(n));
        }
        pend.append(buf, int(n));
        // Walk the admin commands we must answer.
        for (int i = 0; i + 1 < pend.size(); ++i) {
            if (uchar(pend[i]) != 0x00) continue;
            const uchar sub = uchar(pend[i + 1]);
            if (sub == 0x04 && i + 2 < pend.size()) {
                const char echo = pend[i + 2];   // echo test: bounce it back
                ::write(ptyFd, &echo, 1);
                i += 2;
            } else if (sub == 0x02) {
                const char rev = char(fakeRev.load());
                ::write(ptyFd, &rev, 1);
                i += 1;
            }
        }
        if (pend.size() > 512) pend.clear();
    }
}

// Drain what the host has written since the last call.
static QByteArray take() {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    QCoreApplication::processEvents();
    std::lock_guard<std::mutex> lk(capMu);
    QByteArray out = captured;
    captured.clear();
    return out;
}

static void expect(WinKeyer& wk, void (*act)(WinKeyer&), const char* name,
                   std::initializer_list<int> want) {
    take();                                // clear anything pending
    act(wk);
    const QByteArray got = take();
    QByteArray exp;
    for (int b : want) exp.append(char(b));
    const bool ok = got == exp;
    if (!ok)
        std::printf("  FAIL  %s: wrote [%s], expected [%s]\n", name,
                    hex(got).constData(), hex(exp).constData());
    else
        std::printf("  ok    %s -> %s\n", name, hex(exp).constData());
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::printf("WinKeyer wire format (K1EL WK3 manual Rev 1.3)\n");

    ptyFd = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (ptyFd < 0 || ::grantpt(ptyFd) || ::unlockpt(ptyFd)) {
        std::printf("  FAIL  cannot create pty\n");
        return 1;
    }
    const QString slave = QString::fromLocal8Bit(::ptsname(ptyFd));
    std::printf("  pty: %s\n", qPrintable(slave));

    std::thread fake(fakeKeyer);

    WinKeyer wk;
    const bool opened = wk.open(slave);
    check(opened, "host open handshake completes against the fake keyer");
    if (!opened) { fakeRun = false; fake.join(); return 1; }
    check(wk.firmwareRev() == 31,
          "firmware revision captured from the host-open reply (was discarded)");
    check(!wk.wk2Mode(), "host open leaves the keyer in WK1 mode");

    std::printf("\n datasheet worked examples\n");
    expect(wk, [](WinKeyer& k) { k.setWeighting(50); },
           "weight 50 = no adjustment (p8 example)", {0x03, 0x32});
    expect(wk, [](WinKeyer& k) { k.setKeyComp(180); },
           "key comp 180 ms (p13 example)", {0x11, 0xB4});
    expect(wk, [](WinKeyer& k) { k.setFarnsworth(18); },
           "Farnsworth 18 WPM (p10 example)", {0x0D, 0x12});
    expect(wk, [](WinKeyer& k) { k.setPttLeadTail(1, 160); },
           "PTT lead 1 / tail 160 (p8 example)", {0x04, 0x01, 0xA0});
    expect(wk, [](WinKeyer& k) { k.setRatio(50); },
           "dit/dah ratio 50 = 1:3 (p14)", {0x17, 0x32});
    expect(wk, [](WinKeyer& k) { k.setFirstExtension(12); },
           "first extension 12 ms (p12)", {0x10, 0x0C});
    expect(wk, [](WinKeyer& k) { k.setSwitchpoint(55); },
           "paddle switchpoint 55 % (p13)", {0x12, 0x37});
    expect(wk, [](WinKeyer& k) { k.setSidetone(5); },
           "sidetone table index 5 = 800 Hz (p7)", {0x01, 0x05});

    std::printf("\n range clamping (never write an out-of-range byte)\n");
    expect(wk, [](WinKeyer& k) { k.setWeighting(0); },
           "weight clamps up to 10", {0x03, 0x0A});
    expect(wk, [](WinKeyer& k) { k.setWeighting(999); },
           "weight clamps down to 90", {0x03, 0x5A});
    expect(wk, [](WinKeyer& k) { k.setKeyComp(-5); },
           "key comp clamps up to 0", {0x11, 0x00});
    expect(wk, [](WinKeyer& k) { k.setKeyComp(9999); },
           "key comp clamps down to 250", {0x11, 0xFA});
    expect(wk, [](WinKeyer& k) { k.setRatio(1); },
           "ratio clamps up to 33", {0x17, 0x21});
    expect(wk, [](WinKeyer& k) { k.setRatio(99); },
           "ratio clamps down to 66", {0x17, 0x42});
    expect(wk, [](WinKeyer& k) { k.setFirstExtension(300); },
           "first extension clamps down to 250", {0x10, 0xFA});
    expect(wk, [](WinKeyer& k) { k.setSwitchpoint(5); },
           "switchpoint clamps up to 10", {0x12, 0x0A});
    expect(wk, [](WinKeyer& k) { k.setFarnsworth(0); },
           "Farnsworth 0 = off passes through", {0x0D, 0x00});
    expect(wk, [](WinKeyer& k) { k.setFarnsworth(3); },
           "Farnsworth below range snaps to 10", {0x0D, 0x0A});

    std::printf("\n letterspace enters WK2 mode first (X1MODE is absent in WK1)\n");
    expect(wk, [](WinKeyer& k) { k.setLetterspace(6); },
           "WK2 mode then X1MODE, letterspace in the upper nibble",
           {0x00, 0x0B, 0x00, 0x0F, 0x60});
    check(wk.wk2Mode(), "WK2 mode is remembered");
    expect(wk, [](WinKeyer& k) { k.setLetterspace(15); },
           "second change does not re-enter WK2 mode",
           {0x00, 0x0F, 0xF0});

    std::printf("\n adopted parameters replay on reconnect\n");
    {
        wk.close();
        // Drain AFTER the close, not before: host close is 00 03, and its
        // trailing 03 reads exactly like a Set Weighting command byte to a
        // naive search. (It did — this assertion caught its own harness.)
        take();
        WinKeyer wk2;
        // Nothing adopted: a fresh keyer must be written NOTHING beyond
        // the speed the console has always sent. This is the policy that
        // keeps someone else's keyer untouched.
        const bool ok2 = wk2.open(slave);
        check(ok2, "reopen");
        const QByteArray after = take();
        check(!after.contains(QByteArray(1, 0x03)),
              "untouched keyer gets no weighting write");
        check(!after.contains(QByteArray(1, 0x11)),
              "untouched keyer gets no key-comp write");
        wk2.setKeyComp(25);
        wk2.setWeighting(60);
        take();
        wk2.close();
        const bool ok3 = wk2.open(slave);
        check(ok3, "reopen after adopting two parameters");
        const QByteArray replay = take();
        check(replay.contains(QByteArray::fromRawData("\x03\x3C", 2)),
              "weighting 60 replayed on reconnect");
        check(replay.contains(QByteArray::fromRawData("\x11\x19", 2)),
              "key comp 25 ms replayed on reconnect");
        wk2.close();
    }

    std::printf("\n WK3-only prosigns are gated on the firmware revision\n");
    {
        // [ AS, \ DN, ] KN exist only on WK3. On a WK2 they must be
        // dropped by US, not silently swallowed by the keyer.
        fakeRev = 23;                          // pretend WK2 silicon
        WinKeyer wk2;
        check(wk2.open(slave), "open against a WK2-revision keyer");
        check(!wk2.isWk3(), "rev 23 is not WK3");
        take();
        wk2.send("A[B");
        const QByteArray wk2out = take();
        check(wk2out == QByteArray("AB"),
              "WK2: '[' dropped, surrounding text still sent");
        wk2.close();

        fakeRev = 31;                          // WK3 silicon
        WinKeyer wk3;
        check(wk3.open(slave), "open against a WK3-revision keyer");
        check(wk3.isWk3(), "rev 31 is WK3");
        take();
        wk3.send("A[B]C\\D");
        const QByteArray wk3out = take();
        check(wk3out == QByteArray("A[B]C\\D"),
              "WK3: [ ] and \\ pass through to the keyer");
        wk3.close();
    }

    fakeRun = false;
    fake.join();
    ::close(ptyFd);

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
