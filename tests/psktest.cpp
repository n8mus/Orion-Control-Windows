// SPDX-License-Identifier: GPL-3.0-or-later
// FreeDV reporting, offline — nothing is sent anywhere:
//   cmake --build build --target psktest && ./build/psktest
// - PSK Reporter datagram, byte-compared against the layout freedv-gui
//   builds (freedv-backend pskreporter.cpp): IPFIX header, both
//   templates, receiver + sender records, 4-byte padding, lengths
// - FreeDV Reporter reporting role: the connect payload, and the
//   freq_change / tx_report / rx_report events, only once the server
//   says connection_successful
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

#include "net/FreeDvReporter.h"
#include "net/PskReporter.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}
static quint32 be32(const QByteArray& b, int at) {
    return (quint32(quint8(b[at])) << 24) | (quint32(quint8(b[at + 1])) << 16)
         | (quint32(quint8(b[at + 2])) << 8) | quint32(quint8(b[at + 3]));
}
static int be16(const QByteArray& b, int at) { return (quint8(b[at]) << 8) | quint8(b[at + 1]); }

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // ---- PSK Reporter -------------------------------------------------
    const QVector<PskReporter::Rec> recs = {{"VE4WSC", 14236000, 7, 1760540000},
                                            {"G8SEZ", 5368500, -2, 1760540060}};
    const QByteArray d = PskReporter::encode("N8EM", "EN83al", "FreeDV tentec-console", recs,
                                             5, 0xCAFEF00D, 1760540100);
    // Expected size, freedv's arithmetic: 16 + 36 + 52 + rx + tx.
    int rx = 4 + 5 + 7 + 22;                       // 38 -> pad 40
    rx += (4 - rx % 4) % 4;
    int tx = 4 + (7 + 5 + 1 + 7 + 1 + 4) + (6 + 5 + 1 + 7 + 1 + 4);
    tx += (4 - tx % 4) % 4;
    std::printf("    datagram %d bytes (expected %d)\n", int(d.size()), 16 + 36 + 52 + rx + tx);
    check(d.size() == 16 + 36 + 52 + rx + tx, "size = header + templates + padded sets");
    check(be16(d, 0) == 10 && be16(d, 2) == d.size(), "IPFIX v10, total length");
    check(be32(d, 4) == 1760540100u && be32(d, 8) == 5 && be32(d, 12) == 0xCAFEF00Du,
          "export time, sequence, observation id");
    check(be16(d, 16) == 3 && be16(d, 52) == 2, "receiver then sender template");
    const int rxSet = 16 + 36 + 52;
    check(be16(d, rxSet) == 0x9992 && be16(d, rxSet + 2) == rx && d.mid(rxSet + 5, 4) == "N8EM",
          "receiver record: N8EM, padded length");
    const int txSet = rxSet + rx;
    check(be16(d, txSet) == 0x9993 && be16(d, txSet + 2) == tx, "sender set id and length");
    const int r0 = txSet + 4;
    const quint64 f = (quint64(quint8(d[r0 + 7])) << 32) | be32(d, r0 + 8);
    check(d.mid(r0 + 1, 6) == "VE4WSC" && f == 14236000 && qint8(d[r0 + 12]) == 7,
          "sender record: call, 5-byte frequency, SNR");
    check(d.mid(r0 + 14, 6) == "FREEDV" && d[r0 + 20] == 1 && be32(d, r0 + 21) == 1760540000u,
          "  mode FREEDV, info source 1, flow start");
    const QByteArray none = PskReporter::encode("N8EM", "EN83al", "x", {}, 0, 1, 1);
    check(none.size() == 16 + 36 + 20, "no records: receiver template + record (18 -> 20) only");

    // ---- FreeDV Reporter, reporting role --------------------------------
    FreeDvReporter r;
    QStringList out;
    r.send = [&](const QString& t) { out << t; };
    check(r.authPayload() == R"(40{"protocol_version":2,"role":"view"})", "default: view-only");
    r.setReporting(true, "n8em", "EN83al", "tentec-console 0.1.8", true);
    const QJsonObject auth = QJsonDocument::fromJson(r.authPayload().mid(2).toUtf8()).object();
    check(auth["role"] == "report" && auth["callsign"] == "N8EM" && auth["grid_square"] == "EN83al"
              && auth["rx_only"] == false && auth["os"] == "linux" && auth["protocol_version"] == 2,
          "reporting: role report, call, grid, TX-capable, linux, protocol 2");
    r.reportFrequency(14236000);
    r.reportTransmit(true);
    check(out.isEmpty(), "nothing sent before the server is ready");
    r.handleFrame(R"(0{"sid":"x","pingInterval":25000,"pingTimeout":20000})");
    check(out.value(0) == r.authPayload(), "open -> the reporting connect");
    r.handleFrame(R"(42["connection_successful",{}])");
    const auto ev = [&](int i) { return QJsonDocument::fromJson(out.value(i).mid(2).toUtf8()).array(); };
    check(ev(1).at(0) == "freq_change" && ev(1).at(1)["freq"].toDouble() == 14236000,
          "ready: our frequency");
    check(ev(2).at(0) == "tx_report" && ev(2).at(1)["mode"] == "RADEV1" && ev(2).at(1)["transmitting"] == true,
          "  and our TX state");
    r.reportTransmit(false);
    r.reportHeard("ve4wsc", 7);
    r.reportHeard("", 3);
    check(ev(3).at(0) == "tx_report" && ev(3).at(1)["transmitting"] == false, "un-key reported");
    check(ev(4).at(0) == "rx_report" && ev(4).at(1)["callsign"] == "VE4WSC" && ev(4).at(1)["snr"] == 7,
          "heard: VE4WSC at 7 dB");
    check(ev(5).at(1)["callsign"] == "", "receiving, no call yet: empty callsign (freedv-gui's '--')");
    r.reportFrequency(14236000);
    check(out.size() == 6, "an unchanged frequency is not re-sent");
    r.sendStatusMessage("QRV 20 m");
    r.requestQsy("sidX", 14236000, "14236.0 kHz?");
    check(ev(6).at(0) == "message_update" && ev(6).at(1)["message"] == "QRV 20 m", "status message");
    check(ev(7).at(0) == "qsy_request" && ev(7).at(1)["dest_sid"] == "sidX"
              && ev(7).at(1)["frequency"].toDouble() == 14236000, "QSY request (dest_sid, frequency)");

    if (fails == 0) std::printf("psktest: all checks passed\n");
    return fails ? 1 : 0;
}
