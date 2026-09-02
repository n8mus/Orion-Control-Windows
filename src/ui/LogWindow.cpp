// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/LogWindow.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>

#include "log/LogDb.h"
#include "log/QrzLookup.h"
#include "net/RotorLink.h"
#include "util/Bearing.h"
#include "util/BrowserGlobe.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

namespace {
// The band map's worked-before ramp (PanadapterWidget paints spots with
// these exact values) — the entry window must tell the same story.
const char* statusColor(QChar st) {
    switch (st.toLatin1()) {
        case 'N': return "#ff5c46";        // new one!
        case 'B': return "#ffbe3c";        // new band
        case 'W': return "#82de8c";        // worked
        case 'C': return "#96a2b2";        // confirmed
        default:  return "#dde7f0";        // no data yet
    }
}
QString needText(QChar st, const QString& what) {
    switch (st.toLatin1()) {
        case 'N': return what + " — needed";
        case 'W': return what + " worked, unconfirmed";
        case 'C': return what + " confirmed";
        default:  return what + " — no log data";
    }
}
} // namespace

LogWindow::LogWindow(LogDb* db, LogbookIndex* idx, const CtyLookup* cty,
                     RotorLink* rotor, QrzLookup* qrz, QWidget* parent)
    : QDialog(parent), db_(db), idx_(idx), cty_(cty), rotor_(rotor),
      qrz_(qrz) {
    setModal(false);
    setWindowTitle("LOG — New QSO");
    // On Windows the tool windows are parentless top-levels (N1MM-style),
    // so clicking the console would bury this one — but the whole point of
    // the entry window is living above the band map while spots feed it
    // (operator call, first live QSO). Linux keeps normal transient-for
    // stacking via the parent.
    if (!parent) setWindowFlag(Qt::WindowStaysOnTopHint);
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 15px; }"
        "QLabel { color: #b8c8d8; font-size: 13px; }"
        "QLineEdit { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 5px 8px; font-size: 16px;"
        " font-family: monospace; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 7px 12px; font-size: 14px;"
        " font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }"
        "QPushButton:disabled { color: #5a6a7a; }"
        "QTableWidget { background: #10161e; color: #b8c8d8; border: 1px"
        " solid #2a3644; font-size: 13px; }"
        "QHeaderView::section { background: #1a222c; color: #8fa2b5;"
        " border: none; padding: 3px 6px; font-size: 11px; }");

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(14, 12, 14, 12);
    v->setSpacing(9);

    // Call + country line.
    auto* callRow = new QHBoxLayout;
    call_ = new QLineEdit(this);
    call_->setPlaceholderText("CALL");
    call_->setMaxLength(16);
    QFont cf = call_->font();
    cf.setPointSize(20);
    cf.setBold(true);
    call_->setFont(cf);
    call_->setMinimumWidth(190);
    callRow->addWidget(call_, 0);
    auto* qrzBtn = new QPushButton("QRZ", this);
    qrzBtn->setToolTip("Look the call up on QRZ.com — fills name, QTH and "
                       "grid\n(QRZ website login goes in Online Logs)");
    qrzBtn->setFocusPolicy(Qt::NoFocus);
    qrzBtn->setMaximumWidth(56);
    callRow->addWidget(qrzBtn, 0);
    auto* globeBtn = new QPushButton("Globe", this);
    globeBtn->setToolTip("The 3D globe in your browser — both stations, "
                         "borders, country names,\nand the great-circle "
                         "path (the cqrlog globe, ported whole)");
    globeBtn->setFocusPolicy(Qt::NoFocus);
    globeBtn->setMaximumWidth(64);
    callRow->addWidget(globeBtn, 0);
    connect(globeBtn, &QPushButton::clicked, this,
            [this] { openBrowserGlobe(); });
    country_ = new QLabel(this);
    country_->setTextFormat(Qt::PlainText);
    country_->setWordWrap(true);
    callRow->addWidget(country_, 1);
    v->addLayout(callRow);
    // The button does what a ham means by "look him up on QRZ": the page
    // opens in the browser, and the XML callbook quietly fills name/QTH/
    // grid behind it when the website login is configured.
    connect(qrzBtn, &QPushButton::clicked, this, [this] {
        const QString c = call_->text().trimmed().toUpper();
        if (c.isEmpty()) return;
        QDesktopServices::openUrl(
            QUrl("https://www.qrz.com/db/" + c));
        if (qrz_) qrz_->lookup(c);
    });
    if (qrz_) {
        connect(qrz_, &QrzLookup::result, this,
                [this](const QString& call, bool ok, const QString& name,
                       const QString& qth, const QString& grid,
                       const QString& err) {
                    if (call != call_->text().trimmed().toUpper()) return;
                    if (!ok) {
                        country_->setText("QRZ: " + err);
                        return;
                    }
                    if (!name.isEmpty()) name_->setText(name);
                    if (!qth.isEmpty()) qth_->setText(qth);
                    // A precise grid beats the country-center bearing.
                    if (!grid.isEmpty()) grid_->setText(grid);
                    updateBadges();
                    updateRotor();
                });
    }

    // Needed badges (country-centric, HRD's three checkboxes).
    auto* badges = new QHBoxLayout;
    badges->setSpacing(8);
    for (QLabel** b : {&bCountry_, &bBand_, &bMode_}) {
        *b = new QLabel(this);
        (*b)->setStyleSheet("QLabel { border: 1px solid #3a4a5e;"
                            " border-radius: 3px; padding: 3px 9px;"
                            " font-size: 12px; font-weight: bold; }");
        badges->addWidget(*b);
    }
    badges->addStretch(1);
    v->addLayout(badges);

    // Fields grid.
    auto* g = new QGridLayout;
    g->setHorizontalSpacing(8);
    g->setVerticalSpacing(6);
    const auto addField = [this, g](int row, int col, const char* label,
                                    QLineEdit** e, int width) {
        auto* l = new QLabel(QLatin1String(label), this);
        l->setStyleSheet("QLabel { color: #77869a; font-size: 11px; }");
        g->addWidget(l, row * 2, col);
        *e = new QLineEdit(this);
        if (width > 0) (*e)->setMaximumWidth(width);
        g->addWidget(*e, row * 2 + 1, col);
    };
    addField(0, 0, "FREQ (Hz)", &freq_, 130);
    addField(0, 1, "MODE", &mode_, 80);
    addField(0, 2, "RST SENT", &rstS_, 70);
    addField(0, 3, "RST RCVD", &rstR_, 70);
    addField(1, 0, "DATE (UTC)", &date_, 130);
    addField(1, 1, "TIME", &time_, 80);
    addField(1, 2, "NAME", &name_, 0);
    addField(1, 3, "QTH", &qth_, 0);
    addField(2, 0, "GRID", &grid_, 130);
    addField(2, 1, "POTA", &pota_, 110);
    addField(2, 2, "COMMENT", &comment_, 0);
    g->addWidget(new QWidget(this), 5, 3);         // keep comment from greed
    v->addLayout(g);

    // Rotor bar.
    auto* rot = new QHBoxLayout;
    heading_ = new QLabel(this);
    rot->addWidget(heading_, 1);
    spBtn_ = new QPushButton(this);
    lpBtn_ = new QPushButton(this);
    spBtn_->setToolTip("Turn the rotor to the short path bearing");
    lpBtn_->setToolTip("Turn the rotor to the long path bearing");
    rot->addWidget(spBtn_);
    rot->addWidget(lpBtn_);
    v->addLayout(rot);

    // Previous QSOs with this call.
    prev_ = new QTableWidget(0, 5, this);
    prev_->setHorizontalHeaderLabels({"DATE", "FREQ", "MODE", "RST", "QSL"});
    prev_->horizontalHeader()->setStretchLastSection(true);
    prev_->verticalHeader()->setVisible(false);
    prev_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    prev_->setSelectionMode(QAbstractItemView::NoSelection);
    prev_->setFocusPolicy(Qt::NoFocus);
    prev_->setMaximumHeight(110);
    v->addWidget(prev_);

    // Footer.
    auto* foot = new QHBoxLayout;
    auto* logBtn = new QPushButton("LOG QSO", this);
    logBtn->setDefault(true);
    logBtn->setStyleSheet("QPushButton { background: #2b62c9; border-color:"
                          " #3a73dd; color: #ffffff; padding: 8px 22px; }");
    foot->addWidget(logBtn);
    auto* clearBtn = new QPushButton("Clear", this);
    foot->addWidget(clearBtn);
    foot->addStretch(1);
    auto* where = new QLabel(this);
    where->setStyleSheet("QLabel { color: #5f6e7e; font-size: 11px; }");
    where->setText("station log: " + (db_ ? db_->path() : QString()));
    foot->addWidget(where);
    v->addLayout(foot);

    connect(call_, &QLineEdit::textEdited, this, [this] { onCallEdited(); });
    connect(call_, &QLineEdit::editingFinished, this,
            [this] { autoLookup(); });
    connect(mode_, &QLineEdit::textEdited, this, [this] {
        // Mode typed by hand: refresh RST defaults + badges.
        const bool cw = mode_->text().trimmed().compare("CW",
                            Qt::CaseInsensitive) == 0;
        rstS_->setText(cw ? "599" : "59");
        rstR_->setText(cw ? "599" : "59");
        updateBadges();
    });
    const auto stopClock = [this] { autoTime_ = false; };
    connect(date_, &QLineEdit::textEdited, this, stopClock);
    connect(time_, &QLineEdit::textEdited, this, stopClock);
    connect(logBtn, &QPushButton::clicked, this, [this] { logNow(); });
    connect(clearBtn, &QPushButton::clicked, this, [this] { clearForNext(); });
    connect(spBtn_, &QPushButton::clicked, this, [this] {
        if (rotor_ && spAz_ >= 0) rotor_->turnTo(spAz_);
    });
    connect(lpBtn_, &QPushButton::clicked, this, [this] {
        if (rotor_ && lpAz_ >= 0) rotor_->turnTo(lpAz_);
    });
    if (rotor_)
        connect(rotor_, &RotorLink::azimuthChanged, this,
                [this](double) { updateRotor(); });
    if (idx_)
        connect(idx_, &LogbookIndex::updated, this,
                [this] { updateBadges(); });

    clock_ = new QTimer(this);
    clock_->setInterval(1000);
    connect(clock_, &QTimer::timeout, this, [this] { tickClock(); });
    clock_->start();

    rstS_->setText("599");
    rstR_->setText("599");
    tickClock();
    updateBadges();
    updateRotor();
}

