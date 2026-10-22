// SPDX-License-Identifier: GPL-3.0-or-later
// FreeDV Reporter frame handling, offline (no socket):
//   cmake --build build --target freedvtest && ./build/freedvtest
// Engine.IO open -> view-only connect; ping -> pong; the connect snapshot
// becomes the station table: ON AIR / TX n min ago (band map) / TX hours
// ago / listening / RX only (spot table only); rx_report spots the heard
// call at the RECEIVER's frequency; remove_connection; HF clamp.
#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>
#include <cstdio>
#include <algorithm>
#include <cstdlib>

#include "net/FreeDvReporter.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static const Spot* find(const QVector<Spot>& v, const char* call) {
    for (const Spot& s : v)
        if (s.call == QLatin1String(call)) return &s;
    return nullptr;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1 && QString(argv[1]) == "--live") {
        // Watch the real reporter for N seconds (view-only, nothing sent
        // about this station): ./freedvtest --live 30
        FreeDvReporter live;
        QObject::connect(&live, &FreeDvReporter::statusChanged,
                         [](const QString& s) { std::printf("  %s\n", qPrintable(s)); });
        live.setEnabled(true);
        QTimer::singleShot((argc > 2 ? atoi(argv[2]) : 30) * 1000, &app, [&] {
            std::printf("  %d stations online, %d spots\n", live.stationCount(),
                        int(live.spots().size()));
            QVector<Spot> v = live.spots();
            std::sort(v.begin(), v.end(), [](const Spot& a, const Spot& b) { return a.atSecs > b.atSecs; });
            for (const Spot& s : v)
                std::printf("    %-10s %10.1f kHz  %s%s\n", qPrintable(s.call), s.hz / 1000.0,
                            qPrintable(s.comment), s.mapHidden ? "" : "   [band map]");
            app.quit();
        });
        return app.exec();
    }
    FreeDvReporter r;
    const qint64 T = QDateTime::fromString("2026-10-15T15:20:00Z", Qt::ISODate).toSecsSinceEpoch();
    r.now = [T] { return T; };
    QStringList out;
    r.send = [&](const QString& t) { out << t; };
    int changed = 0;
    QObject::connect(&r, &FreeDvReporter::spotsChanged, [&] { ++changed; });

    r.handleFrame(R"(0{"sid":"x","upgrades":[],"pingInterval":25000,"pingTimeout":20000})");
    check(out.value(0) == R"(40{"protocol_version":2,"role":"view"})", "open -> view-only connect");
    r.handleFrame("2");
    check(out.value(1) == "3", "ping -> pong");
    r.handleFrame(R"(40{"sid":"abc"})");

    // The connect snapshot, shaped like the live one (2026-10-15): on air,
    // TX 6 min ago, TX hours ago, listening, RX-only, anonymous, 60 m.
    r.handleFrame(R"(42["bulk_update",[)"
        R"(["new_connection",{"sid":"s1","callsign":"f4lvf","grid_square":"JN18","rx_only":false,"last_update":"2026-10-15T15:15:13.849492+00:00"}],)"
        R"(["freq_change",{"sid":"s1","freq":7174000}],)"
        R"(["tx_report",{"sid":"s1","mode":"RADEV1","transmitting":true,"last_tx":"2026-10-15T15:15:13.849492+00:00"}],)"
        R"(["new_connection",{"sid":"s2","callsign":"WJ8A","grid_square":"EN82"}],)"
        R"(["freq_change",{"sid":"s2","freq":14236000}],)"
        R"(["tx_report",{"sid":"s2","mode":"RADEV1","transmitting":false,"last_tx":"2026-10-15T15:13:28.881991+00:00"}],)"
        R"(["new_connection",{"sid":"s3","callsign":"ZL2IT"}],)"
        R"(["freq_change",{"sid":"s3","freq":7083000}],)"
        R"(["tx_report",{"sid":"s3","mode":"RADEV1","transmitting":false,"last_tx":"2026-10-15T07:53:56.042139+00:00"}],)"
        R"(["new_connection",{"sid":"s4","callsign":"KN4BNX","last_update":"2026-10-15T15:12:38.308275+00:00"}],)"
        R"(["freq_change",{"sid":"s4","freq":14236000}],)"
        R"(["tx_report",{"sid":"s4","mode":"RADEV1","transmitting":false,"last_tx":null}],)"
        R"(["new_connection",{"sid":"s5","callsign":"F4WFD","rx_only":true}],)"
        R"(["freq_change",{"sid":"s5","freq":14236000}],)"
        R"(["new_connection",{"sid":"s6","callsign":""}],)"
        R"(["freq_change",{"sid":"s6","freq":5368500}],)"
        R"(["new_connection",{"sid":"s7","callsign":"S51SU"}],)"
        R"(["freq_change",{"sid":"s7","freq":3700000000}]]])");
    const QVector<Spot> v = r.spots();
    const Spot* f4 = find(v, "F4LVF");
    const Spot* wj = find(v, "WJ8A");
    const Spot* zl = find(v, "ZL2IT");
    const Spot* kn = find(v, "KN4BNX");
    const Spot* fw = find(v, "F4WFD");
    check(v.size() == 5, "every named station on HF is a spot, snapshot included");
    check(f4 && f4->hz == 7174000 && f4->comment.startsWith("RADEV1 · ON AIR") && !f4->mapHidden,
          "transmitting: ON AIR, on the band map");
    check(f4 && f4->grid == "JN18" && f4->comment.contains("JN18"), "  with its grid");
    check(wj && wj->comment.startsWith("RADEV1 · TX 6 min ago") && !wj->mapHidden && wj->atSecs < T,
          "TX 6 min ago: on the band map, at its real time");
    check(zl && zl->comment.contains("TX 7 h ago") && zl->mapHidden, "TX hours ago: table only");
    check(kn && kn->comment.contains("listening") && kn->mapHidden, "never transmitted: listening, table only");
    check(fw && fw->comment.contains("RX only"), "RX-only station says so");
    check(!find(v, "S51SU"), "out-of-band frequency is not a spot");

    r.handleFrame(R"(42["rx_report",{"sid":"s4","receiver_callsign":"KN4BNX","callsign":"ZL2AB","snr":7.4,"mode":"RADEV1"}])");
    const Spot* z = find(r.spots(), "ZL2AB");
    check(z && z->hz == 14236000 && z->comment.contains("KN4BNX") && z->comment.contains("7 dB"),
          "rx_report spots the heard call at the receiver's freq, with SNR");
    r.handleFrame(R"(42["rx_report",{"sid":"s4","receiver_callsign":"KN4BNX","callsign":"","snr":3}])");
    check(r.spots().size() == 6, "rx_report without a decoded call adds nothing");

    r.handleFrame(R"(42["tx_report",{"sid":"s1","mode":"RADEV1","transmitting":false,"last_tx":"2026-10-15T15:19:30.000000+00:00"}])");
    check(find(r.spots(), "F4LVF")->comment.startsWith("RADEV1 · TX just now"), "un-key: TX just now");
    r.handleFrame(R"(42["remove_connection",{"sid":"s2","callsign":"WJ8A"}])");
    check(!find(r.spots(), "WJ8A"), "remove_connection drops the station");
    check(changed >= 4, "spotsChanged fires");
    // The FreeDV window's columns: version, message, what each one heard.
    r.handleFrame(R"(42["new_connection",{"sid":"s9","callsign":"VE4WSC","grid_square":"EN19ku","version":"FreeDV 2.0.2"}])");
    r.handleFrame(R"(42["freq_change",{"sid":"s9","freq":14236000}])");
    r.handleFrame(R"(42["message_update",{"sid":"s9","message":"QRV 20m"}])");
    r.handleFrame(R"(42["rx_report",{"sid":"s9","receiver_callsign":"VE4WSC","callsign":"N8EM","snr":11.6,"mode":"RADEV1"}])");
    FreeDvReporter::StationInfo ve;
    for (const auto& st : r.stations())
        if (st.call == "VE4WSC") ve = st;
    check(ve.version == "FreeDV 2.0.2" && ve.message == "QRV 20m", "station list: version and message");
    check(ve.lastRxCall == "N8EM" && ve.lastRxSnr == 12, "  and what it last decoded (N8EM, 12 dB)");
    QString qFrom, qMsg;
    qint64 qHz = 0;
    QObject::connect(&r, &FreeDvReporter::qsyRequested, [&](const QString& c, qint64 hz, const QString& m) {
        qFrom = c; qHz = hz; qMsg = m;
    });
    r.handleFrame(R"(42["qsy_request",{"callsign":"ve4wsc","frequency":14240000,"message":"QSY?"}])");
    check(qFrom == "VE4WSC" && qHz == 14240000 && qMsg == "QSY?", "a QSY request to us surfaces");
    r.handleFrame(R"(42["remove_connection",{"sid":"s9"}])");

    r.handleFrame("42[garbage");
    r.handleFrame("6");
    check(r.spots().size() == 6, "junk frames are ignored (5 + VE4WSC's heard N8EM)");

    if (fails == 0) std::printf("freedvtest: all checks passed\n");
    return fails ? 1 : 0;
}
