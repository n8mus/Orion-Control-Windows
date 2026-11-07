// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>
#include <functional>

class QComboBox;
class QDialog;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShortcut;
class QSpinBox;
class QTimer;

namespace ttc {

class CtyLookup;
class LogbookIndex;
class QrzLookup;
class QsoContext;
class RotorLink;
class RttyEngine;
class RttyTuneWidget;

// The RTTY deck (2026-11-07): the CW deck's shape for a RTTY QSO, in the
// same slot under the pan (RTTY PNL button; exclusive with CNTST and CW
// PNL) —
//
//   [ RTTY READ ] [ CALL · NAME · S · R · LOG IT          ] [ RTTY TYPE ]
//   [ mark/space ] [ country · wkd b4          Spot SP LP ]
//                 [ F1 .. F12, two rows of six             ]
//                 [ TX  NARROW   BAUD SHIFT  REV USOS DIDL SQL  LVL ]
//
// Reading is the engine's demodulator on the radio's audio; sending goes
// out as one OVER: an F-key or Enter keys the radio (the main window owns
// PTT), the text plays, PTT drops when the last bit is on the air. The
// TX button held down keeps the key up between lines (a rag-chew); TX
// again or Esc ends it. ←/→ walk the band map's spots from an empty call
// box, F12 wipes, Enter in a report or NAME logs. The QSO is the shared
// QsoContext, as the CW and LOG windows'.
class RttyDeck : public QWidget {
    Q_OBJECT
public:
    RttyDeck(RttyEngine* eng, LogbookIndex* idx, const CtyLookup* cty,
             RotorLink* rotor, QrzLookup* qrz, QWidget* parent = nullptr);

    void setContext(QsoContext* ctx);
    void setRig(qint64 hz, const QString& adifMode);
    void prefillCall(const QString& call, qint64 hz = 0);
    void showTx(bool on);                   // PTT state from the main window
    void showNarrow(bool on);
    void setStatus(const QString& s);

public slots:
    void appendRead(const QString& text);
    void clearForNext();

signals:
    void walkSpots(int dir);
    void logRequested();
    void spotDxRequested(const QString& call, qint64 hz);
    void rotorAimed(double azDeg, double distKm, const QString& label);
    // Key the radio and play `text`; `hold` = stay keyed after it (the TX
    // button is down). The main window keys, plays, and un-keys.
    void sendRequested(const QString& text, bool hold);
    void txHoldChanged(bool on);            // the TX button
    void abortRequested();                  // Esc / STOP
    void narrowRequested(bool on);          // 500 Hz filter on the tones

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUi();
    void keyFkey(int idx0);
    void editFkey(int idx0);
    void applyFkeyLabels();
    QString expand(const QString& text, QString* missing) const;
    void sendText(const QString& text);
    void wipe();
    void stopEverything();
    void refreshInfo();
    void requestQrz(const QString& call);
    void sendTypedLine();
    void pushSettings();
    void pullSettings();
    struct PaneSlot {
        QWidget* pane = nullptr;
        QString title;
        QDialog* fly = nullptr;
        std::function<void(QWidget*)> reinsert;
    };
    QHash<QString, PaneSlot> panes_;
    void addPopout(const QString& key, const QString& title, QWidget* pane,
                   QHBoxLayout* headerRow, std::function<void(QWidget*)> reinsert);
    void floatPane(const QString& key);
    void unfloatPane(const QString& key, bool saveGeom);
    void sizeMenu(QPlainTextEdit* pane, const QString& key);

    RttyEngine* eng_;
    LogbookIndex* idx_;
    const CtyLookup* cty_;
    RotorLink* rotor_;
    QrzLookup* qrz_ = nullptr;
    QsoContext* ctx_ = nullptr;
    bool ctxEcho_ = false;
    bool settingsEcho_ = false;
    QHash<QString, QString> qrzGrid_;
    qint64 rigHz_ = 0;
    QString rigMode_ = "RTTY";
    bool callFromSpot_ = false;
    bool txOn_ = false;
    double spAz_ = -1.0, lpAz_ = -1.0, distKm_ = -1.0;

    QWidget* readPane_ = nullptr;
    QWidget* typePane_ = nullptr;
    QPlainTextEdit* read_ = nullptr;
    QPlainTextEdit* type_ = nullptr;
    RttyTuneWidget* tune_ = nullptr;
    QLabel* sent_ = nullptr;
    QLineEdit* call_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* rstS_ = nullptr;
    QLineEdit* rstR_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* spBtn_ = nullptr;
    QPushButton* lpBtn_ = nullptr;
    QPushButton* fk_[12] = {};
    QPushButton* txBtn_ = nullptr;
    QPushButton* narrowBtn_ = nullptr;
    QComboBox* baud_ = nullptr;
    QComboBox* shift_ = nullptr;
    QPushButton* revBtn_ = nullptr;
    QPushButton* usosBtn_ = nullptr;
    QPushButton* diddleBtn_ = nullptr;
    QPushButton* sqlBtn_ = nullptr;
    QPushButton* afcBtn_ = nullptr;
    QSpinBox* level_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* tick_ = nullptr;
    QList<QShortcut*> shortcuts_;
};

}  // namespace ttc