void LogWindow::prefill(const QString& call, const QString& park,
                        const QString& grid) {
    const QString c = call.trimmed().toUpper();
    const bool fresh = c != call_->text().trimmed().toUpper();
    call_->setText(c);
    if (fresh) {                          // new station: old details go
        name_->clear();
        qth_->clear();
        grid_->clear();
        pota_->clear();
    }
    if (!park.isEmpty()) pota_->setText(park.trimmed().toUpper());
    if (!grid.isEmpty()) grid_->setText(grid.trimmed().toUpper());
    onCallEdited();
    if (fresh) autoLookup();              // cqrlog's reflex: name/QTH fill
    call_->setFocus();
    call_->deselect();
}

// A call in the box means a callbook question — cqrlog asks it without
// being told to, so this window does too. Quiet when no login is set (the
// QRZ button still explains what's missing when pressed by hand).
void LogWindow::autoLookup() {
    if (!qrz_) return;
    if (QSettings().value("up/qrzweb/user").toString().trimmed().isEmpty())
        return;
    const QString c = call_->text().trimmed();
    if (c.size() >= 3) qrz_->lookup(c);
}

void LogWindow::setRig(qint64 freqHz, const QString& mode) {
    if (freqHz > 0) freq_->setText(QString::number(freqHz));
    // Only a CHANGE of rig mode rewrites the field — so a WSJT-X mode
    // hint ("FT8") survives the radio sitting in USB the whole session.
    if (!mode.isEmpty() && mode != lastAutoMode_) {
        lastAutoMode_ = mode;
        mode_->setText(mode);
        const bool cw = mode.compare("CW", Qt::CaseInsensitive) == 0;
        rstS_->setText(cw ? "599" : "59");
        rstR_->setText(cw ? "599" : "59");
    }
    updateBadges();
}

