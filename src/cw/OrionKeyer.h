// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QElapsedTimer>
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
// The queue runs AHEAD of the air, keeping about kDepthMs of CW sitting
// in the radio's buffer. That depth is the whole trick: with break-in the
// rig drops back to receive the moment its buffer empties, so a character
// arriving just-in-time has to switch it back to transmit and lands a gap
// on top of the inter-character spacing its keyer already adds. Operator
// copy of the first attempt, which released each character only 60 ms
// early: "cw four sends n 8 e m space between each letter". Keeping the
// buffer non-empty is what makes it a word instead of four letters.
//
// The depth is the abort budget, in the only unit that matters: STOP can
// only lose what the radio has already been handed, so kDepthMs of audio
// is the most that can still go out. Deeper sounds better and stops
// worse.
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
    static constexpr int kDepthMs = 400; // CW kept buffered; see above

private:
    void releaseNext();

    RadioController* radio_ = nullptr;
    QQueue<QChar> pending_;
    QElapsedTimer clock_;
    qint64 airFreeAt_ = 0;               // when the rig's buffer runs dry
    QTimer* timer_ = nullptr;
    bool open_ = false;
    bool busy_ = false;
    int  wpm_ = 25;
    QString err_;
};

} // namespace ttc
