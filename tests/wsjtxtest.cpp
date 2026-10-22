// SPDX-License-Identifier: GPL-2.0-or-later
// WsjtxListener offline: WSJT-X's UDP messages built by hand
// (NetworkMessage.hpp layout) and sent to the listener on a private port —
// message classification (CQ / calling me / others), Decode -> heard()
// at dial + DF, Status -> transmitting, Heartbeat/Close -> alive, a band
// change clearing the list, and the Close we send back. No WSJT-X, no radio.
#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QSettings>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <cstdio>

#include "log/WsjtxListener.h"

using namespace ttc;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static QByteArray head(quint32 type) {
    QByteArray b;
    QDataStream o(&b, QIODevice::WriteOnly);
    o.setByteOrder(QDataStream::BigEndian);
    o << quint32(0xadbccbda) << quint32(2) << type << QByteArray("WSJT-X");
    return b;
}
static void put(QByteArray& b, auto&&... v) {
    QDataStream o(&b, QIODevice::Append);
    o.setByteOrder(QDataStream::BigEndian);
    (o << ... << v);
}
static QByteArray status(quint64 dial, bool tx) {
    QByteArray b = head(1);
    put(b, dial, QByteArray("FT8"), QByteArray("K1ABC"), QByteArray("-10"),
        QByteArray("FT8"), true, tx, false, quint32(1500), quint32(1200),
        QByteArray("N8EM"), QByteArray("EN83"), QByteArray("FN42"));
    return b;
}
static QByteArray decode(quint32 df, qint32 snr, const char* msg, bool isNew = true) {
    QByteArray b = head(2);
    put(b, isNew, quint32(3600000), snr, 0.1, df, QByteArray("~"),
        QByteArray(msg), false, false);
    return b;
}

int main(int argc, char** argv) {
    QTemporaryDir cfg;
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("n8mus");
    QCoreApplication::setApplicationName("wsjtxtest");
    const quint16 port = 14237;
    QSettings().setValue("log/wsjtxAddr", "127.0.0.1");
    QSettings().setValue("log/wsjtxPort", port);
    QSettings().setValue("station/callsign", "N8EM");

    // --- classify -------------------------------------------------------
    struct C { const char* msg; bool ok; const char* call; const char* grid; bool me; };
    const C cases[] = {
        {"CQ K1ABC FN42", true, "K1ABC", "FN42", false},
        {"CQ DX JA1XYZ PM95", true, "JA1XYZ", "PM95", false},
        {"CQ POTA W8ABC EN81", true, "W8ABC", "EN81", false},
        {"CQ VE3XX", true, "VE3XX", "", false},
        {"N8EM K1ABC FN42", true, "K1ABC", "FN42", true},
        {"N8EM K1ABC -12", true, "K1ABC", "", true},
        {"N8EM <PJ4/K1ABC> RR73", true, "PJ4/K1ABC", "", true},
        {"W1AW K1ABC FN42", false, "", "", false},     // others' QSO
        {"TNX 73 GL", false, "", "", false},
        {"CQ", false, "", "", false},
    };
    for (const C& c : cases) {
        WsjtxListener::Heard h;
        const bool ok = WsjtxListener::classify(c.msg, "N8EM", h);
        if (ok != c.ok || (ok && (h.call != c.call || h.grid != c.grid || h.toMe != c.me))) {
            std::printf("FAIL classify \"%s\" -> %d %s %s %d\n", c.msg, ok,
                        qPrintable(h.call), qPrintable(h.grid), h.toMe);
            ++fails;
        }
    }

    // --- over UDP -------------------------------------------------------
    WsjtxListener l(nullptr, nullptr);
    l.start();
    QUdpSocket peer;                       // plays WSJT-X
    peer.bind(QHostAddress::LocalHost, 0);
    int states = 0;
    QObject::connect(&l, &WsjtxListener::stateChanged, [&](bool, bool) { ++states; });
    const auto send = [&](const QByteArray& d) {
        peer.writeDatagram(d, QHostAddress::LocalHost, port);
        QElapsedTimer t; t.start();
        while (t.elapsed() < 60) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    CHECK(!l.alive());
    send(head(0));                         // heartbeat
    CHECK(l.alive());
    CHECK(!l.transmitting());
    send(decode(1000, -5, "CQ K1ABC FN42"));   // no dial yet: ignored
    CHECK(l.heard().isEmpty());
    send(status(14074000, false));
    send(decode(1000, -5, "CQ K1ABC FN42"));
    send(decode(1500, -12, "N8EM JA1XYZ PM95"));
    send(decode(2000, 3, "W1AW VE3XX FN03"));      // not for us
    send(decode(2500, 3, "CQ G4ABC IO91", false)); // replay: not new
    auto h = l.heard();
    CHECK(h.size() == 2);
    for (const auto& x : h) {
        if (x.call == "K1ABC") { CHECK(x.hz == 14075000); CHECK(!x.toMe); CHECK(x.snr == -5); }
        else if (x.call == "JA1XYZ") { CHECK(x.hz == 14075500); CHECK(x.toMe); }
        else CHECK(false);
    }
    send(decode(1100, -2, "CQ K1ABC FN42"));   // same call: replaced, not added
    CHECK(l.heard().size() == 2);
    send(status(14074000, true));
    CHECK(l.transmitting());
    send(status(7074000, false));              // band change clears
    CHECK(!l.transmitting());
    CHECK(l.heard().isEmpty());

    // Close goes back to the address WSJT-X spoke from, with its id.
    CHECK(l.requestClose());
    QElapsedTimer t; t.start();
    while (!peer.hasPendingDatagrams() && t.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    CHECK(peer.hasPendingDatagrams());
    if (peer.hasPendingDatagrams()) {
        QDataStream in(peer.receiveDatagram().data());
        in.setByteOrder(QDataStream::BigEndian);
        quint32 magic = 0, schema = 0, type = 0; QByteArray id;
        in >> magic >> schema >> type >> id;
        CHECK(magic == 0xadbccbda); CHECK(type == 6); CHECK(id == "WSJT-X");
    }
    send(head(6));                             // WSJT-X says it is closing
    CHECK(!l.alive());
    CHECK(states >= 3);

    std::printf(fails ? "wsjtxtest: %d FAILED\n" : "wsjtxtest: all passed\n", fails);
    return fails ? 1 : 0;
}
