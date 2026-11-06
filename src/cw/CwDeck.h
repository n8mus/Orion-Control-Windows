// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>
#include <functional>

class QDialog;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShortcut;
class QSlider;
class QSpinBox;

namespace ttc {

class CtyLookup;
class CwWindow;
class LogbookIndex;
class QrzLookup;
class QsoContext;
class RotorLink;

// The CW deck (operator's design, 2026-11-02): the contest deck's shape,
// cut down to a plain CW QSO. It takes the contest deck's slot under the
// pan (CW PNL button; the two are exclusive there) and reads
//
//   [ CW READ ] [ CALL · NAME · S · R · 0-BEAT · LOG IT ] [ CW TYPE ]
//               [ country · wkd b4         Spot SP LP  ]
//               [ F1 .. F12, two rows of six             ]
//               [ WPM TUNE STOP      FLD SOM DEEP RADIO NR ]
//               [ VOL / PITCH   QSK / RISE   KEYER wpm  (rig, CAT) ]
//               [ read wpm · Hz    WinKeyer…   rig keyer line ]
//
// Day-to-day CW on the contest deck's reflexes: ←/→ walk the band map's
// spots while the call box is empty, ↑/↓ change the keying speed from
// ANY box (the type pane too), F1-F12 key the CW window's own macros,
// F12 wipes, Esc stops keying, Enter in CALL moves to RST R, Enter in a
// report or the name box LOGS. The QSO is the shared QsoContext, so the
// LOG and CW windows see the same call and reports; LOG IT asks the main
// window to write the station log (name/QTH/grid fill from the callbook
// after). The deck owns no keyer and no decoder: every send goes through
// the CW window (single master of the WinKeyer), every decode toggle and
// rig slider is a mirror of that window's own control.
class CwDeck : public QWidget {
    Q_OBJECT
public:
    CwDeck(CwWindow* cw, LogbookIndex* idx, const CtyLookup* cty,
           RotorLink* rotor, QrzLookup* qrz, QWidget* parent = nullptr);

    void setContext(QsoContext* ctx);
    void setRig(qint64 hz, const QString& adifMode);
    void nudgeSpeed(int delta);          // ↑/↓ from the pan or any box
    // A spot click / arrow landing: the call into the box, anchored at
    // the frequency it lives on. The arrows keep walking while the
    // landing is untouched.
    void prefillCall(const QString& call, qint64 hz = 0);
    void setRigCwAvailable(bool on);

public slots:
    void appendRead(const QString& text);   // decoded CW
    void setRxWpm(int wpm);
    void setRxPitch(double hz);
    // The radio's CW settings from the CAT poll (the CW window gets the
    // same report; its sliders are blocked while they follow, so the
    // deck's mirrors need their own copy).
    void showRigSidetoneVol(int pct);
    void showRigSidetonePitch(int hz);
    void showRigQskDelay(int val);
    void showRigAttackDecay(int ms);
    void clearForNext();                    // after a log: boxes, focus

signals:
    void walkSpots(int dir);                // ←/→ on an empty call box
    void logRequested();                    // LOG IT / Enter in a report
    void spotDxRequested(const QString& call, qint64 hz);
    void rotorAimed(double azDeg, double distKm, const QString& label);
    void zeroBeatRequested();
    void winKeyerPanelRequested();

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUi();
    void keyFkey(int idx0);
    void applyFkeyLabels();
    void wipe();                            // F12
    void stopEverything();                  // Esc
    void onCallEdited();
    void refreshInfo();                     // country · wkd b4 · heading
    void requestQrz(const QString& call);
    void sendTypedLine();                   // Enter in the type pane
    void onTypeChanged();                   // live / word keying
    QString currentTypeLine() const;
    void setStatus(const QString& s);
    // Pop-out panes (the contest deck's ⧉): read and type float as their
    // own windows; closing one returns it. Geometry in cwdeck/float/*.
    struct PaneSlot {
        QWidget* pane = nullptr;
        QString title;
        QDialog* fly = nullptr;
        std::function<void(QWidget*)> reinsert;
    };
    QHash<QString, PaneSlot> panes_;
    void addPopout(const QString& key, const QString& title,
                   QWidget* pane, QHBoxLayout* headerRow,
                   std::function<void(QWidget*)> reinsert);
    void floatPane(const QString& key);
    void unfloatPane(const QString& key, bool saveGeom);
    // Right-click text size on a pane (cwdeck/<key>Pt).
    void sizeMenu(QPlainTextEdit* pane, const QString& key);

    CwWindow* cw_;
    LogbookIndex* idx_;
    const CtyLookup* cty_;
    RotorLink* rotor_;
    QrzLookup* qrz_ = nullptr;
    QsoContext* ctx_ = nullptr;
    bool ctxEcho_ = false;
    QHash<QString, QString> qrzGrid_;    // call -> grid (hits only)

    qint64 rigHz_ = 0;
    QString rigMode_ = "CW";
    bool callFromSpot_ = false;          // box filled by walk/click, untouched
    double spAz_ = -1.0, lpAz_ = -1.0, distKm_ = -1.0;

    // widgets
    QWidget* readPane_ = nullptr;
    QWidget* typePane_ = nullptr;
    QPlainTextEdit* read_ = nullptr;
    QPlainTextEdit* type_ = nullptr;
    QLabel* sent_ = nullptr;
    QLineEdit* call_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* rstS_ = nullptr;
    QLineEdit* rstR_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* spBtn_ = nullptr;
    QPushButton* lpBtn_ = nullptr;
    QPushButton* fk_[12] = {};
    QSpinBox* wpm_ = nullptr;
    QPushButton* tuneBtn_ = nullptr;
    QSlider* rigVol_ = nullptr;   QLabel* rigVolVal_ = nullptr;
    QSlider* rigPitch_ = nullptr; QLabel* rigPitchVal_ = nullptr;
    QSlider* rigQsk_ = nullptr;   QLabel* rigQskVal_ = nullptr;
    QSlider* rigRise_ = nullptr;  QLabel* rigRiseVal_ = nullptr;
    QSlider* rigSpeed_ = nullptr; QLabel* rigSpeedVal_ = nullptr;
    QWidget* rigRow_[3] = {};
    QLabel* rigKeyer_ = nullptr;
    QLabel* rxInfo_ = nullptr;
    QLabel* status_ = nullptr;
    int rxWpm_ = 0;
    double rxPitch_ = -1.0;
    QString typeLine_;                   // the line being typed (live/word)
    int typeSent_ = 0;                   // chars of it already keyed (word)
    QList<QShortcut*> shortcuts_;
};

} // namespace ttc
