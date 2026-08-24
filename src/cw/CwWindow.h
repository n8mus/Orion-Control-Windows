// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QString>

class QLineEdit;
class QLabel;
class QSpinBox;
class QPushButton;
class QCheckBox;
class QComboBox;
class QSlider;
class QGroupBox;
class QUdpSocket;
class QPlainTextEdit;

namespace ttc {

class CwKeyer;
class RadioController;

// CW sending window (the CWX idea): type-ahead line, four macro memories,
// speed control synced both ways with the WinKeyer pot, TUNE and STOP.
// The paddle always wins — the keyer halts buffered sending in hardware
// the instant it's touched, and we just clear the display.
//
// Macros support %mc (my call, DISPLAY panel) and %c (his call, LOG panel).
// Defaults mirror the operator's cqrlog CW F-keys. Right-click a memory
// button to edit it; texts persist in QSettings (cw/mem1..4).
//
// While the keyer is open the window also serves the cwdaemon UDP protocol
// (default 127.0.0.1:6789): plain text = send, ESC-4 = abort, ESC-2<n> =
// speed. Point cqrlog's CW interface at cwdaemon and both programs can key
// through the one WinKeyer, console arbitrating — same single-master
// pattern as rig control on :4532.
class CwWindow : public QDialog {
    Q_OBJECT
public:
    explicit CwWindow(RadioController* radio = nullptr,
                      QWidget* parent = nullptr);

    void setMyCall(const QString& call)  { myCall_ = call; }
    // Arms the %c macro AND shows the call in the DX box, so every click
    // path (spot, decode pane, skimmer) is visible and correctable.
    void setHisCall(const QString& call);
    void openKeyer();                    // connect + handshake (idempotent)
    bool keyerOpen() const;              // holds the serial port right now?
    // The live backend, for the WinKeyer control panel. Null before the
    // first open; may be an OrionKeyer, so callers must qobject_cast.
    CwKeyer* keyer() const { return keyer_; }

public slots:
    void appendRx(const QString& text);  // decoded CW from the SDR reader
    void setRxWpm(int wpm);
    void setRxPitch(double hz);          // fldigi-equivalent audio pitch

    // The radio's own CW settings, arriving from the CAT poll. Each one
    // moves its slider without echoing a set back down the link.
    void showRigSidetoneVol(int pct);
    void showRigSidetonePitch(int hz);
    void showRigQskDelay(int val);
    void showRigAttackDecay(int ms);
    void showRigKeyerSpeed(int wpm);     // the RADIO's keyer, read-only —
    void showRigKeyerWeight(int pct);    // the WinKeyer owns our speed
    void showRigKeyerEnabled(bool on);
    void setRigCwAvailable(bool on);     // gray out on a radio without them
    // Re-read the keyer choice (Station setup just changed it). Applying
    // only on the next window open was a trap: the operator selects the
    // radio's keyer, nothing changes, and there is no way to tell why.
    void reloadKeyer();

signals:
    // True while the window is visible with RX decode checked — gates the
    // IQ-side decoder so it costs nothing when the window is closed.
    void rxDecodeWanted(bool on);
    void rxSourceChanged(bool radioAudio);  // RADIO src box toggled
    // The operator just asked the keyer to make RF (send/tune/macro/
    // cwdaemon) — the TX monitor can drop SDR gain BEFORE the first
    // element instead of reacting to its overload.
    void txImminent();
    void rxNrChanged(bool on);              // RNNoise toggle (RADIO source)
    void zeroBeatRequested();               // 0-BEAT button (Z lives here now)
    // Double-click on a callsign-shaped token in the decode pane: the
    // fldigi move — copy says who they are, one gesture logs them.
    void callDoubleClicked(const QString& call);
    // A hand-typed DX call is finished: it rides the same rails as a
    // clicked one (cqrlog New QSO prefill). Fires when the operator leaves
    // the box, presses Enter, or spends %c on the air — never per
    // keystroke, and never twice for the same call.
    void hisCallEntered(const QString& call);
    // Decode-engine adjustments changed (engine, som, deep, attack, decay).
    void rxDecodeConfigChanged(bool eng, bool som, bool deep, int atk,
                               int dcy);
    // Noise squelch changed (fldigi metric gate, 0..40 on the SQL slider).
    void rxSquelchChanged(int sql);
    // Radio-side CW settings the operator dragged. Throttled — the CAT
    // link is shared with the poll rotation, so a drag must not flood it.
    void rigSidetoneVolChanged(int pct);
    void rigSidetonePitchChanged(int hz);
    void rigQskDelayChanged(int val);
    void rigAttackDecayChanged(int ms);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;  // rx_ dbl-click

private:
    QString substitute(QString t) const; // %mc / %c
    void sendText(const QString& t);
    void editMemory(int i);
    void updateStatus(const QString& s = QString());
    void tintDxCall();                   // amber until it looks like a call
    void announceHisCall();              // hand a typed call to cqrlog, once
    void sendDaemonText(const QString& t);  // cwdaemon prosign convention
    void updateRigKeyerLine();           // "rig keyer: off · 29 wpm · wt 119"
    // Backend selection. Which engine is live is a TX-safety
    // question, not a preference: see applyKeyerChoice().
    QString resolveKeyerKind() const;
    void applyKeyerChoice();
    void wireKeyer();
    void applyKeyerCaps();

