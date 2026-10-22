// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include "util/TuneTable.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace ttc {

// One tuner/amp entry, opened by clicking the pan's tuner box. Non-modal
// on purpose: the operator keys a low-power carrier, turns the 238C and
// Titan knobs, types what he set, and hits Save WHILE STILL TRANSMITTING
// so the SWR captured is the one that setting gave. The SWR field follows
// the wattmeter only while it sees a carrier (setLiveSwr is fed nothing
// else) and freezes at key-up — it never drifts while he is not tuning.
class TuneEntryDialog : public QDialog {
    Q_OBJECT
public:
    // antenna is fixed for the dialog's life (it names the table the save
    // lands in); "" = unlabeled. replacesHz: the entry being edited, so a
    // frequency change moves it instead of leaving a copy behind (0 = new).
    TuneEntryDialog(const QString& antenna, const TunePoint& start,
                    qint64 replacesHz, const QString& meterName,
                    QWidget* parent = nullptr);
    void setLiveSwr(double swr);                   // carrier readings only

signals:
    void saveRequested(const QString& antenna, const ttc::TunePoint& pt,
                       qint64 replacesHz);

private:
    void updateBand();
    void updateBypass();
    void updateLive();
    TunePoint collect() const;

    QString         ant_;
    qint64          replacesHz_ = 0;
    QString         meterName_;
    QDoubleSpinBox* freq_   = nullptr;             // MHz, kHz resolution
    QLabel*         band_   = nullptr;
    QCheckBox*      bypass_ = nullptr;
    QDoubleSpinBox* cap_    = nullptr;
    QComboBox*      z_      = nullptr;             // Low Z 1-5, High Z 1-5
    QDoubleSpinBox* ind_    = nullptr;
    QDoubleSpinBox* tune_   = nullptr;
    QDoubleSpinBox* load_   = nullptr;
    QDoubleSpinBox* swr_    = nullptr;
    QLabel*         live_   = nullptr;             // LIVE / held indicator
    QLineEdit*      note_   = nullptr;
    QPushButton*    save_   = nullptr;
    QTimer*         liveTimer_ = nullptr;
    QElapsedTimer   sinceLive_;
};

// Every saved entry for one antenna, all bands or one, sorted by band and
// frequency: edit, delete or QSY to any of them. Owns no data — reads
// the MainWindow's table on refresh() and asks for changes by signal.
class TuneTableDialog : public QDialog {
    Q_OBJECT
public:
    TuneTableDialog(const TuneTable* table, QWidget* parent = nullptr);
    // antennas: every antenna name the station knows (SWR menu list plus
    // whatever the table holds); cur*: what to select on open.
    void setChoices(const QStringList& antennas, const QString& curAnt,
                    const QString& curBand);
    void refresh();                                // table changed elsewhere

signals:
    void editRequested(const QString& antenna, const ttc::TunePoint& pt);
    void addRequested(const QString& antenna);
    void deleteRequested(const QString& antenna, const QString& band,
                         qint64 hz);
    void qsyRequested(qint64 hz);

private:
    QString selectedAnt() const;
    int     selectedRow() const;

    const TuneTable* table_;
    QComboBox*    antSel_  = nullptr;
    QComboBox*    bandSel_ = nullptr;
    QTableWidget* grid_    = nullptr;
    QPushButton*  editBtn_ = nullptr;
    QPushButton*  delBtn_  = nullptr;
    QPushButton*  qsyBtn_  = nullptr;
    QVector<QPair<QString, TunePoint>> rows_;      // band + point per row
};

// Box/table text shared by the pan and the table dialog, so both say the
// same thing the same way ("Cap 6.5", "LoZ 3", "Ind 14").
QString tuneZText(const TunePoint& p);
QString tuneTunerText(const TunePoint& p);         // "Cap 6.5  LoZ 3  Ind 14"
QString tuneAmpText(const TunePoint& p);           // "Tune 5.2  Load 3.8"

} // namespace ttc
