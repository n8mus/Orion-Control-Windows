// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "radio/TxMeter.h"

#include <QByteArray>

class QTimer;

namespace ttc {

class SerialPort;

// TelePost LP-100A digital vector RF wattmeter.
//
// Protocol (115200 8N1, no flow control, straight-through DB9 M-F — NOT a
// null modem). There is no streaming mode: you poll and the meter answers
// with exactly 43 bytes starting with ';' and NO terminator, so framing is
// by leading ';' + length. Nine CSV fields:
//
//   ;<watts>,<|Z|>,<phase>,<alarm>,<call>,<range>,<mode>,<dBm>,<SWR>
//   ;0000.00,052.1,083.6,0,N8EM  ,2,1,-2.3,1.00      <- live idle frame
//
// Fixed-offset tables for this frame circulate on the net and are off by
// one on the power field; splitting on ',' is both simpler and right.
//
// The poll command is ";P?". That is the form TelePost's own Virtual
// Control Panel and LP100_Plot use — both VB6 binaries carry the literal
// ";P?" and nothing else for the meter — so it is the best-tested path
// across firmware revisions. A bare "P" also works (verified on the
// operator's meter, thousands of polls, zero malformed frames), but there
// is no reason to prefer our discovery over the vendor's own wire format.
//
// ";X?" appears to be a QUERY form: ";F?" sent three times did NOT move
// the mode field, while a bare "F" cycles it every time. So the trailing
// '?' asks rather than acts. Other query literals in the VCP binary are
// ";A?" (alarm) and ";C" (a prefix, set-callsign). Notably absent from the
// vendor's entire command set: anything that changes the display page.
//
// At idle the meter still reports |Z| and phase, and they are NOISE — the
// frame above claims 52.1 ohms at 83.6 degrees into a dead radio. Anything
// consuming impedance MUST check zValid, which gates on forward power.
//
// Mode control is the byte 'F', which cycles Average -> PeakHold -> Tune
// -> Average. Bench-verified on the operator's meter 2026-08-01; all three
// transitions observed, and every one of them is visible in field 6, so a
// seek can confirm it landed.
//
// NEVER SEND 'M'. Third-party code documents it as the mode control and it
// is not — it walks the front-panel LCD through its display pages (Normal,
// Vector, dBm, Field Strength), and NONE of that appears in the frame. The
// vendor's own software has no display-page command at all, which is the
// strongest possible hint that 'M' is not part of the sanctioned API. So
// it reads as a harmless no-op in the data while it silently strands the
// meter's face on a page the operator did not choose. That is exactly what
// happened while this protocol was being worked out: three exploratory 'M'
// presses left the operator's meter on Field Strength, and the press count
// never reconciled afterwards — the observed cycle is shorter than the
// manual's five-mode list implies, and some presses appear not to register.
// It took stepping ONE press at a time with the operator watching the LCD
// to get home. The rule this bought: the console only ever touches state it
// can read back. 'A' (cycle the SWR alarm setpoint) is unimplemented for
// the same reason plus a better one — it changes a protective setting the
// operator chose.
class LpMeter : public TxMeter {
    Q_OBJECT
public:
    enum class Mode { Unknown = -1, Average = 0, PeakHold = 1, Tune = 2 };

    // Readings land in the shared TxMeter::Reading. LP-specific notes:
    // Reading::mode carries the Mode enum as an int. Reading::range is the
    // autoranging power scale — 0 = 2500 W, 1 = 250 W, 2 = 25 W, per the
    // range selector in TelePost's own VCP (" 25 " / " 250" / "2500", plus
    // "10K" with the high-power coupler option). Community code calls
    // these 750/125/25, which matches no TelePost coupler spec and is very
    // probably wrong — a decade ladder is what an autoranging meter would
    // have. Not verified under power; nothing in the console depends on it.
    // Reading::valid == Reading::zValid here: both gate on forward power.
    using Reading = TxMeter::Reading;

    explicit LpMeter(QObject* parent = nullptr);
    ~LpMeter() override;

    // Opens the port and starts polling. pollMs 50..5000 (the meter's own
    // control panel offers that range; 100 ms is a calm 10 Hz and the
    // round trip measured 10.5 ms, so there is plenty of headroom).
    bool start(const QString& device, int pollMs = 100) override;
    void stop() override;

    bool isOpen() const;
    // A well-formed frame arrived recently enough to trust. Everything that
    // consumes the meter should ask this before preferring it to the radio.
    bool isAlive() const override { return alive_; }

    // Drive the meter to a display mode, seeking with 'F'. Emits
    // modeSeekFailed() if it will not land (meter unplugged mid-seek, or a
    // firmware that ignores 'F') so the caller can ask the operator to turn
    // the knob. Mode::Unknown cancels an in-flight seek.
    //
    // Currently UNUSED by choice: the console never changes the operator's
    // meter mode (his call, 2026-08-01, after a display-page incident — and
    // TelePost's own Plot program doesn't either). Kept because it is
    // bench-verified and is the correct tool if that decision is ever
    // revisited with evidence, e.g. a sweep taken in Peak Hold coming out
    // visibly smeared.
    void seekMode(Mode target);
    Mode mode() const { return Mode(last_.mode); }

    // Decode one 43-byte frame. Static and public so the wire format is
    // unit-testable (lptest) without a meter or a serial port on the bench.
    static bool parseFrame(const QByteArray& f, Reading& out);

signals:
    void modeSeekFailed(ttc::LpMeter::Mode target);

private:
    void poll();
    void onBytes(const QByteArray& chunk);
    void setAlive(bool a);

    SerialPort* port_   = nullptr;
    QTimer*     tick_   = nullptr;
    QByteArray  rx_;
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
