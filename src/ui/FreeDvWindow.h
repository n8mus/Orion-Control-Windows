// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <functional>

#include "net/FreeDvReporter.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QSlider;
class QTableWidget;
class QTimer;

namespace ttc {

// The FDV mode's one window: everything FreeDV in one place, laid out
// like freedv-gui (its main window's sync/SNR + Analog + TX level, and its
// FreeDV Reporter window's station list) — but inside the console, so a
// station is a double-click away (QSY), coloured by worked-before, a
// right-click from the rotor, and a heard call one click from the log.
//
//   status strip   SYNC light, SNR bar, offset, RX/TX, last call heard,
//                  modem output level (to the SignaLink)
//   controls       Analog, TX level, Report my station, my status message,
//                  the website
//   station list   qso.freedv.org's live table, this band or all bands
//   heard log      every end-of-over callsign this session -> Log QSO
class FreeDvWindow : public QDialog {
    Q_OBJECT
public:
    explicit FreeDvWindow(QWidget* parent = nullptr);

    struct Status {
        bool sync = false, tx = false;
        float snrDb = 0, freqOffsetHz = 0;
        QString lastCall;
        float modemOutDb = -120;                   // what goes to the SignaLink
        float micDb = -120;                        // mic peak into the RADE encoder
    };
    void setStatus(const Status& s);
    void setStations(const QVector<FreeDvReporter::StationInfo>& st, qint64 dialHz);
    void addHeard(const QString& call, int snrDb, qint64 hz);
    void setControls(bool analog, int txLevelDb, bool report, const QString& message);
    // The SDR receive volume + MUTE (the same setting as SDR ▾ → Receive
    // audio's; no emit).
    void setVolume(int db, bool mute);
    void setReportingLive(bool live, const QString& who);   // "reporting as N8EM (EN83al)"
    // 'N' new, 'B' new band, 'W' worked, 'C' confirmed, '?' unknown.
    void setStatusFor(std::function<char(const QString&, qint64)> f) { statusFor_ = std::move(f); }
    void setMyGrid(const QString& grid) { myGrid_ = grid; }

signals:
    void analogChanged(bool on);
    void volumeChanged(int db, bool mute);
    void txLevelChanged(int db);
    void reportChanged(bool on);
    void statusMessageSent(const QString& text);
    void qsyTo(qint64 hz, const QString& call);
    void lookupCall(const QString& call);
    void turnRotor(double azDeg);
    void requestQsy(const QString& sid, const QString& call);
    void logQso(const QString& call, const QString& grid, qint64 hz, int snrDb);

private:
    void rebuild();
    QString gridFor(const QString& call) const;

    QLabel* sync_ = nullptr;
    QProgressBar* snr_ = nullptr;
    QLabel* snrText_ = nullptr;
    QLabel* foff_ = nullptr;
    QLabel* trx_ = nullptr;
    QLabel* last_ = nullptr;
    QProgressBar* modem_ = nullptr;
    QProgressBar* mic_ = nullptr;
    QLabel* micText_ = nullptr;
    float micShown_ = -120.f, micHold_ = -120.f;
    int micHoldTicks_ = 0, clipTicks_ = 0;
    QCheckBox* analog_ = nullptr;
    QSlider* vol_ = nullptr;
    QLabel* volText_ = nullptr;
    QCheckBox* mute_ = nullptr;
    QSlider* txLevel_ = nullptr;
    QLabel* txLevelText_ = nullptr;
    QCheckBox* report_ = nullptr;
    QLabel* reportInfo_ = nullptr;
    QLineEdit* msg_ = nullptr;
    QComboBox* bandFilter_ = nullptr;
    QLabel* count_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTableWidget* heard_ = nullptr;
    QTimer* rebuildTimer_ = nullptr;
    QVector<FreeDvReporter::StationInfo> st_;
    qint64 dialHz_ = 0;
    bool dirty_ = false;
    QString myGrid_;
    std::function<char(const QString&, qint64)> statusFor_;
};

} // namespace ttc
