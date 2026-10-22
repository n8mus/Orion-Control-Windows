// SPDX-License-Identifier: GPL-3.0-or-later
// The console's rigctld server (:4532) against Pat's (Winlink) exact
// client sequence, over a real TCP socket — with NO radio attached, so a
// PTT command only raises pttRequested (nothing can key):
//   cmake --build build --target rigctldtest && ./build/rigctldtest
// Pat (la5nta/wl2k-go rigcontrol/hamlib/rigctld.go) sends "dump_caps"
// (Ping), "\chk_vfo", "\get_freq", "\set_freq <Hz>", "t", "\set_ptt 1|0"
// and reads exactly ONE line per command. Also: the short forms and the
// '+' extended path still answer as before.
#include <QCoreApplication>
#include <QTcpSocket>
#include <cstdio>

#include "net/RigctldServer.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    RigctldServer srv(nullptr);
    if (!srv.listen(0)) { std::printf("listen failed\n"); return 1; }
    QList<bool> ptt;
    QObject::connect(&srv, &RigctldServer::pttRequested, [&](bool on) { ptt << on; });
    srv.cacheFrequency(14236000);

    QTcpSocket c;
    c.connectToHost("127.0.0.1", srv.port());
    check(c.waitForConnected(2000), "connects");
    // One command, one line back — exactly how Pat reads.
    const auto ask = [&](const QByteArray& cmd) -> QByteArray {
        c.write(cmd + "\n");
        c.flush();
        while (!c.canReadLine()) {
            app.processEvents();
            if (!c.waitForReadyRead(50) && !c.canReadLine()) {
                static int spins = 0;
                if (++spins > 60) return "(timeout)";
            }
        }
        return c.readLine().trimmed();
    };
    struct Step { const char* send; const char* want; };
    const Step pat[] = {
        {"dump_caps", "RPRT 0"},                   // Ping
        {"\\chk_vfo", "0"},                        // VFO mode: no prefix
        {"\\get_freq", "14236000"},
        {"\\set_freq 7100000", "RPRT 0"},          // QSY to the gateway
        {"\\get_freq", "7100000"},
        {"t", "0"},
        {"\\set_ptt 1", "RPRT 0"},                 // ARQ turnaround: key...
        {"\\set_ptt 0", "RPRT 0"},                 // ...and un-key
        {"\\set_freq 14236000", "RPRT 0"},         // put the dial back
        {"f", "14236000"},                         // short form still fine
        {"\\set_freq 0", "RPRT -1"},               // never zero the rig
    };
    for (const Step& s : pat) {
        const QByteArray got = ask(s.send);
        std::printf("    %-22s -> %s\n", s.send, got.constData());
        check(got == s.want, "  one-line answer as Pat expects");
    }
    check(ptt == QList<bool>({true, false}), "\\set_ptt reaches the PTT signal (1 then 0)");
    // The stream stayed aligned: one more round trip answers itself.
    check(ask("\\get_freq") == "14236000", "no stray lines left behind");
    // cqrlog's extended form is untouched.
    c.write("+\\get_freq\n");
    c.flush();
    QByteArray ext;
    for (int i = 0; i < 60 && !ext.contains("RPRT"); ++i) {
        app.processEvents();                       // the server shares this thread
        c.waitForReadyRead(50);
        ext += c.readAll();
    }
    std::printf("    +\\get_freq -> %s\n", ext.replace('\n', " | ").constData());
    check(ext.startsWith("get_freq:") && ext.contains("RPRT 0"), "extended (+) protocol unchanged");

    if (fails == 0) std::printf("rigctldtest: all checks passed\n");
    return fails ? 1 : 0;
}