void LogWindow::setModeHint(const QString& mode) {
    const QString m = mode.trimmed().toUpper();
    if (m.isEmpty()) return;
    mode_->setText(m);
    updateBadges();
}

void LogWindow::onCallEdited() {
    updateBadges();
    updateRotor();
    // Previous QSOs.
    prev_->setRowCount(0);
    if (!db_) return;
    const QList<Qso> qs = db_->prevQsos(call_->text());
    for (const Qso& q : qs) {
        const int r = prev_->rowCount();
        prev_->insertRow(r);
        const auto put = [this, r](int c, const QString& t) {
            prev_->setItem(r, c, new QTableWidgetItem(t));
        };
        put(0, q.tsUtc.toString("yyyy-MM-dd"));
        put(1, q.freqHz > 0 ? QString::number(q.freqHz / 1e6, 'f', 3)
                            : q.band);
        put(2, q.mode);
        put(3, q.rstS + " / " + q.rstR);
        put(4, q.lotwRcvd == "Y" ? "LoTW"
             : q.eqslRcvd == "Y" ? "eQSL"
             : q.qslRcvd == "Y" ? "card" : QString());
    }
}

void LogWindow::updateBadges() {
    const QString call = call_->text().trimmed().toUpper();
    const QString band = LogbookIndex::bandForHz(freq_->text().toLongLong());
    const QString mode = mode_->text().trimmed().toUpper();

    // Call color rides the band map's worked-before ramp.
    const QChar st = (idx_ && !call.isEmpty()) ? idx_->status(call, band)
                                               : QChar('?');
    call_->setStyleSheet(QString("QLineEdit { color: %1; }")
                             .arg(QLatin1String(statusColor(st))));

    // Country line.
    QString line;
    if (cty_ && !call.isEmpty()) {
        CtyInfo ci;
        if (cty_->info(call, ci)) {
            line = ci.country;
            if (ci.cq > 0) line += QString(" · CQ %1").arg(ci.cq);
            if (ci.itu > 0) line += QString(" · ITU %1").arg(ci.itu);
        }
    }
    country_->setText(line);

    // Needed badges (country-centric).
    const LogbookIndex::Need nd = (idx_ && !call.isEmpty())
        ? idx_->need(call, band, mode) : LogbookIndex::Need{};
    const auto paint = [](QLabel* l, QChar st2, const QString& what) {
        l->setVisible(!what.isEmpty());
        l->setText(needText(st2, what));
        l->setStyleSheet(QString(
            "QLabel { border: 1px solid %1; border-radius: 3px; padding:"
            " 3px 9px; font-size: 12px; font-weight: bold; color: %1; }")
            .arg(QLatin1String(statusColor(st2 == 'W' ? QChar('B') : st2))));
    };
    paint(bCountry_, nd.country,
          line.isEmpty() ? QStringLiteral("country")
                         : line.section(QChar(0xB7), 0, 0).trimmed());
    paint(bBand_, nd.band, band.isEmpty() ? QStringLiteral("band") : band);
    paint(bMode_, nd.mode, mode.isEmpty() ? QStringLiteral("mode") : mode);
}

