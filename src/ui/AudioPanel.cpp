// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/AudioPanel.h"

#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QTimer>
#include <QToolButton>

namespace ttc {

namespace {
QLabel* caption(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8fa3b8; font-size: 10px; font-weight: bold;");
    return l;
}
constexpr char kColLetter[3] = {'M', 'S', 'B'};    // A / B / A+B columns
} // namespace

QToolButton* AudioPanel::cell(const QString& tip) {
    auto* b = new QToolButton(this);
    b->setCheckable(true);
    // Sized by the box: the three columns share its width.
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setFixedHeight(26);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    return b;
}

// A sidebar box since 2026-10-18 (it was the AUDIO dropdown): the same
// look as the BAND / MODE / RECEIVE boxes beside it — the box's own
// background, 11 px buttons, 10 px captions.
AudioPanel::AudioPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(
        "QWidget { background: transparent; color: #c8d4e0; font-size: 11px; }"
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; color: #c8d4e0; font-size: 11px; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        // Routing cells: A green, B blue, A+B teal — the console's VFO colors.
        "QToolButton[accent=\"a\"]:checked { background: #1f7a45; border-color: #3ecf7a; color: #eafff2; }"
        "QToolButton[accent=\"b\"]:checked { background: #2f6d9e; border-color: #5db2f0; color: #eaf6ff; }"
        "QToolButton[accent=\"ab\"]:checked { background: #2f7d7d; border-color: #4ad0d0; color: #eafcfc; }"
        "QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;"
        " background: #6aa5d8; }");

    auto* g = new QGridLayout(this);
    g->setContentsMargins(8, 6, 8, 8);
    g->setHorizontalSpacing(4);
    g->setVerticalSpacing(4);

    g->addWidget(caption("A", this),   0, 1, Qt::AlignHCenter);
    g->addWidget(caption("B", this),   0, 2, Qt::AlignHCenter);
    g->addWidget(caption("A+B", this), 0, 3, Qt::AlignHCenter);
    static const char* rowName[3] = {"PH L", "PH R", "SPKR"};
    static const char* rowWhat[3] = {"left headphone", "right headphone", "speaker"};
    static const char* colAccent[3] = {"a", "b", "ab"};
    static const char* colWhat[3] = {"VFO A (main RX)", "VFO B (sub RX)", "both receivers"};
    for (int r = 0; r < 3; ++r) {
        g->addWidget(caption(rowName[r], this), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            route_[r][c] = cell(QString("%1 hears %2").arg(rowWhat[r]).arg(colWhat[c]));
            route_[r][c]->setProperty("accent", colAccent[c]);
            g->addWidget(route_[r][c], r + 1, c + 1);
            connect(route_[r][c], &QToolButton::clicked, this, [this, r, c] {
                for (int k = 0; k < 3; ++k)         // one source per output
                    route_[r][k]->setChecked(k == c);
                emitRouting();
            });
        }
    }

    // TX monitor level (sidetone of your own transmit audio) — set-and-forget,
    // so it rides in the popup instead of the TX bar.
    g->addWidget(caption("MON", this), 4, 0);
    mon_ = new QSlider(Qt::Horizontal, this);
    mon_->setRange(0, 100);
    mon_->setToolTip("TX audio monitor level (0 = off)");
    monVal_ = new QLabel("--", this);
    monVal_->setFixedWidth(28);
    monTx_ = new QTimer(this);
    monTx_->setSingleShot(true);
    monTx_->setInterval(40);
    connect(monTx_, &QTimer::timeout, this, [this] {
        if (pendMon_ >= 0) { emit monitorChanged(pendMon_); pendMon_ = -1; }
    });
    connect(mon_, &QSlider::valueChanged, this, [this](int v) {
        monVal_->setText(QString::number(v));
        pendMon_ = v;
        if (!monTx_->isActive()) monTx_->start();
    });
    g->addWidget(mon_, 4, 1, 1, 2);
    g->addWidget(monVal_, 4, 3);

    showRouting('B', 'B', 'B');                     // sane default until ?UC answers
}

void AudioPanel::showMonitor(int pct) {
    const QSignalBlocker b(mon_);
    mon_->setValue(pct);
    monVal_->setText(QString::number(pct));
}

void AudioPanel::emitRouting() {
    if (updating_) return;
    char out[3] = {'B', 'B', 'B'};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (route_[r][c]->isChecked()) out[r] = kColLetter[c];
    emit routingEdited(out[0], out[1], out[2]);
}

void AudioPanel::showRouting(char left, char right, char speaker) {
    const char in[3] = {left, right, speaker};
    updating_ = true;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            route_[r][c]->setChecked(in[r] == kColLetter[c]);
    updating_ = false;
}

} // namespace ttc
