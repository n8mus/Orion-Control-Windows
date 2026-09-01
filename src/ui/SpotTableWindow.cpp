// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SpotTableWindow.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "net/RotorLink.h"
#include "util/Bearing.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"
#include "util/SpotMode.h"

namespace ttc {

namespace {
enum Col { ColUtc = 0, ColKhz, ColCall, ColC, ColB, ColM, ColAz, ColSpotter,
           ColComment, ColCount };

// The band map's worked-before ramp — same story everywhere.
const char* statusColor(char st) {
    switch (st) {
        case 'N': return "#ff5c46";
        case 'B': return "#ffbe3c";
        case 'W': return "#82de8c";
        case 'C': return "#96a2b2";
        default:  return "#c6d2df";
    }
}
// Needed-status glyph cell: check = confirmed, ring = worked-unconfirmed,
// cross = needed. Text glyphs so the table needs no icon plumbing.
void needCell(QTableWidget* t, int row, int col, QChar st) {
    auto* it = new QTableWidgetItem;
    it->setTextAlignment(Qt::AlignCenter);
    it->setFlags(Qt::ItemIsEnabled);
    switch (st.toLatin1()) {
        case 'C': it->setText(QStringLiteral("✓"));
                  it->setForeground(QColor(130, 222, 140)); break;
        case 'W': it->setText(QStringLiteral("○"));
                  it->setForeground(QColor(255, 190, 60)); break;
        case 'N': it->setText(QStringLiteral("✕"));
                  it->setForeground(QColor(255, 92, 70)); break;
        default:  it->setText(QStringLiteral("·"));
                  it->setForeground(QColor(90, 104, 120)); break;
    }
    it->setData(Qt::UserRole, QString(st));
    t->setItem(row, col, it);
}
} // namespace

SpotTableWindow::SpotTableWindow(LogbookIndex* idx, RotorLink* rotor,
                                 QWidget* parent)
    : QDialog(parent), idx_(idx), rotor_(rotor) {
    setModal(false);
    setWindowTitle("DX Cluster — spot table");
    resize(940, 480);
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 14px; }"
        "QLabel { color: #8fa2b5; font-size: 12px; }"
        "QPushButton { background: #24303e; color: #b8c8d8; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 4px 12px; font-size: 12px;"
        " font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }"
        "QPushButton:checked { background: #2b62c9; border-color: #3a73dd;"
        " color: #ffffff; }"
        "QTableWidget { background: #10161e; color: #c6d2df; border: 1px"
        " solid #2a3644; font-size: 13px; gridline-color: #1a222c;"
        " selection-background-color: #24406b; selection-color: #eef4f9; }"
        "QHeaderView::section { background: #1a222c; color: #8fa2b5;"
        " border: none; padding: 4px 6px; font-size: 11px; }");

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(7);

    auto* chips = new QHBoxLayout;
    chips->setSpacing(6);
    const auto chip = [this, chips](QPushButton** b, const char* text,
                                    const char* tip, bool on) {
        *b = new QPushButton(QLatin1String(text), this);
        (*b)->setCheckable(true);
        (*b)->setChecked(on);
        (*b)->setToolTip(QLatin1String(tip));
        (*b)->setFocusPolicy(Qt::NoFocus);
        connect(*b, &QPushButton::toggled, this, [this] { rebuild(); });
        chips->addWidget(*b);
    };
    chip(&fDx_, "DX", "Classic human cluster spots", true);
    chip(&fPota_, "POTA", "Park activations", true);
    chip(&fFt8_, "FT8", "FT8/FT4 skimmer spots", true);
    chip(&fSkim_, "SKIM", "This station's own CW skimmer", true);
    chip(&fNeed_, "NEED ONLY",
         "Only rows where the country, this band, or this mode is still "
         "needed", false);
    fNeed_->setStyleSheet("QPushButton { color: #ff5c46; }"
                          "QPushButton:checked { background: #8a2727;"
                          " border-color: #e05d5d; color: #ffe8e8; }");
    chips->addStretch(1);
    count_ = new QLabel(this);
    chips->addWidget(count_);
    v->addLayout(chips);

    table_ = new QTableWidget(0, ColCount, this);
    table_->setHorizontalHeaderLabels({"UTC", "KHZ", "CALL", "C", "B", "M",
                                       "~AZ", "SPOTTER", "COMMENT"});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(ColUtc, 52);
    table_->setColumnWidth(ColKhz, 78);
    table_->setColumnWidth(ColCall, 110);
    table_->setColumnWidth(ColC, 28);
    table_->setColumnWidth(ColB, 28);
    table_->setColumnWidth(ColM, 28);
    table_->setColumnWidth(ColAz, 52);
    table_->setColumnWidth(ColSpotter, 90);
    v->addWidget(table_, 1);

    auto* legend = new QLabel(
        "✓ confirmed · ○ worked, unconfirmed · ✕ needed"
        "     az click = rotate · double-click = tune + log", this);
    v->addWidget(legend);

    // Az click turns the rotor; double-click anywhere tunes.
    connect(table_, &QTableWidget::cellClicked, this,
            [this](int row, int col) {
        if (col != ColAz || !rotor_ || !rotor_->connected()) return;
        const auto* it = table_->item(row, ColAz);
        if (!it) return;
        bool ok = false;
        const double az = it->data(Qt::UserRole).toDouble(&ok);
        if (ok) rotor_->turnTo(az);
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
        const auto* it = table_->item(row, ColCall);
        if (!it) return;
        emit spotActivated(it->text(),
                           table_->item(row, ColKhz)->data(Qt::UserRole)
                               .toLongLong(),
                           QChar(it->data(Qt::UserRole).toString().isEmpty()
                                     ? QChar('D')
                                     : it->data(Qt::UserRole).toString()[0]),
                           it->data(Qt::UserRole + 1).toString());
    });

    // The feed pushes on every change; rebuild at most twice a second.
    refresh_ = new QTimer(this);
    refresh_->setInterval(500);
    connect(refresh_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) rebuild();
    });
    refresh_->start();
    if (idx_)
        connect(idx_, &LogbookIndex::updated, this,
                [this] { dirty_ = true; });
}

