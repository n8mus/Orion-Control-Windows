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
// Characters go out a WHOLE WORD AT A TIME, in a burst.
//
// That is the documented cure, not a guess. The rig queues ~20 characters
// (hamlib's tt565_send_morse, empirically derived) and its own keyer
// spaces them perfectly once it has them; hamlib fires "/c" as fast as
// the port accepts and never reports a spacing problem. The one Windows
// program that uses this command as its primary CW path documents the
// same two cures in its release notes: buffer until a full word is
// typed "so words are transmitted smoothly", and run the local CW clock
// slightly fast so the queue never underruns.
//
// Metering character-by-character — which this class did first, with a
// cushion of one character and then five — gives the queue a chance to
// run dry between every letter, and the rig then has to restart. The
// operator heard exactly that: "5nn mi" as "5 n n m i", "running it all
// t o g e t h e r", identical at 20/30/50 wpm and unchanged by cushion
// depth, while his paddle through the same keyer sounded perfect.
//
// So: burst the word, then wait out its modelled duration before the
// next. Within a word the timing is entirely the rig's and our model
// cannot hurt it; the model only has to be good enough to decide when
// the NEXT word may go. STOP loses at most the word already handed over,
// which the operator ruled acceptable ("few letters before stopping
// would be no issue, correct timing is a big issue").
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
    // display can lag reality after a paddle interrupt.
    //
    // backspace is declared FALSE, which grays out Live keys. It does edit
    // the queue and works on words not yet burst, but the word being sent
    // has already gone in full, so anything just typed is usually already
    // committed. Promising an
    // unsend that usually cannot fire is worse than not offering it —
    // and live keystroke-at-a-time keying is meaningless behind a
    // multi-second buffer anyway.
    static constexpr Caps kCaps{"Orion keyer", false, false, false, true, 10, 60};

    // How long a character occupies the air at wpm, inter-character gap
    // included. Public so the timing can be tested without a radio.
    static int charMs(QChar c, int wpm);
    // The rig's queue depth, from hamlib's Orion backend: "Orion can queue
    // up to about 20 characters". A longer word is split across bursts.
    static constexpr int kMaxBurst = 20;
    // Feed the next word this fraction early, so the queue is refilled
    // before it drains — the "run the CW clock slightly fast" trick.
    static constexpr double kFeedEarly = 0.06;

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
