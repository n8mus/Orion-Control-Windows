// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/TuneBoxWidget.h"

#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <algorithm>

namespace ttc {

namespace {
constexpr int kOuter = 6;         // card inset from the box edge
constexpr int kPad = 9;           // text inset inside the card
}  // namespace

TuneBoxWidget::TuneBoxWidget(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setToolTip("Tuner / amp settings saved for this antenna and band\n"
               "Click: edit or add the setting here\nRight-click: more");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    combo_ = new QComboBox(this);
    combo_->setToolTip("Antenna the tuner settings are filed under\n"
                       "(remembered per band)");
    combo_->setStyleSheet(
        "QComboBox { background: #1c2430; color: #5db2f0; font-weight: bold;"
        " border: 1px solid #2a3644; border-radius: 3px; padding: 2px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }");
    combo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo_->setMinimumContentsLength(8);
    connect(combo_, &QComboBox::activated, this, [this](int i) {
        if (i < 0) return;
        emit antennaChosen(combo_->itemData(i).toString());
    });
}

int TuneBoxWidget::comboBand() const {
    return combo_->sizeHint().height() + 4;
}

void TuneBoxWidget::setAntennas(const QStringList& onBand,
                                const QStringList& others,
                                const QString& current) {
    const QSignalBlocker block(combo_);
    combo_->clear();
    const auto add = [this](const QString& a) {
        combo_->addItem(a.isEmpty() ? QStringLiteral("(unlabeled)") : a, a);
    };
    for (const QString& a : onBand) add(a);
    if (!onBand.isEmpty() && !others.isEmpty()) combo_->insertSeparator(combo_->count());
    for (const QString& a : others) add(a);
    int idx = combo_->findData(current);
    if (idx < 0) { add(current); idx = combo_->count() - 1; }
    combo_->setCurrentIndex(idx);
}

void TuneBoxWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    combo_->setGeometry(kOuter, kOuter, width() - 2 * kOuter,
                        combo_->sizeHint().height());
}

QFont TuneBoxWidget::cardFont() const {
    QFont f = font();
    f.setPixelSize(13);
    f.setBold(true);
    return f;
}

int TuneBoxWidget::rowCount() const {
    // title + "at" line, then the value rows.
    if (box_.title.isEmpty() || box_.empty) return 3;
    int n = 4;                                     // title, at, TUNER, AMP
    if (!box_.swr.isEmpty() || !box_.note.isEmpty()) ++n;
    return n;
}

QSize TuneBoxWidget::sizeHint() const {
    const QFontMetrics fm(cardFont());
    const int lineH = fm.height() + 3;
    return {280, rowCount() * lineH + 2 * kPad + 2 * kOuter + comboBand()};
}

void TuneBoxWidget::setBox(const Box& b) {
    const int rowsBefore = rowCount();
    box_ = b;
    if (rowCount() != rowsBefore) updateGeometry();
    setFixedHeight(sizeHint().height());
    update();
}

void TuneBoxWidget::mousePressEvent(QMouseEvent* e) {
    if (box_.title.isEmpty()) return;              // outside the bands
    if (e->button() == Qt::LeftButton) emit clicked();
    else if (e->button() == Qt::RightButton)
        emit menuRequested(e->globalPosition().toPoint());
}

void TuneBoxWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x12, 0x19, 0x21));
    const QFont f = cardFont();
    p.setFont(f);
    const QFontMetrics fm(f);
    // The sidebar's blue / light-blue / white (operator, 2026-10-18: the
    // amber card read brown beside the other boxes).
    const QColor cTitle(93, 178, 240), cLabel(143, 163, 184),
                 cValue(255, 255, 255), cDim(111, 135, 160),
                 cExact(160, 215, 255);
    const int lineH = fm.height() + 3;
    const QRect card = rect().adjusted(kOuter, kOuter + comboBand(), -kOuter, -kOuter);
    p.fillRect(card, QColor(0x1c, 0x24, 0x30));
    p.setPen(QColor(0x2a, 0x36, 0x44));
    p.drawRect(card.adjusted(0, 0, -1, -1));
    const int x0 = card.left() + kPad, x1 = card.right() - kPad;
    const int labW = std::max(fm.horizontalAdvance("TUNER"),
                              fm.horizontalAdvance("AMP")) + 12;
    int y = card.top() + kPad + fm.ascent() - 2;
    const auto line = [&](const QColor& c, const QString& t, int x = -1) {
        if (x < 0) x = x0;
        p.setPen(c);
        p.drawText(x, y, fm.elidedText(t, Qt::ElideRight, x1 - x));
    };
    if (box_.title.isEmpty()) {
        line(cDim, "Outside the ham bands");
        y += lineH;
        line(cDim, "no tuner settings here");
        return;
    }
    line(cTitle, box_.title);
    y += lineH;
    if (box_.empty) {
        line(cDim, "No settings saved here yet");
        y += lineH;
        line(cDim, "click to add");
        return;
    }
    line(box_.exact ? cExact : cDim, box_.at);
    y += lineH;
    const auto row = [&](const QString& label, const QString& value) {
        line(cLabel, label);
        line(cValue, value, x0 + labW);
        y += lineH;
    };
    row("TUNER", box_.tuner);
    row("AMP", box_.amp);
    if (!box_.swr.isEmpty() || !box_.note.isEmpty()) {
        int x = x0;
        if (!box_.swr.isEmpty()) {
            line(cLabel, "SWR");
            x += labW;
            line(cValue, box_.swr, x);
            x += fm.horizontalAdvance(box_.swr) + 18;
        }
        if (!box_.note.isEmpty() && x < x1) line(cDim, box_.note, x);
    }
}

} // namespace ttc
