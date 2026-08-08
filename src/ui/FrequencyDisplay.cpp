// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/FrequencyDisplay.h"

#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
constexpr uint64_t kMinHz = 100000;      // Orion RX range sanity
constexpr uint64_t kMaxHz = 60000000;
constexpr int kMarginX = 6;
// Cell layout: [10M][1M] . [100k][10k][1k] . [100][10][1]
constexpr uint64_t kPlace[8] = {10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};
} // namespace

int FrequencyDisplay::cellW() const {
    return static_cast<int>(19 * scale_ + 0.5);
}

int FrequencyDisplay::hzCellW() const {
    return static_cast<int>(12 * scale_ + 0.5);
}

int FrequencyDisplay::dotW() const {
    return static_cast<int>(9 * scale_ + 0.5);
}

// x offset of digit cell i (0..7). MHz/kHz cells are cellW wide, the Hz
// group narrower (hzCellW); a decimal separates MHz, a plain space (a
// dotW gap, no glyph) sets off the Hz group — the KE9NS grouping.
int FrequencyDisplay::cellX(int i) const {
    int x = kMarginX;
    for (int j = 0; j < i; ++j)
        x += (j < 5 ? cellW() : hzCellW());
    if (i >= 2) x += dotW();             // decimal point after MHz
    if (i >= 5) x += dotW();             // space before the Hz group
    return x;
}

void FrequencyDisplay::applyGeometry() {
    setFixedSize(cellX(7) + hzCellW() + kMarginX,
                 digitTop_ + static_cast<int>(34 * scale_));
    edit_->setGeometry(QRect(2, digitTop_ + 2, width() - 4,
                             static_cast<int>(28 * scale_)));
}

FrequencyDisplay::FrequencyDisplay(const QString& caption, const QColor& accent,
                                   QWidget* parent)
    : QWidget(parent), caption_(caption), accent_(accent) {
    digitTop_ = caption_.isEmpty() ? 0 : 14;
    setCursor(Qt::PointingHandCursor);

    edit_ = new QLineEdit(this);
    edit_->setAlignment(Qt::AlignCenter);
    edit_->setStyleSheet("QLineEdit { background: #1c2430; color: #e8f0f8;"
                         " border: 1px solid #3f7cb4; font-size: 16px; }");
    edit_->hide();
    connect(edit_, &QLineEdit::returnPressed, this, &FrequencyDisplay::finishEdit);
    connect(edit_, &QLineEdit::editingFinished, this, [this] { edit_->hide(); });
    applyGeometry();
}

void FrequencyDisplay::setBandText(const QString& t) {
    if (t == bandText_) return;
    bandText_ = t;
    update();
}

void FrequencyDisplay::setBadge(const QString& t) {
    if (t == badge_) return;
    badge_ = t;
    update();
}

void FrequencyDisplay::setLargeDigits(bool on) {
    const double s = on ? 1.25 : 1.0;
    if (s == scale_) return;
    scale_ = s;
    applyGeometry();
    update();
}

void FrequencyDisplay::setAccent(const QColor& c) {
    if (c == accent_) return;
    accent_ = c;
    update();
}

void FrequencyDisplay::setFrequency(uint64_t hz) {
    if (edit_->isVisible()) return;      // don't repaint under a type-in
    if (hz == hz_) return;
    hz_ = hz;
    update();
}