    CwKeyer* keyer_ = nullptr;      // WinKeyer, Orion keyer, or none
    QString keyerKind_;             // "winkeyer" | "radio" | "none"
    RadioController* radio_ = nullptr;
    QUdpSocket* daemon_ = nullptr;       // cwdaemon-protocol server
    QUdpSocket* feed_ = nullptr;         // decode-text feed (localhost UDP)
    quint16 feedPort_ = 2336;            // cw/feedPort — Not1MM dock listens
    QLineEdit* line_ = nullptr;
    QLineEdit* dxCall_ = nullptr;        // the station being worked (%c)
    QLabel* status_ = nullptr;
    QLabel* sentView_ = nullptr;         // what's queued/sent this over
    QSpinBox* wpm_ = nullptr;
    QPushButton* mem_[4] = {};
    QPushButton* tuneBtn_ = nullptr;
    QCheckBox* live_ = nullptr;          // stream keystrokes as typed
    QCheckBox* word_ = nullptr;          // space bar releases each word
    QCheckBox* fldEng_ = nullptr;        // fldigi decode engine on/off
    QCheckBox* som_ = nullptr;           // fuzzy character matching
    QCheckBox* deep_ = nullptr;          // weak-signal narrow filter
    QCheckBox* radioSrc_ = nullptr;      // decode from SignaLink audio
    QCheckBox* nr_ = nullptr;            // RNNoise ahead of the decoder
    QComboBox* atk_ = nullptr;           // tracker attack speed
    QComboBox* dcy_ = nullptr;           // tracker decay speed
    QSlider*   sql_ = nullptr;           // noise squelch (metric gate)
    QPlainTextEdit* rx_ = nullptr;       // decoded-CW readout
    QCheckBox* rxOn_ = nullptr;
    QLabel* rxWpm_ = nullptr;
    int rxWpmVal_ = 0;
    double rxPitchVal_ = -1.0;
    void updateRxInfo();                 // compose "18 WPM · 547 Hz"
    // The radio's own CW settings over CAT (Orion *CV/*CT/*CQ/*CD). Rig-
    // side, so they apply with the WinKeyer doing the keying.
    QGroupBox* rigBox_    = nullptr;
    QSlider* rigVol_      = nullptr;     QLabel* rigVolVal_   = nullptr;
    QSlider* rigPitch_    = nullptr;     QLabel* rigPitchVal_ = nullptr;
    QSlider* rigQsk_      = nullptr;     QLabel* rigQskVal_   = nullptr;
    QSlider* rigRise_     = nullptr;     QLabel* rigRiseVal_  = nullptr;
    QLabel*  rigKeyer_    = nullptr;     // the radio's keyer, read-only
    int rigKeyerWpm_ = 0, rigKeyerWt_ = 0, rigKeyerOn_ = -1;

    QString myCall_, hisCall_;
    QString pushedCall_;                 // last call handed to cqrlog
    int prevLen_ = 0;                    // live-mode: chars already streamed
};

} // namespace ttc