void LogWindow::updateRotor() {
    spAz_ = lpAz_ = -1.0;
    dxLat_ = dxLon_ = 999.0;
    double myLat = 0, myLon = 0, dxLat = 0, dxLon = 0;
    const QString myGrid =
        QSettings().value("station/grid", "EN83al").toString();
    bool haveMe = CtyLookup::gridToLatLon(myGrid, myLat, myLon);
    bool haveDx = false;
    // A typed DX grid beats the cty.dat country center.
    if (CtyLookup::gridToLatLon(grid_->text(), dxLat, dxLon)) haveDx = true;
    else if (cty_ && !call_->text().trimmed().isEmpty())
        haveDx = cty_->lookup(call_->text(), dxLat, dxLon);
    QString head;
    if (rotor_ && rotor_->connected() && rotor_->azimuth() >= 0)
        head = QString("rotor %1°").arg(int(rotor_->azimuth() + 0.5));
    if (haveDx) {
        dxLat_ = dxLat;
        dxLon_ = dxLon;
        if (!call_->text().trimmed().isEmpty())
            emit dxLocated(dxLat, dxLon, call_->text().trimmed().toUpper());
    }
    if (haveMe && haveDx) {
        spAz_ = bearing::initialDeg(myLat, myLon, dxLat, dxLon);
        lpAz_ = spAz_ >= 180.0 ? spAz_ - 180.0 : spAz_ + 180.0;
        const double km = bearing::distanceKm(myLat, myLon, dxLat, dxLon);
        spBtn_->setText(QString("SP %1°").arg(int(spAz_ + 0.5)));
        lpBtn_->setText(QString("LP %1°").arg(int(lpAz_ + 0.5)));
        if (!head.isEmpty()) head += "   ";
        // station/units: auto = the system locale's system (US -> miles).
        const QString up =
            QSettings().value("station/units", "auto").toString();
        const bool miles = up == "mi"
            || (up == "auto"
                && QLocale().measurementSystem() != QLocale::MetricSystem);
        const double f = miles ? 0.621371 : 1.0;
        const QString unit = miles ? "mi" : "km";
        head += QString("%1 %3 sp · %2 %3 lp")
                    .arg(qRound(km * f))
                    .arg(qRound((40030.0 - km) * f))
                    .arg(unit);
    } else {
        spBtn_->setText("SP —");
        lpBtn_->setText("LP —");
    }
    heading_->setText(head);
    const bool can = rotor_ && rotor_->connected() && spAz_ >= 0;
    spBtn_->setEnabled(can);
    lpBtn_->setEnabled(can);
}

