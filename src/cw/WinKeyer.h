// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>
#include "cw/CwKeyer.h"

#include <optional>

class QSerialPort;
class QTimer;

namespace ttc {

// K1EL WinKeyer USB client (WK2/WK3 protocol, 1200 8N2, DTR on / RTS off).
// The keyer hardware owns everything that matters: element timing, the
// paddle (which instantly interrupts buffered sending — break-in is a
// hardware feature, we just get told), and the speed pot. Protocol per the
// K1EL datasheet, cross-checked against the GPL implementations in cqrlog
// (uCWKeying.pas) and fldigi. We deliberately never write the keyer's mode
// register unless the operator opts in (see setModeRegister) — WK3 cannot
// report those values back, so writing them is one-way.
class WinKeyer : public CwKeyer {
    Q_OBJECT
public:
    explicit WinKeyer(QObject* parent = nullptr);
    ~WinKeyer() override;

    const Caps& caps() const override { return kCaps; }
    bool open() override;                  // from cw/port + cw/potMin/Max
    bool open(const QString& device);      // null + echo test + host open
    void close() override;                 // host close; keyer standalone again
    bool isOpen() const override { return open_; }
    QString lastError() const override { return err_; }

    void setSpeed(int wpm) override;       // host speed (0x02)
    int  speed() const { return wpm_; }
    void setPotRange(int minWpm, int maxWpm);  // 0x05 min range 0

    // Everything that shapes TRANSMITTED CW. Ranges and defaults are the
    // WK3 datasheet Rev 1.3 pp8-14; the console shipped none of these for
    // years, so the keyer ran every one at its factory default — which is
    // why it sounded mechanically exact next to the radio's own keyer.
    //
    // Each is std::optional and UNSET means "never written": the keyer
    // keeps whatever its owner stored, which is the long-standing policy
    // in this file. Touching a control adopts that one parameter and
    // nothing else, and applyOwned() re-sends only the adopted ones on
    // every open (host close reverts the keyer to its stored config).
    void setWeighting(int pct);            // 0x03, 10-90, 50 = no adjustment
    void setKeyComp(int ms);               // 0x11, 0-250 ms added per element
    void setFirstExtension(int ms);        // 0x10, 0-250 ms on the 1st element
    void setRatio(int nn);                 // 0x17, 33-66, 50 = 1:3
    void setFarnsworth(int wpm);           // 0x0D, 0 = off, else 10-99
    void setPttLeadTail(int leadMs, int tailMs);   // 0x04, 0-250 in 10 ms steps
    void setSwitchpoint(int pct);          // 0x12, 10-90 % of a dit
    void setSidetone(int n);               // 0x01, WK1/WK2 table index 1-10
    void setModeRegister(int bits);        // 0x0E — paddle feel; opt-in only
    // X1MODE letterspace, 0-15 in 2% steps. NOT available in WK1 mode (the
    // mode host-open leaves us in), so adopting it also moves the keyer to
    // WK2 mode — which starts pushbutton status bytes we must not mistake
    // for keyer status. Left alone, we stay in WK1 exactly as before.
    void setLetterspace(int steps);

    int  firmwareRev() const { return fwRev_; }   // -1 until a successful open
    bool wk2Mode() const { return wk2_; }
    // WK3 silicon. Host open answers with the major firmware revision and
    // the datasheet pins WK3 at 3x ("31 for rev 31.03"); WK2 shipped 2x.
    // Gates only ADDITIVE features, so an unknown/older chip loses nothing.
    bool isWk3() const { return fwRev_ >= 30; }

    // Re-send every adopted parameter. Called at the end of open(); public
    // so the panel can re-push after the operator reconnects the keyer.
    void applyOwned();
    void loadOwned();                      // adopt the cw/wk/* keys
    void send(const QString& text) override;   // buffered ASCII (uppercased)
    void sendProsign(char a, char b) override; // 0x1B merge
    void stop() override;                  // 0x0A: dump the buffer, key up
    void tune(bool on) override;           // 0x0B: steady carrier
    void backspace() override;             // 0x08: unsend last buffered char

    // Everything the hardware owns outright: element timing, the paddle,
    // and the speed knob. This is the reference the other backends are
    // measured against.
    static constexpr Caps kCaps{"WinKeyer", true, true, true, true, 5, 99,
                               "< AR   > SK   = BT   ( KN   ; AA   ) KK"
                               "   | half-space"};

private:
    void onReadyRead();

    QSerialPort* ser_ = nullptr;
    bool open_ = false;
    bool busy_ = false;
    int  wpm_ = 25;
    int  potMin_ = 7;                      // pot byte is offset from this
    int  potPending_ = -1;                 // latest raw pot report
    int  potEmitted_ = -1;                 // last value actually emitted
    QTimer* potTimer_ = nullptr;           // settle timer (see onReadyRead)
    QString err_;
    int  fwRev_ = -1;                      // from the host-open reply byte
    bool wk2_ = false;                     // WK2 mode entered (letterspace)

    // Adopted parameters — see the setters. Unset = the keyer's own.
    std::optional<int> weight_, keyComp_, firstExt_, ratio_, farns_,
                       pttLead_, pttTail_, switchpoint_, sidetone_,
                       modeReg_, letterspace_;
};

} // namespace ttc