void SpotTableWindow::setSpots(const QVector<SpotLabel>& spots) {
    spots_ = spots;
    dirty_ = true;
}

void SpotTableWindow::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    dirty_ = true;
    rebuild();
}

QString SpotTableWindow::modeGuess(const SpotLabel& l) const {
    return guessSpotMode(l.kind, l.comment, l.hz);
}

void SpotTableWindow::rebuild() {
    dirty_ = false;
    double myLat = 0, myLon = 0;
    const bool haveMe = CtyLookup::gridToLatLon(
        QSettings().value("station/grid", "EN83al").toString(), myLat, myLon);

    // Newest first.
    QVector<SpotLabel> rows = spots_;
    std::sort(rows.begin(), rows.end(),
              [](const SpotLabel& a, const SpotLabel& b) {
                  return a.atSecs > b.atSecs;
              });

    table_->setUpdatesEnabled(false);
    table_->setRowCount(0);
    int shown = 0;
    for (const SpotLabel& l : rows) {
        if (l.kind == 'D' && !fDx_->isChecked()) continue;
        if (l.kind == 'P' && !fPota_->isChecked()) continue;
        if (l.kind == 'F' && !fFt8_->isChecked()) continue;
        if (l.kind == 'S' && !fSkim_->isChecked()) continue;
        const QString band = LogbookIndex::bandForHz(l.hz);
        const QString mode = modeGuess(l);
        const LogbookIndex::Need nd =
            idx_ ? idx_->need(l.call, band, mode) : LogbookIndex::Need{};
        if (fNeed_->isChecked()
            && nd.country != 'N' && nd.band != 'N' && nd.mode != 'N')
            continue;

        const int r = table_->rowCount();
        table_->insertRow(r);

        auto* utc = new QTableWidgetItem(
            QDateTime::fromSecsSinceEpoch(l.atSecs, QTimeZone::utc())
                .toString("HHmm"));
        utc->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, ColUtc, utc);

        auto* khz = new QTableWidgetItem(
            QString::number(l.hz / 1000.0, 'f', 1));
        khz->setData(Qt::UserRole, l.hz);
        khz->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        khz->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, ColKhz, khz);

        auto* call = new QTableWidgetItem(l.call);
        call->setForeground(QColor(QLatin1String(statusColor(l.status))));
        QFont cf = call->font();
        cf.setBold(true);
        call->setFont(cf);
        call->setData(Qt::UserRole, QString(QChar::fromLatin1(l.kind)));
        call->setData(Qt::UserRole + 1, l.tag);
        call->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, ColCall, call);

        needCell(table_, r, ColC, nd.country);
        needCell(table_, r, ColB, nd.band);
        needCell(table_, r, ColM, nd.mode);

        auto* az = new QTableWidgetItem;
        az->setTextAlignment(Qt::AlignCenter);
        az->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (haveMe && l.lat < 500.0) {
            const double a =
                bearing::initialDeg(myLat, myLon, l.lat, l.lon);
            az->setText(QString("%1°").arg(int(a + 0.5)));
            az->setData(Qt::UserRole, a);
            az->setForeground(QColor(127, 183, 217));
        }
        table_->setItem(r, ColAz, az);

        auto* sp = new QTableWidgetItem(l.spotter);
        sp->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, ColSpotter, sp);

        auto* cm = new QTableWidgetItem(
            l.comment.isEmpty() ? l.tag : l.comment);
        cm->setForeground(QColor(138, 153, 171));
        cm->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        table_->setItem(r, ColComment, cm);
        ++shown;
    }
    table_->setUpdatesEnabled(true);
    count_->setText(QString("%1 of %2 spots").arg(shown).arg(spots_.size()));
}

} // namespace ttc
