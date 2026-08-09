// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

namespace ttc {

class DcuRotor;
class RotorClient;
class RotorServer;

// One rotor for the rest of the console to talk to, whichever way the
// wire is arranged. Two modes, chosen by "rotor/mode":
//
//   "direct"  (default) — we own the DCU-3 on its serial port and
//              re-export it on :4533, so cqrlog's SP/LP buttons still
//              work. Reads a live heading while the antenna turns.
//   "rotctld" — someone else's rotator daemon owns the rotor and we are
//              just a client (a different rotor, a remote station, or a
//              DCU-3 back on its USB port with the jumper on U).
//
// The direct path exists because hamlib's DCU-3 backend cannot survive
// the controller's habit of streaming position while it turns — see the
// header of DcuRotor for the whole story.
class RotorLink : public QObject {
    Q_OBJECT
public:
    explicit RotorLink(QObject* parent = nullptr);

    void configure();                      // (re)read QSettings
    void setActive(bool on);
    bool connected() const;

    double azimuth() const;                // -1 unknown
    double target() const;

    void turnTo(double azDeg);
    void stop();

    bool direct() const { return direct_; }

signals:
    void azimuthChanged(double azDeg);
    void connectedChanged(bool on);

private:
    DcuRotor*    dev_ = nullptr;
    RotorServer* srv_ = nullptr;
    RotorClient* tcp_ = nullptr;
    bool direct_ = true;
    bool active_ = false;
};

} // namespace ttc
