// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QQueue>
#include <QString>
#include "cw/CwKeyer.h"

class QTimer;

namespace ttc {

class RadioController;

// Keys CW through the radio's own keyer over CAT ("/c"), for an operator
// who has an Orion but no WinKeyer. The rig's keyer generates the
// elements, so the CW itself has the radio's own timing and weighting —
// this class only decides WHEN each character is handed over.
//
// That decision is the whole design, and it is forced by what the radio
// actually does (measured on-air 2026-08-23, operator copy):
//
//   * one character per command — "/abc" sends only 'a'
//   * the radio BUFFERS whatever it is given
//   * *TU does NOT abort a send in flight
//   * a burst starves the CAT link (6 of 20 queries answered)
//
// Hand it a whole macro and every character of it WILL go out: there is
// no stop, and the S-meter and dial go blind meanwhile. So the queue
// lives here and characters are released one at a time. That is the only
// thing standing between the operator and a macro he cannot interrupt —
// STOP, Esc and the paddle all depend on it.
//
// The release is deliberately a little EARLY (kLeadMs before the
// character should have finished). Early is self-correcting: the radio
// buffers the next character and its own keyer emits the inter-character
// gap exactly, so the timing stays the rig's rather than ours. Late would
// stretch every gap and sound like bad Farnsworth. The cost of the lead
// is that at most one extra character is committed when STOP lands.
class OrionKeyer : public CwKeyer {
    Q_OBJECT
public:
    explicit OrionKeyer(RadioController* radio, QObject* parent = nullptr);

    const Caps& caps() const override { return kCaps; }
    bool open() override;
    void close() override;
    bool isOpen() const override { return open_; }
    QString lastError() const override { return err_; }

    void setSpeed(int wpm) override;
    void send(const QString& text) override;
    void stop() override;
    void tune(bool on) override;
    void backspace() override;

    // No speed pot (no such hardware) and no break-in report: the paddle
    // still wins AT THE RADIO, we simply never hear about it, so the sent
    // display can lag reality after a paddle interrupt. Backspace is real
    // — it edits our own queue, before the character is committed.
    static constexpr Caps kCaps{"Orion keyer", true, false, false, true, 10, 60};

    // How long a character occupies the air at wpm, inter-character gap
    // included. Public so the timing can be tested without a radio.
    static int charMs(QChar c, int wpm);
    static constexpr int kLeadMs = 60;   // release this early; see above

private:
    void releaseNext();

    RadioController* radio_ = nullptr;
    QQueue<QChar> pending_;
    QTimer* timer_ = nullptr;
    bool open_ = false;
    bool busy_ = false;
    int  wpm_ = 25;
    QString err_;
};

} // namespace ttc