void LogWindow::openBrowserGlobe() {
    double myLat = 0, myLon = 0;
    const QString myGrid =
        QSettings().value("station/grid", "EN83al").toString();
    if (!CtyLookup::gridToLatLon(myGrid, myLat, myLon)) return;
    if (dxLat_ > 500.0) return;               // no located DX yet
    const double km =
        bearing::distanceKm(myLat, myLon, dxLat_, dxLon_);
    const QString up =
        QSettings().value("station/units", "auto").toString();
    const bool miles = up == "mi"
        || (up == "auto"
            && QLocale().measurementSystem() != QLocale::MetricSystem);
    const QString dist = miles
        ? QString("%L1 mi").arg(qRound(km * 0.621371))
        : QString("%L1 km").arg(qRound(km));
    BrowserGlobe::show(
        myLat, myLon,
        QSettings().value("station/callsign", "N8EM").toString(), myGrid,
        dxLat_, dxLon_, call_->text().trimmed().toUpper(),
        grid_->text().trimmed().toUpper(), dist,
        spAz_ >= 0 ? int(spAz_ + 0.5) : 0, country_->text());
}

void LogWindow::tickClock() {
    if (!autoTime_) return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    date_->setText(now.toString("yyyy-MM-dd"));
    time_->setText(now.toString("HH:mm"));
}

void LogWindow::logNow() {
    const QString call = call_->text().trimmed().toUpper();
    if (call.isEmpty() || !db_) return;
    Qso q;
    q.call = call;
    q.freqHz = freq_->text().toLongLong();
    q.band = LogbookIndex::bandForHz(q.freqHz);
    q.mode = mode_->text().trimmed().toUpper();
    q.tsUtc = QDateTime(
        QDate::fromString(date_->text().trimmed(), "yyyy-MM-dd"),
        QTime::fromString(time_->text().trimmed(),
                          time_->text().trimmed().size() > 5 ? "HH:mm:ss"
                                                             : "HH:mm"),
        QTimeZone::utc());
    if (!q.tsUtc.isValid()) q.tsUtc = QDateTime::currentDateTimeUtc();
    q.rstS = rstS_->text();
    q.rstR = rstR_->text();
    q.name = name_->text();
    q.qth = qth_->text();
    q.grid = grid_->text().trimmed().toUpper();
    q.pota = pota_->text().trimmed().toUpper();
    q.comment = comment_->text();
    if (cty_) {
        CtyInfo ci;
        if (cty_->info(call, ci)) {
            q.country = ci.country;
            q.cqz = ci.cq;
            q.ituz = ci.itu;
        }
    }
    const qint64 id = db_->addQso(q);
    if (id < 0) return;
    emit qsoLogged(id, call);
    clearForNext();
}

void LogWindow::clearForNext() {
    call_->clear();
    name_->clear();
    qth_->clear();
    grid_->clear();
    pota_->clear();
    comment_->clear();
    prev_->setRowCount(0);
    autoTime_ = true;
    tickClock();
    updateBadges();
    updateRotor();
    call_->setFocus();
    emit cleared();
}

} // namespace ttc