void FrequencyDisplay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 16, 22));

    if (!caption_.isEmpty()) {
        QFont cf = p.font();
        cf.setPixelSize(10);
        cf.setBold(true);
        p.setFont(cf);
        p.setPen(QColor(143, 163, 184));
        p.drawText(QRect(kMarginX, 0, width() - kMarginX, digitTop_),
                   Qt::AlignLeft | Qt::AlignVCenter, caption_);
        int rightEdge = width() - kMarginX;
        if (!bandText_.isEmpty()) {                // band label, right-aligned
            p.setPen(QColor(255, 210, 60, 200));
            const int bw = p.fontMetrics().horizontalAdvance(bandText_);
            p.drawText(QRect(rightEdge - bw, 0, bw, digitTop_),
                       Qt::AlignRight | Qt::AlignVCenter, bandText_);
            rightEdge -= bw + 8;
        }
        if (!badge_.isEmpty()) {                   // red chip ("SPLIT")
            const int bw = p.fontMetrics().horizontalAdvance(badge_) + 8;
            const QRect chip(rightEdge - bw, 1, bw, digitTop_ - 3);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(138, 39, 39));
            p.drawRoundedRect(chip, 3, 3);
            p.setPen(QColor(255, 214, 214));
            p.drawText(chip, Qt::AlignCenter, badge_);
        }
    }

    // KE9NS look: bold ITALIC digits, MHz+kHz big, the Hz group smaller
    // and set off by a space (not a dot). The TX VFO (red accent) reads
    // white MHz/kHz with a red Hz group so the live TX frequency pops;
    // an RX VFO stays one calm accent color throughout. Leading zeros
    // aren't drawn (their cells stay live as click/wheel targets).
    QFont big("DejaVu Sans");
    big.setPixelSize(static_cast<int>(27 * scale_));
    big.setBold(true);
    big.setItalic(true);
    QFont small = big;
    small.setPixelSize(static_cast<int>(16 * scale_));

    // Red-dominant accent = the TX VFO (see MainWindow's vfoTxRed).
    const bool isTx = accent_.red() > accent_.green() + 30
                      && accent_.red() > accent_.blue() + 30;
    const QColor bigCol = isTx ? QColor(238, 244, 250) : accent_;

    const QRect row(0, digitTop_, width(), height() - digitTop_ - 2);
    bool leading = true;
    for (int i = 0; i < 8; ++i) {
        const int d = static_cast<int>((hz_ / kPlace[i]) % 10);
        if (d != 0 || i >= 1) leading = false;      // only the 10 MHz digit hides
        if (leading) continue;
        const bool hz = i >= 5;
        p.setFont(hz ? small : big);
        p.setPen(hz ? accent_ : bigCol);            // Hz = accent (red TX/green RX)
        p.drawText(QRect(cellX(i), row.y(), hz ? hzCellW() : cellW(),
                         row.height()),
                   Qt::AlignHCenter | Qt::AlignBottom, QString::number(d));
    }
    // One decimal after MHz; the kHz->Hz break is the cellX space, no glyph.
    p.setFont(big);
    p.setPen(QColor(bigCol.red(), bigCol.green(), bigCol.blue(), 160));
    p.drawText(QRect(cellX(1) + cellW(), row.y(), dotW(), row.height()),
               Qt::AlignHCenter | Qt::AlignBottom, ".");
}

int FrequencyDisplay::digitAt(int x) const {
    for (int i = 0; i < 8; ++i) {
        const int w = i < 5 ? cellW() : hzCellW();
        if (x >= cellX(i) && x < cellX(i) + w) return i;
    }
    return -1;
}

void FrequencyDisplay::bump(int digit, int direction) {
    if (digit < 0) return;
    const int64_t step = static_cast<int64_t>(kPlace[digit]) * direction;
    const int64_t next = static_cast<int64_t>(hz_) + step;
    hz_ = static_cast<uint64_t>(std::clamp<int64_t>(next, kMinHz, kMaxHz));
    update();
    emit frequencyEdited(hz_);
}

void FrequencyDisplay::mousePressEvent(QMouseEvent* e) {
    // Top half of a digit increments that decade, bottom half decrements.
    const int mid = digitTop_ + (height() - digitTop_) / 2;
    bump(digitAt(e->pos().x()), e->pos().y() < mid ? +1 : -1);
}

void FrequencyDisplay::mouseDoubleClickEvent(QMouseEvent*) { beginEdit(); }

void FrequencyDisplay::wheelEvent(QWheelEvent* e) {
    const int steps = e->angleDelta().y() / 120;
    if (steps) bump(digitAt(static_cast<int>(e->position().x())), steps > 0 ? +1 : -1);
    e->accept();
}

void FrequencyDisplay::beginEdit() {
    edit_->setText(QString::number(hz_ / 1e6, 'f', 6));
    edit_->selectAll();
    edit_->show();
    edit_->setFocus();
}

void FrequencyDisplay::finishEdit() {
    bool ok = false;
    const double v = edit_->text().trimmed().toDouble(&ok);
    edit_->hide();
    if (!ok || v <= 0) return;
    // Accept MHz ("14.2"), kHz ("14200") or Hz ("14200000") by magnitude.
    double hz = v;
    if (v < 100.0)          hz = v * 1e6;
    else if (v < 100000.0)  hz = v * 1e3;
    hz_ = static_cast<uint64_t>(std::clamp(hz, double(kMinHz), double(kMaxHz)));
    update();
    emit frequencyEdited(hz_);
}

} // namespace ttc
