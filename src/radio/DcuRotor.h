// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>

class QTimer;

namespace ttc {

class SerialPort;

// Hy-Gain DCU-3 rotor controller on RS-232 (4800 8N1, no flow control).
//
// The console drives the rotor itself instead of leaning on hamlib's
// rotctld, and re-exports it on :4533 (RotorServer) — the same
// single-master pattern as the rig on :4532. The reason is a protocol
// detail every fixed-length reader gets wrong, this one included until
// 2026-08-09:
//
//   "AI1;"      query position   -> "ddd;" in ~17 ms
//   "AP1ddd;"   set target       -> silent
//   "AM1;"      execute the turn -> silent
//   ";"         stop
//
// but WHILE TURNING the controller streams "ddd;" frames unasked, a few
// per second, and it can emit stray or truncated ones (";34;", "1;",
// "4;" all seen live on the bench). hamlib's DCU-3 backend answers each
// query with exactly one read of exactly four bytes and has no resync
// path (its Rotor-EZ sibling does), so the first turn leaves a backlog
// that grows every poll until the tty buffer is full — after which the
// readback is frozen on a minutes-old frame forever, while set still
// works. That is the "rose stuck north while the DCU-3 reads correct"
// bug: 21 hours pointing at a heading the antenna left long ago.
//
// So: never read a fixed count, never flush mid-stream (that is what
// truncates frames). Consume the stream continuously, keep the last
// well-formed frame, and only prod with AI1; when it has gone quiet.
class DcuRotor : public QObject {
    Q_OBJECT
public:
    explicit DcuRotor(QObject* parent = nullptr);
    ~DcuRotor() override;

    void setDevice(const QString& dev);    // /dev/portS4 at this station
    void setActive(bool on);
    bool holding() const;                  // we have the port open
    bool connected() const { return connected_; }

    double azimuth() const { return az_; } // last good frame, -1 unknown
    double target()  const { return target_; }

    // Both return false when we are not holding the port, so the
    // rotctld face in front of us can answer honestly instead of
    // acknowledging a turn that never left the building.
    bool turnTo(double azDeg);             // AP1ddd; + AM1;
    bool stop();                           // ";"

    // Pulls every complete "ddd;" frame out of buf_, consuming what it
    // uses (including the garbage in between) and leaving any partial
    // tail for the next chunk. Returns the LAST valid azimuth, or -1 if
    // the chunk held none. Static and side-effect free so dcutest can
    // replay the bench captures against it.
    static double parseFrames(QByteArray& buf);

signals:
    void azimuthChanged(double azDeg);     // -1 = lost/unknown
    void connectedChanged(bool on);

private:
    void onBytes(const QByteArray& chunk);
    void poll();
    void setConnected(bool on);

    SerialPort* port_ = nullptr;
    QTimer* timer_ = nullptr;
    QString dev_;
    QByteArray buf_;
    bool   active_ = false;
    bool   connected_ = false;
    double az_ = -1.0, target_ = -1.0;
    qint64 lastFrameMs_ = 0;               // when a frame last arrived
};

} // namespace ttc
