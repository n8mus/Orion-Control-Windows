// SPDX-License-Identifier: GPL-3.0-or-later
// Winlink session watching (PatMonitor), offline — Pat's own websocket
// JSON (la5nta/pat api/wshub.go, api/types/types.go) fed by hand:
//   cmake --build build --target wltest && ./build/wltest
#include <QCoreApplication>
#include <cstdio>

#include "net/PatMonitor.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    PatMonitor m;
    QList<bool> edges;
    QString lastRemote, lastProg;
    QObject::connect(&m, &PatMonitor::sessionChanged, [&](bool a, const QString& r) {
        edges << a;
        lastRemote = r;
    });
    QObject::connect(&m, &PatMonitor::progressChanged, [&](const QString& t) { lastProg = t; });

    m.handleMessage(R"({"Status":{"active_listeners":[],"connected":false,"dialing":false,"remote_addr":"","http_clients":["127.0.0.1"],"config_hash":"x"}})");
    check(!m.active() && edges.isEmpty(), "idle status: no session");
    m.handleMessage(R"({"Ping":true})");
    m.handleMessage(R"({"LogLine":"2026/10/15 connecting..."})");
    check(edges.isEmpty(), "pings and log lines change nothing");
    m.handleMessage(R"({"Status":{"connected":false,"dialing":true,"remote_addr":""}})");
    check(m.active() && edges == QList<bool>{true}, "dialing = a session (the radio will key)");
    m.handleMessage(R"({"Status":{"connected":true,"dialing":false,"remote_addr":"W8EDU"}})");
    check(m.active() && edges.size() == 1 && m.remote() == "W8EDU", "connected: same session, gateway named");
    m.handleMessage(R"({"Progress":{"bytes_transferred":3174,"bytes_total":12288,"mid":"A","subject":"hi","receiving":true,"sending":false,"done":false}})");
    check(lastProg == "receiving 3.1/12.0 kB", "progress readout");
    m.handleMessage(R"({"Progress":{"bytes_transferred":12288,"bytes_total":12288,"receiving":true,"done":true}})");
    check(lastProg.isEmpty(), "done clears the readout");
    m.handleMessage(R"({"Status":{"connected":false,"dialing":false,"remote_addr":""}})");
    check(!m.active() && edges == (QList<bool>{true, false}) && lastRemote == "W8EDU",
          "disconnect ends the session (remembering who it was)");
    m.handleMessage("not json");
    check(!m.active(), "junk ignored");
    if (fails == 0) std::printf("wltest: all checks passed\n");
    return fails ? 1 : 0;
}
