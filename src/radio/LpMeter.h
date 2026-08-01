// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>

class QTimer;

namespace ttc {

class SerialPort;

// TelePost LP-100A digital vector RF wattmeter.
//
// Protocol (115200 8N1, no flow control, straight-through DB9 M-F — NOT a
// null modem). There is no streaming mode: you poll with the single ASCII
// byte 'P' and the meter answers with exactly 43 bytes starting with ';'
// and NO terminator, so framing is by leading ';' + length. Nine CSV
// fields:
//
//   ;<watts>,<|Z|>,<phase>,<alarm>,<call>,<range>,<mode>,<dBm>,<SWR>
//   ;0000.00,052.1,083.6,0,N8EM  ,2,1,-2.3,1.00      <- live idle frame
//
// Fixed-offset tables for this frame circulate on the net and are off by
// one on the power field; splitting on ',' is both simpler and right.
//
// At idle the meter still reports |Z| and phase, and they are NOISE — the
// frame above claims 52.1 ohms at 83.6 degrees into a dead radio. Anything
// consuming impedance MUST check zValid, which gates on forward power.
//
// Mode control is the byte 'F', which cycles Average -> PeakHold -> Tune
// -> Average. Bench-verified on the operator's meter 2026-08-01; all three
// transitions observed. The 'M' byte is documented by third-party code as
// the mode control and IS NOT — it cycles some LCD page that never appears
// in the frame, so pressing it looks like a no-op here. Don't "fix" this
// back to 'M'. 'A' (cycle the SWR alarm setpoint) is left unimplemented on
// purpose: it changes a protective setting the operator chose, and we have
// no reason to touch it.
class LpMeter : public QObject {
    Q_OBJECT
public:
    enum class Mode { Unknown = -1, Average = 0, PeakHold = 1, Tune = 2 };

    struct Reading {
        double  watts    = 0.0;
        double  swr      = 1.0;
        double  dbm      = 0.0;
        double  zOhm     = 0.0;      // |Z| magnitude
        double  phaseDeg = 0.0;      // signed impedance angle
        double  rOhm     = 0.0;      // derived: |Z| cos(phase)
        double  xOhm     = 0.0;      // derived: |Z| sin(phase)
        bool    zValid   = false;    // false => zOhm/phase/r/x are garbage
        Mode    mode     = Mode::Unknown;
        int     range    = 0;        // 0 = 750 W, 1 = 125 W, 2 = 25 W
        QString callsign;            // as programmed into the meter
        qint64  tsMs     = 0;
    };

    explicit LpMeter(QObject* parent = nullptr);
    ~LpMeter() override;

    // Opens the port and starts polling. pollMs 50..5000 (the meter's own
    // control panel offers that range; 100 ms is a calm 10 Hz and the
    // round trip measured 10.5 ms, so there is plenty of headroom).
    bool start(const QString& device, int pollMs = 100);
    void stop();

    bool isOpen() const;
    // A well-formed frame arrived recently enough to trust. Everything that
    // consumes the meter should ask this before preferring it to the radio.
    bool isAlive() const { return alive_; }
    const Reading& last() const { return last_; }

    // Drive the meter to a display mode, seeking with 'F'. Emits
    // modeSeekFailed() if it will not land (meter unplugged mid-seek, or a
    // firmware that ignores 'F') so the caller can ask the operator to turn
    // the knob. Mode::Unknown cancels an in-flight seek.
    void seekMode(Mode target);
    Mode mode() const { return last_.mode; }

    // Decode one 43-byte frame. Static and public so the wire format is
    // unit-testable (lptest) without a meter or a serial port on the bench.
    static bool parseFrame(const QByteArray& f, Reading& out);

signals:
    void reading(const ttc::LpMeter::Reading& r);
    void aliveChanged(bool alive);          // connection came up / went away
    void modeSeekFailed(ttc::LpMeter::Mode target);
    void error(const QString& what);

private:
    void poll();
    void onBytes(const QByteArray& chunk);
    void setAlive(bool a);

    SerialPort* port_   = nullptr;
    QTimer*     tick_   = nullptr;
    QByteArray  rx_;
    Reading     last_;
    bool        alive_  = false;
    int         pollMs_ = 100;
    qint64      lastGoodMs_ = 0;

    // Mode seek state. Each 'F' needs settling time before the next frame
    // reports the new mode; pressing faster than that double-steps and
    // overshoots the target.
    Mode        seekTarget_  = Mode::Unknown;
    qint64      lastPressMs_ = 0;
    int         presses_     = 0;
};

} // namespace ttc
