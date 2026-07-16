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
class QUdpSocket;
class QPlainTextEdit;

namespace ttc {

class WinKeyer;

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
    explicit CwWindow(QWidget* parent = nullptr);

    void setMyCall(const QString& call)  { myCall_ = call; }
    void setHisCall(const QString& call) { hisCall_ = call; }
    void openKeyer();                    // connect + handshake (idempotent)

public slots:
    void appendRx(const QString& text);  // decoded CW from the SDR reader
    void setRxWpm(int wpm);
    void setRxPitch(double hz);          // fldigi-equivalent audio pitch

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
    // Decode-engine adjustments changed (engine, som, deep, attack, decay).
    void rxDecodeConfigChanged(bool eng, bool som, bool deep, int atk,
                               int dcy);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QString substitute(QString t) const; // %mc / %c
    void sendText(const QString& t);
    void editMemory(int i);
    void updateStatus(const QString& s = QString());

    WinKeyer* wk_ = nullptr;
    QUdpSocket* daemon_ = nullptr;       // cwdaemon-protocol server
    QLineEdit* line_ = nullptr;
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
    QPlainTextEdit* rx_ = nullptr;       // decoded-CW readout
    QCheckBox* rxOn_ = nullptr;
    QLabel* rxWpm_ = nullptr;
    int rxWpmVal_ = 0;
    double rxPitchVal_ = -1.0;
    void updateRxInfo();                 // compose "18 WPM · 547 Hz"
    QString myCall_, hisCall_;
    int prevLen_ = 0;                    // live-mode: chars already streamed
};

} // namespace ttc
