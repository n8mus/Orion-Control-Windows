// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QWidget>

namespace ttc {

// The green Thetis-style space-weather block as a sidebar box (it used to
// sit in the waterfall's corner — operator, 2026-10-18): SFI, sunspots,
// A, K, X-ray from NOAA (MainWindow's SolarClient) and a rough HF verdict.
class SolarWidget : public QWidget {
    Q_OBJECT
public:
    explicit SolarWidget(QWidget* parent = nullptr);
    // Values < 0 / empty = unknown ("--").
    void setInfo(int sfi, int aIdx, double kIdx, int ssn, const QString& xray);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {120, sizeHint().height()}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int sfi_ = -1, a_ = -1, ssn_ = -1;
    double k_ = -1.0;
    QString xray_;
};

} // namespace ttc
