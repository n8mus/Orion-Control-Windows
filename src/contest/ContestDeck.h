// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QPair>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "contest/ContestDb.h"
#include "contest/ContestEngine.h"

class QDialog;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShortcut;
class QSpinBox;
class QTableWidget;

namespace ttc {

class CtyLookup;
class CwWindow;
class PracticeEngine;
class QrzLookup;
class QtcDialog;
class RotorLink;

// The contest deck: the strip that replaces the WATERFALL while contest
// mode is on (the operator's sketch, 2026-09-05). The spectrum above it
// is the band map; this is everything the hands touch per QSO:
//
//   [ CW READ ] [ SUPER CHECK          ] [ CW TYPE ]
//              [ N1MM-style entry+Fkeys ]
//
// Keys are deliberately chord-free: F1–F11 macros (right-click a key to
// edit it), F12 = WIPE always, Enter = ESM, Esc = stop keying, ←/→ walk
// the panadapter spots while the call box is empty, PgUp/PgDn = speed.
// The heavier screens (log grid, setup/resume, Cabrillo, statistics)
// live in the contest manager window behind one button.
class ContestDeck : public QWidget {
    Q_OBJECT
public:
    ContestDeck(ContestDb* db, const CtyLookup* cty, CwWindow* cw,
                RotorLink* rotor, QrzLookup* qrz = nullptr,
                QWidget* parent = nullptr);

    void setRig(qint64 hz, const QString& adifMode);
    void setMasterScp(const QSet<QString>& scp) { scp_ = scp; }
    // Phone contests: {VK1}..{VK4} macros play DVR slots through these.
    // play returns false when nothing went out (slot not recorded) so
    // the ESM beats stay honest.
    void setVoiceKeyer(std::function<bool(int)> play,
                       std::function<void()> stop);
    // ↑/↓ anywhere in contest mode: keying speed (CW contests only).
    void nudgeSpeed(int delta);
    // The call frame: knob-tuned onto a spotted station, its call shows
    // beside CALL and Space (empty box) grabs it. Empty call = no spot
    // near the dial.
    void setNearbySpot(const QString& call, char cls, qint64 hz);
    bool scpHas(const QString& call) const { return scp_.contains(call); }
    bool openContestId(qint64 id);
    bool contestActive() const { return contestId_ >= 0; }
    qint64 currentContestId() const { return contestId_; }
    void closeContest();             // the open contest was deleted

    // Contest coloring for a spot: 'M' new mult, 'N' new, 'W' worked/
    // dupe, 'Z' zero points. 0 = leave the normal colors. hz picks the
    // band the spot LIVES on (the cross-band spot table's whole point);
    // 0 = the band under the dial.
    char classifySpot(const QString& call, qint64 hz = 0) const;
    // "CW" / "SSB" / "MIXED" — the running contest's mode category, for
    // the spot-feed mode fence. Empty when no contest is open.
    QString contestModeCategory() const {
        return def_ ? def_->modeCategory : QString();
    }

public slots:
    void appendRead(const QString& text);   // decoded CW rides in here
    // Spot click / arrow landing / Space grab. hz anchors the call at
    // the frequency it lives on (QSY-abandon detection); 0 = the dial.
    void prefillCall(const QString& call, qint64 hz = 0);

signals:
    void walkSpots(int dir);                // ←/→ on an empty call box
    void openManagerRequested();            // "Contest log…"
    // A TYPED, unworked call abandoned by turning the knob — parked as
    // a local spot at the frequency it was heard on (N1MM's "QSYing
    // wipes the call and spots it in the bandmap").
    void callParked(const QString& call, qint64 hz);
    // Spot button: send the entered call up to the cluster.
    void spotDxRequested(const QString& call, qint64 hz);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void buildUi();
    void openContest(qint64 id);
    void rebuildEntryFields();
    void refreshAll();
    void enterPressed();                    // ESM
    void execPlan(const QList<EsmAct>& plan, bool updateOnly);
    void updateEsmHint();
    void logNow();
    void wipe();                            // F12
    // Cursor to the field that needs COPY: first empty required field,
    // else the first non-RST field. Space once dumped the operator into
    // the preset 59 box and his age landed there — AGE stayed empty and
    // Enter refused all afternoon (the JI2MED trace).
    void focusExchange();
    void flashRefusal(QLineEdit* field, const QString& msg);
    void keyFkey(int idx0);
    void editFkey(int idx0);                // right-click editor
    QString fkeySpec(int key) const;        // override -> def fallback
    void applyFkeyLabels();
    void onCallEdited();
    void autoFillExch();             // QRZ zone -> the ZONE field
    void verifyExch();               // amber-flag a zone that mismatches
    void historyPrefill();                  // space in the call box
    void refreshScp();
    void keyText(const QString& text);
    void stopEverything();           // Esc: dump keyer/voice, kill auto-CQ
    // PRACTICE mode (Morse-Runner-in-the-deck). While on, keyText routes
    // to the simulator (the transmitter NEVER keys), logNow verifies
    // against the sim's truth (contest.db is NEVER written), spots and
    // QRZ lookups are fenced off. The four isolation guarantees live at
    // those choke points — keep them there.
    void setPractice(bool on);
    bool practiceOn() const;
    void practiceVerdict();          // practice-mode logNow
    CQsoValues collectValues() const;
    // True heading: QRZ grid > call-history grid > entity centre, and
    // the label SAYS which one is on screen (the "rose stuck at 228°"
    // evening was an unlabeled centroid, not a bug).
    void updateHeading();
    void requestQrz(const QString& call);
    QString currentBand() const;
    QString modeNow() const;         // rig mode as the contest mode
    QString rstPreset() const;       // "599" CW, "59" phone
    void trace(const QString& line);

