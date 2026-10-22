// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <functional>

class QTableWidget;
class QLabel;
class QTimer;

namespace ttc {

class SkimmerEngine;

// Band map: the CW skimmer's finds as a frequency-sorted list — the
// contest-style view of who's where. Rows are colored by worked-before
// status (same scheme as the panadapter labels); clicking a row tunes
// the radio there and prefills the LOG panel with the call. Channels
// still hunting (no call yet) show as dim "?" rows so the machinery is
// visible.
class SkimmerWindow : public QDialog {
    Q_OBJECT
public:
    // status: call+hz -> logbook code ('N'/'B'/'W'/'C'/'?'), wired to
    // LogbookIndex by the owner.
    SkimmerWindow(SkimmerEngine* engine,
                  std::function<QChar(const QString&, qint64)> status,
                  QWidget* parent = nullptr);

signals:
    void tuneTo(qint64 hz, const QString& call);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    void refresh();

    SkimmerEngine* eng_;
    std::function<QChar(const QString&, qint64)> status_;
    QTableWidget* table_ = nullptr;
    QLabel* info_ = nullptr;
    QTimer* tick_ = nullptr;
};

} // namespace ttc
