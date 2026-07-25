// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QTcpServer>
#include "radio/RadioController.h"

class QTcpSocket;

namespace ttc {

// Hamlib rigctld-protocol TCP server. This is the interop seam: cqrlog, WSJT-X,
// fldigi and GridTracker point their "Hamlib NET rigctl" client at this port and
// drive the Ten-Tec radio through us instead of flrig.
//
// Covers what the station's clients actually send: f/F (freq), m/M (mode),
// t/T (PTT), i (split freq), s (split vfo), v, \chk_vfo and a minimal
// \dump_state. Extend when a client trips over a missing command.
class RigctldServer : public QObject {
    Q_OBJECT
public:
    explicit RigctldServer(RadioController* radio, QObject* parent = nullptr);

    bool listen(quint16 port = 4532);
    quint16 port() const { return server_.serverPort(); }

public slots:
    // Fed from the radio driver so `f`/`m`/`t` queries answer from cached state.
    void cacheFrequency(uint64_t hz) { freqHz_ = hz; }
    void cacheBandwidth(int bwHz)    { bwHz_ = bwHz; }
    void cacheMode(Mode m)           { mode_ = m; }
    void cachePtt(bool on)           { ptt_ = on; }

signals:
    void clientConnected(const QString& peer);
    void commandReceived(const QString& line);
    void pttRequested(bool on);       // client sent 'T 1' / 'T 0'

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    QByteArray handleLine(const QByteArray& line);
    QByteArray handleExtended(const QByteArray& cmd, const QList<QByteArray>& args);

    QTcpServer server_;
    RadioController* radio_;
    uint64_t freqHz_ = 14000000;
    int bwHz_ = 2400;
    Mode mode_ = Mode::USB;
    bool ptt_ = false;
};

} // namespace ttc