    ContestDb* db_;
    const CtyLookup* cty_;
    CwWindow* cw_;
    RotorLink* rotor_;
    QrzLookup* qrz_ = nullptr;
    QHash<QString, QString> qrzGrid_;    // call -> grid (hits only)
    QHash<QString, int> qrzCqz_;         // call -> QRZ CQ zone (explicit/derived)
    QHash<QString, int> qrzItuz_;        // call -> QRZ ITU zone
    QSet<QString> qrzAsked_;             // in flight / answered — don't re-ask
    QTimer qrzTimer_;                    // debounce while typing

    qint64 contestId_ = -1;
    const ContestDef* def_ = nullptr;
    ContestRow row_;
    ContestContext ctx_;
    QList<ContestQso> qsos_;
    QList<CQsoValues> values_;
    ScoreBreakdown sb_;
    QSet<QString> scp_;
    bool runMode_ = true;
    bool esmOn_ = true;
    bool myCallSent_ = false, exchSent_ = false;
    bool callFromSpot_ = false;      // box filled by walk/click, untouched
    QString confirmPending_;         // no-country call awaiting 2nd Enter
    bool exchAutoLock_ = false;      // operator overtyped an auto-fill
    int  expectedCqz_ = 0, expectedItuz_ = 0;  // QRZ zones for the call
    double myLat_ = 0, myLon_ = 0;
    int hdg_ = -1;

    qint64 rigHz_ = 0;
    QString rigMode_ = "CW";

    // Pop-out panes: CW READ, CW TYPE and LAST QSOs each carry a ⧉
    // button — the pane floats as its own window to size and park
    // anywhere; closing it returns it to the deck. State + geometry
    // persist (contest/float/*).
    struct PaneSlot {
        QWidget* pane = nullptr;
        QString title;
        QDialog* fly = nullptr;
        std::function<void(QWidget*)> reinsert;
        bool allowed = true;         // phone hides the CW panes
    };
    QHash<QString, PaneSlot> panes_;
    void addPopout(const QString& key, const QString& title,
                   QWidget* pane, QHBoxLayout* headerRow,
                   std::function<void(QWidget*)> reinsert);
    void floatPane(const QString& key);
    void unfloatPane(const QString& key, bool saveGeom);
    void applyPaneVisibility();

    // widgets
    QWidget* readPane_ = nullptr;    // CW READ column (hidden on phone)
    QWidget* typeTop_ = nullptr;     // CW TYPE box (hidden on phone)
    QWidget* logPane_ = nullptr;     // LAST QSOs wrapper
    QPlainTextEdit* read_ = nullptr;
    QWidget* scpRow_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* clock_ = nullptr;
    QPushButton* runBtn_ = nullptr;
    QPushButton* spBtn_ = nullptr;
    QPushButton* autoBtn_ = nullptr;     // AUTO CQ toggle
    QSpinBox* autoSecs_ = nullptr;       // seconds between CQ starts
    QTimer autoCqTimer_;
    bool autoPaused_ = false;            // typing pauses; log/wipe resumes
    QLabel* frameLbl_ = nullptr;         // knob-tune call frame
    QString frameCall_;
    qint64 frameHz_ = 0;
    // Where the call in the box was ACQUIRED (typed, grabbed, landed).
    // Rolling >1 kHz from here means abandoned: typed calls park,
    // spot-sourced calls just clear (their spot is already on the map).
    qint64 anchorHz_ = 0;
    QSpinBox* wpm_ = nullptr;
    QLineEdit* call_ = nullptr;
    QLineEdit* rstS_ = nullptr;
    QLabel* sentNr_ = nullptr;
    QWidget* fieldsBox_ = nullptr;
    QList<QPair<ExchCol, QLineEdit*>> edits_;
    QLabel* dupe_ = nullptr;
    QLabel* info_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* hdgLbl_ = nullptr;
    QLabel* hdgSrcLbl_ = nullptr;        // "HDG · QRZ" / "hist" / "cty ctr"
    QPushButton* fk_[12] = {};
    QLineEdit* type_ = nullptr;
    QLabel* sent_ = nullptr;
    QTableWidget* lastLog_ = nullptr;    // the last few QSOs, always on
    QLabel* status_ = nullptr;
    QPushButton* qtcBtn_ = nullptr;      // WAE only
    QtcDialog* qtc_ = nullptr;           // lazy
    std::function<bool(int)> playVk_;    // 0-based DVR slot; false = silent
    std::function<void()> stopVoice_;
    QPushButton* esmBtn_ = nullptr;      // Enter-sends-message toggle
    QPushButton* pracBtn_ = nullptr;     // PRACTICE toggle (amber when on)
    PracticeEngine* practice_ = nullptr;
    QList<QShortcut*> shortcuts_;
    QTimer clockTimer_;
};

} // namespace ttc
