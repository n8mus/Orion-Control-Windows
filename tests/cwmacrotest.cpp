// SPDX-License-Identifier: GPL-2.0-or-later
// Plain-language CW macro expansion (src/cw/CwMacros): the fields, the
// old %mc / %c spellings, a typo left visible, and the missing-field
// report the CW window refuses to key on.
#include "cw/CwMacros.h"
#include "log/QsoContext.h"
#include <QCoreApplication>
#include <cstdio>
using namespace ttc;
static int fails = 0;
static void chk(const QString& got, const QString& want, const char* what) {
    const bool ok = got == want;
    if (!ok) ++fails;
    std::printf("%-28s %-46s %s\n", what, qPrintable(got), ok ? "ok" : ("WANT: " + want).toLocal8Bit().constData());
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    CwMacroFields f;
    f.myCall = "N8EM"; f.myName = "JON"; f.myQth = "MI"; f.myGrid = "EN83al";
    f.hisCall = "K8ABC"; f.hisHandle = "BOB"; f.hisName = "Robert";
    f.rstSent = "579"; f.rstRcvd = "599"; f.hisGrid = "JO01";
    chk(expandCwMacro("[his call] DE [my call] UR RST [rst sent] [rst sent] BK", f),
        "K8ABC DE N8EM UR RST 579 579 BK", "plain fields");
    chk(expandCwMacro("TU [his handle] 73", f), "TU BOB 73", "handle not callbook name");
    chk(expandCwMacro("[HIS  CALL] de [My Call]", f), "K8ABC de N8EM", "case/space tolerant");
    chk(expandCwMacro("CQ CQ DE %MC %MC PSE K", f), "CQ CQ DE N8EM N8EM PSE K", "legacy %mc");
    chk(expandCwMacro("%C DE %MC", f), "K8ABC DE N8EM", "legacy %c");
    chk(expandCwMacro("[his dog] DE [my call]", f), "[his dog] DE N8EM", "typo stays visible");
    QString missing;
    CwMacroFields e = f; e.rstSent.clear();
    chk(expandCwMacro("UR RST [rst sent]", e, &missing), "UR RST", "empty field collapses");
    chk(missing, "rst sent", "missing named");
    missing.clear();
    chk(expandCwMacro("TU 73", f, &missing), "TU 73", "no fields");
    chk(missing, "", "nothing missing");
    // The shared record itself: a setter that changes nothing must stay
    // quiet, or the LOG and CW windows mirror each other forever.
    {
        QsoContext ctx;
        int beats = 0;
        QObject::connect(&ctx, &QsoContext::changed, [&beats] { ++beats; });
        ctx.setCall("k8abc");
        chk(ctx.call(), "K8ABC", "call upper-cased");
        ctx.setCall("K8ABC");                   // the echo back from LOG
        ctx.setCall(" k8abc ");                 // same call, spaced
        chk(QString::number(beats), "1", "no echo, no loop");
        ctx.setRstSent("579");
        ctx.setHandle("Bob");
        chk(ctx.handle(), "Bob", "handle kept as given");
        chk(QString::number(beats), "3", "a real change still reports");
        ctx.clear();
        chk(ctx.call() + ctx.rstSent() + ctx.handle(), "", "clear empties it");
        chk(QString::number(beats), "4", "one beat for the clear");
        ctx.clear();
        chk(QString::number(beats), "4", "clearing twice is quiet");
    }
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
