// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace ttc {

class CtyLookup;
class LogDb;
class LogbookIndex;
class QrzLookup;
class RotorLink;

// New-QSO entry: the cqrlog flow, in-console. A spot click (or typing)
// fills the call; country/zones/bearing come from cty.dat, worked-before
// and the country/band/mode needed badges from LogbookIndex, frequency and
// mode ride in from the rig, and the SP/LP buttons turn the rotor the
// console already owns. LOG writes the station's SQLite log.
class LogWindow : public QDialog {
    Q_OBJECT
public:
    LogWindow(LogDb* db, LogbookIndex* idx, const CtyLookup* cty,
              RotorLink* rotor, QrzLookup* qrz, QWidget* parent = nullptr);

    // From a spot click / CW decoder: set the station being worked.
    void prefill(const QString& call, const QString& park = QString(),
                 const QString& grid = QString());
    // Rig state (dial + ADIF-style mode text, e.g. "CW"/"SSB").
    void setRig(qint64 freqHz, const QString& mode);
    // Re-emit dxLocated for the current call (a globe that just opened).
    void announceDx() { updateRotor(); }

signals:
    void qsoLogged(qint64 id, const QString& call);
    // The DX in the window has a location — rose and globe follow.
    void dxLocated(double lat, double lon, const QString& call);
    void globeRequested();                 // the window's Globe button

private:
    void onCallEdited();
    void logNow();
    void clearForNext();
    void updateBadges();
    void updateRotor();
    void tickClock();

    LogDb* db_;
    LogbookIndex* idx_;
    const CtyLookup* cty_;
    RotorLink* rotor_;
    QrzLookup* qrz_;

    QLineEdit* call_ = nullptr;
    QLabel* country_ = nullptr;
    QLabel* bCountry_ = nullptr;
    QLabel* bBand_ = nullptr;
    QLabel* bMode_ = nullptr;
    QLineEdit* freq_ = nullptr;
    QLineEdit* mode_ = nullptr;
    QLineEdit* rstS_ = nullptr;
    QLineEdit* rstR_ = nullptr;
    QLineEdit* date_ = nullptr;
    QLineEdit* time_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* qth_ = nullptr;
    QLineEdit* grid_ = nullptr;
    QLineEdit* pota_ = nullptr;
    QLineEdit* comment_ = nullptr;
    QLabel* heading_ = nullptr;
    QPushButton* spBtn_ = nullptr;
    QPushButton* lpBtn_ = nullptr;
    QTableWidget* prev_ = nullptr;
    QTimer* clock_ = nullptr;

    double spAz_ = -1.0, lpAz_ = -1.0;
    bool autoTime_ = true;
    QString lastAutoMode_;                 // last rig mode we applied
};

} // namespace ttc
