// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QVector>

class QUdpSocket;

namespace ttc {

class CtyLookup;
class LogDb;

// WSJT-X's UDP status broadcast, listened to the way GridTracker does:
// join the multicast group (default 224.0.0.1:2237, matching WSJT-X's own
// default) and pick the "Logged ADIF" message (type 12) out of the stream —
// a plain ADIF record inside the binary envelope. Every FT8/FT4 QSO WSJT-X
// logs then lands in the console's logbook automatically, so the
// worked-before colors and needed-columns know about digital contacts.
//
// The console's own uploader pushes these QSOs to the online logs
// (log/wsjtxPush, default true — the operator runs WITHOUT GridTracker).
// Set it false to hand pushing back to GridTracker; QSOs then arrive
// stamped already-uploaded so nothing double-posts.
class WsjtxListener : public QObject {
    Q_OBJECT
public:
    WsjtxListener(LogDb* db, const CtyLookup* cty,
                  QObject* parent = nullptr);
    void start();                          // bind + join per settings

    // One station heard by OUR WSJT-X: a CQ, or a call addressed to us.
    // Everything else (QSOs between others) is left to WSJT-X's own list.
    struct Heard {
        QString call, grid;
        qint64  hz = 0;                    // dial + audio offset
        qint64  atSecs = 0;                // epoch seconds of the decode
        int     snr = 0;
        bool    toMe = false;              // "N8EM K1ABC FN42" and the like
        QString message;
    };
    QVector<Heard> heard() const;          // newest per call, < 3 min old

    // WSJT-X itself: heard within the last 20 s (heartbeats come every
    // 15 s), and transmitting right now (its own Status flag).
    bool alive() const;
    bool transmitting() const { return alive() && transmitting_; }

    // Ask WSJT-X to close itself (protocol "Close", inbound since 2.x) —
    // its own clean exit, so settings save and jt9 goes with it. Sent to
    // the address it last spoke from; false if it was never heard.
    bool requestClose();

    // Pure parser for a decoded FT8/FT4 message, exposed for tests:
    // fills call/grid/toMe and returns true for a CQ or a call to myCall.
    static bool classify(const QString& message, const QString& myCall,
                         Heard& out);

signals:
    void qsoLogged(qint64 id, const QString& call, bool pushWanted);
    // The DX call WSJT-X is working changed (a decode was clicked, or a
    // transmission started) — feeds the New QSO window and the globe.
    // mode is WSJT-X's own ("FT8"), which outranks the rig's USB.
    void dxChanged(const QString& call, const QString& grid,
                   const QString& mode);
    void heardChanged();                   // new decode(s) worth drawing
    // alive() or transmitting() flipped.
    void stateChanged(bool alive, bool transmitting);

private:
    void onDatagram();

    LogDb* db_;
    const CtyLookup* cty_;
    QUdpSocket* sock_ = nullptr;
    QString lastDx_;
    QString myCall_;
    QByteArray clientId_;                  // "WSJT-X" (or its --rig-name)
    QHostAddress peer_;
    quint16 peerPort_ = 0;
    quint32 schema_ = 2;
    quint64 dialHz_ = 0;
    bool    transmitting_ = false;
    bool    aliveSent_ = false, txSent_ = false;
    QElapsedTimer lastHeard_;
    QVector<Heard> heard_;
    void noteState();
};

} // namespace ttc
