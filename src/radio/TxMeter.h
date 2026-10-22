// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

#include <cstdint>

namespace ttc {

// Common face of the external TX wattmeters (TelePost LP-100A,
// Array Solutions PowerMaster). The SWR sweep and anything else that
// consumes TX metering talks to this; which meter is behind it is the
// operator's meter/model setting. The two instruments differ in kind:
// the LP-100A is a vector meter (R+jX per reading, zValid true under
// carrier), the PowerMaster is a fast scalar (forward/reflected/SWR
// only, zValid always false) — so sweeps taken through the PowerMaster
// draw SWR curves but never a Smith chart, and the chart menu item
// simply stays dark for those runs.
class TxMeter : public QObject {
    Q_OBJECT
public:
    struct Reading {
        double  watts    = 0.0;      // forward
        double  refWatts = 0.0;      // reflected (PowerMaster reports it)
        double  swr      = 1.0;
        double  dbm      = 0.0;
        double  zOhm     = 0.0;      // vector meters only …
        double  phaseDeg = 0.0;
        double  rOhm     = 0.0;
        double  xOhm     = 0.0;
        // valid: the meter actually saw a carrier — swr is trustworthy.
        // zValid: the impedance fields are real (vector meter + carrier).
        bool    valid    = false;
        bool    zValid   = false;
        int     mode     = -1;       // LP-100A display mode, else -1
        int     range    = 0;        // LP-100A power range, else 0
        QString callsign;            // as programmed into the meter, if any
        qint64  tsMs     = 0;
    };

    using QObject::QObject;
    ~TxMeter() override = default;

    // pollMs is advisory: the LP-100A is polled at that rate, the
    // PowerMaster streams at its own and ignores it.
    virtual bool start(const QString& device, int pollMs = 100) = 0;
    virtual void stop() = 0;
    virtual bool isAlive() const = 0;

    const Reading& last() const { return last_; }

signals:
    void reading(const ttc::TxMeter::Reading& r);
    void aliveChanged(bool alive);
    void error(const QString& what);

protected:
    Reading last_;
};

} // namespace ttc
