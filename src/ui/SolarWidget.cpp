// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SolarWidget.h"

#include <QPainter>

namespace ttc {

namespace {
constexpr int kOuter = 6, kPad = 8;
QFont solarFont(const QFont& base) {
    QFont f = base;
    f.setPixelSize(12);
    f.setBold(true);
    return f;
}
}  // namespace

SolarWidget::SolarWidget(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(sizeHint().height());
    setToolTip("Space weather from NOAA (DISPLAY → Solar data)");
}

QSize SolarWidget::sizeHint() const {
    const QFontMetrics fm(solarFont(font()));
    return {280, 3 * (fm.height() + 2) + 2 * kPad + 2 * kOuter};
}

void SolarWidget::setInfo(int sfi, int aIdx, double kIdx, int ssn, const QString& xray) {
    sfi_ = sfi;
    a_ = aIdx;
    k_ = kIdx;
    ssn_ = ssn;
    xray_ = xray;
    update();
}

void SolarWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x12, 0x19, 0x21));
    const QFont f = solarFont(font());
    p.setFont(f);
    const QFontMetrics fm(f);
    const QRect card = rect().adjusted(kOuter, kOuter, -kOuter, -kOuter);
    p.fillRect(card, QColor(4, 12, 6));
    p.setPen(QColor(30, 90, 45));
    p.drawRect(card.adjusted(0, 0, -1, -1));
    const auto num = [](int v) { return v >= 0 ? QString::number(v) : QString("--"); };
    const QString l1 = QString("SFI %1     SSN %2").arg(num(sfi_), num(ssn_));
    const QString l2 = QString("A %1     K %2").arg(
        num(a_), k_ >= 0 ? QString::number(k_, 'f', 1) : QString("--"));
    const QString l3 = QString("X-RAY %1").arg(xray_.isEmpty() ? QString("--") : xray_);
    // Rough HF-conditions verdict from the same numbers (flux feeds the
    // ionosphere, K wrecks it): worth a glance before calling CQ.
    QString cond;
    QColor condC;
    if (sfi_ < 0) {
        cond = "no data yet";
        condC = QColor(90, 105, 120);
    } else if (k_ >= 5.0 || sfi_ < 75) {
        cond = "COND POOR";
        condC = QColor(235, 80, 60);
    } else if (sfi_ >= 110 && k_ >= 0.0 && k_ <= 3.0) {
        cond = "COND GOOD";
        condC = QColor(80, 230, 120);
    } else {
        cond = "COND FAIR";
        condC = QColor(240, 190, 60);
    }
    const int lineH = fm.height() + 2;
    const int x0 = card.left() + kPad, x1 = card.right() - kPad;
    int y = card.top() + kPad + fm.ascent() - 1;
    p.setPen(QColor(80, 230, 120));
    p.drawText(x0, y, l1);
    y += lineH;
    p.drawText(x0, y, l2);
    y += lineH;
    p.drawText(x0, y, l3);
    p.setPen(condC);
    p.drawText(x1 - fm.horizontalAdvance(cond), y - lineH, cond);
}

} // namespace ttc
