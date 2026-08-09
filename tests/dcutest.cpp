// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hy-Gain DCU-3 rotor: frame parser and the rotctld server we put in
// front of it. No rotor needed.
//
// Every byte string below was captured off the operator's own DCU-3 on
// /dev/portS4 (2026-08-09) with rotctld stopped. The interesting ones
// are from a controller that is TURNING: it streams "ddd;" frames on
// its own initiative, several per second, and it emits stray and
// truncated ones. That is the traffic hamlib's DCU-3 backend cannot
// survive — it reads exactly four bytes per query, so the surplus
// becomes a backlog that grows with every poll until the tty buffer is
// full and the readback is frozen on a frame minutes old. The console's
// rose sat on north for 21 hours that way while aiming still worked.
// A parser that stays honest on these captures is the whole fix.

#include "net/RotorServer.h"
#include "radio/DcuRotor.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QTcpSocket>

#include <cstdio>

using ttc::DcuRotor;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) { std::printf("  FAIL  %s\n", what); ++failures; }
}

static double parse(const char* bytes) {
    QByteArray b(bytes);
    return DcuRotor::parseFrames(b);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::printf("DCU-3 rotor frame decode\n");

    // 1. The quiet case: one query, one answer.
    check(parse("134;") == 134.0, "single frame");
    check(parse("000;") == 0.0, "north");
    check(parse("359;") == 359.0, "just west of north");
    // The controller says 360 for north on some headings; hamlib folds it
    // and so do we, or a client sees an out-of-range bearing.
    check(parse("360;") == 0.0, "360 folds to 0");

    // 2. Streaming while it turns: the LAST frame is the live heading,
    //    not the first — reporting the first is exactly the one-poll lag
    //    that snowballed into a frozen needle.
    check(parse("134;134;134;134;") == 134.0, "repeated frames");
    check(parse("131;135;137;140;") == 140.0, "rising sweep takes the last");
    check(parse("199;207;215;229;") == 229.0, "turn in progress");

    // 3. Real damage, verbatim from the bench log.
    check(parse("134;134;134;134;;34;134;134;") == 134.0,
          "stray semicolon mid-stream");
    check(parse("134;1;4;134;134;134;") == 134.0, "split frame recovers");
    check(parse("2") == -1.0, "lone digit is not a heading");
    check(parse("19;131;") == 131.0, "truncated head, good tail");
    check(parse("13;") == -1.0, "two digits and a semicolon is not a frame");
    check(parse("48;") == -1.0, "tail of a lost frame");
    check(parse("148") == -1.0, "unterminated frame waits");
    check(parse("999;") == -1.0, "impossible bearing rejected");
    check(parse("AI1;") == -1.0, "our own echoed query is not a heading");

    // 4. Partial tails survive to be completed by the next chunk — the
    //    stream arrives in whatever chunks the UART gives us.
    {
        QByteArray buf("134;13");
        check(DcuRotor::parseFrames(buf) == 134.0, "chunk 1 yields a frame");
        check(buf == "13", "partial tail is kept");
        buf += "5;";
        check(DcuRotor::parseFrames(buf) == 135.0, "chunk 2 completes it");
        check(buf.isEmpty(), "consumed frame leaves nothing behind");
    }
    // Garbage must not accumulate: a wedged or mis-wired port (a radio's
    // CAT stream into the rotor setting, say) can talk forever.
    {
        QByteArray buf;
        for (int i = 0; i < 500; ++i) {
            buf += "not a rotor frame";
            DcuRotor::parseFrames(buf);
        }
        check(buf.size() <= 4, "buffer stays bounded on junk");
    }

    // 5. The rotctld face we present to cqrlog and Not1MM. With no
    //    reading yet it must report an error, NOT 0.0 — a client told
    //    "0.0" points a beam at north and believes it.
    {
        DcuRotor rotor;                     // never opened: azimuth is -1
        ttc::RotorServer srv(&rotor);
        check(srv.start(0), "server listens");
        QTcpSocket c;
        c.connectToHost(QHostAddress::LocalHost, srv.port());
        check(c.waitForConnected(2000), "client connects");

        // Both ends live in this one thread, so the server only sees the
        // connection and the command while events are being processed.
        const auto ask = [&c](const char* cmd) {
            c.write(cmd);
            c.waitForBytesWritten(1000);
            for (int i = 0; i < 100 && c.bytesAvailable() == 0; ++i)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            return QString::fromLatin1(c.readAll());
        };
        check(ask("p\n").startsWith("RPRT -"), "unknown position is an error");
        check(ask("_\n").contains("DCU"), "get_info names the model");
        check(ask("\\dump_state\n").contains("406"), "dump_state is model 406");
        // Turning a rotor we do not hold must fail, not be acknowledged:
        // an "RPRT 0" here is cqrlog being told its beam is on its way.
        check(ask("+P 90 0\n").contains("RPRT -5"), "set with no port errors");
        check(ask("P 999 0\n").startsWith("RPRT -"), "absurd bearing rejected");
        check(ask("Z\n").startsWith("RPRT -"), "unknown command is an error");
    }

    std::printf(failures ? "  %d FAILURE(S)\n" : "  all checks passed\n",
                failures);
    return failures ? 1 : 0;
}
