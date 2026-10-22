// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/FreeDvWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "util/Bearing.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

namespace {
const char* kStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QCheckBox { color: #c8d4e0; }"
    "QGroupBox { color: #e8b04a; font-weight: bold; border: 1px solid #2a3644;"
    " margin-top: 12px; padding-top: 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QComboBox, QPushButton, QLineEdit { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #2a3644; padding: 3px 8px; }"
    "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
    " selection-background-color: #2b5c8a; }"
    "QTableWidget { background: #0b1016; color: #c8d4e0; gridline-color: #1c2430;"
    " selection-background-color: #2b5c8a; border: 1px solid #2a3644; }"
    "QHeaderView::section { background: #1c2430; color: #8fa3b8; border: 0;"
    " padding: 3px 6px; }"
    "QProgressBar { background: #0b1016; border: 1px solid #2a3644; height: 12px;"
    " text-align: center; color: transparent; }";
const char* kBarGreen = "QProgressBar::chunk { background: #50c878; }";
const char* kBarAmber = "QProgressBar::chunk { background: #e8b04a; }";
QString callColor(char st) {
    switch (st) {
    case 'N': return "#ff5c46";
    case 'B': return "#ffbe3c";
    case 'W': return "#82de8c";
    case 'C': return "#96a2b2";
    default:  return "#ffd832";
    }
}
enum Col { cCall, cGrid, cKm, cAz, cKhz, cMode, cStatus, cLastTx, cHeard, cVer, cMsg, cN };
QTableWidgetItem* item(const QString& t, Qt::Alignment a = Qt::AlignLeft | Qt::AlignVCenter) {
    auto* i = new QTableWidgetItem(t);
    i->setTextAlignment(a);
    i->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return i;
}
QString ago(qint64 t, qint64 now) {
    if (t <= 0) return QString();
    const qint64 m = (now - t) / 60;
    return m < 1 ? QStringLiteral("now") : m < 120 ? QString("%1 min").arg(m)
                                                   : QString("%1 h").arg(m / 60);
}
}  // namespace

FreeDvWindow::FreeDvWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle("FreeDV — RADE");
    setStyleSheet(kStyle);
    resize(1040, 680);
    auto* lay = new QVBoxLayout(this);

    // ---- status strip -------------------------------------------------
    auto* strip = new QHBoxLayout;
    sync_ = new QLabel("○ searching", this);
    sync_->setMinimumWidth(130);
    sync_->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #8a96a3; }");
    strip->addWidget(sync_);
    strip->addWidget(new QLabel("SNR", this));
    snr_ = new QProgressBar(this);
    snr_->setRange(-5, 25);
    snr_->setFixedWidth(160);
    snr_->setStyleSheet(kBarGreen);
    strip->addWidget(snr_);
    snrText_ = new QLabel("— dB", this);
    snrText_->setMinimumWidth(50);
    strip->addWidget(snrText_);
    foff_ = new QLabel(this);
    foff_->setMinimumWidth(110);
    strip->addWidget(foff_);
    trx_ = new QLabel("RX", this);
    trx_->setAlignment(Qt::AlignCenter);
    trx_->setFixedWidth(48);
    strip->addWidget(trx_);
    last_ = new QLabel("heard: —", this);
    last_->setStyleSheet("QLabel { color: #c8d4e0; font-size: 14px; }");
    strip->addWidget(last_, 1);
    strip->addWidget(new QLabel("to radio", this));
    modem_ = new QProgressBar(this);
    modem_->setRange(-40, 0);
    modem_->setFixedWidth(120);
    modem_->setToolTip("RADE modem level to the SignaLink (dBFS). Set the SignaLink TX\n"
                       "knob so the radio's ALC just moves — never more.");
    strip->addWidget(modem_);
    lay->addLayout(strip);

    // Mic into the RADE encoder (freedv-gui's "Level" gauge while talking):
    // the vocoder measures the voice digitally, so a clipping mic distorts
    // what the far end hears however clean the RF and ALC look. Speech
    // peaks belong in the upper green; red = clipping, turn the mic down.
    auto* micRow = new QHBoxLayout;
    micRow->addWidget(new QLabel("Mic", this));
    mic_ = new QProgressBar(this);
    mic_->setRange(-40, 0);
    mic_->setFixedWidth(300);
    mic_->setToolTip("Mic level into the RADE vocoder (dBFS peak, held 1 s).\n"
                     "Talk normally: peaks in the upper green (about -12 to -6 dB).\n"
                     "Amber = hot, red = clipping — the far end hears it distorted.\n"
                     "Set it with the mic's own gain (the Alesis), not the radio.");
    micRow->addWidget(mic_);
    micText_ = new QLabel("—", this);
    micText_->setMinimumWidth(170);
    micRow->addWidget(micText_);
    micRow->addStretch(1);
    lay->addLayout(micRow);

    // ---- controls -----------------------------------------------------
    auto* ctl = new QHBoxLayout;
    // What you hear: the SDR receive volume and MUTE, here so FreeDV needs
    // no second window (operator, 2026-10-16) — same setting as SDR ▾ →
    // Receive audio.
    ctl->addWidget(new QLabel("Volume", this));
    vol_ = new QSlider(Qt::Horizontal, this);
    vol_->setRange(-40, 20);
    vol_->setFixedWidth(140);
    vol_->setToolTip("Receive volume — the decoded voice (or the sideband with Analog).\n"
                     "Same setting as SDR ▾ → Receive audio.");
    ctl->addWidget(vol_);
    volText_ = new QLabel("0 dB", this);
    volText_->setFixedWidth(44);
    ctl->addWidget(volText_);
    mute_ = new QCheckBox("MUTE", this);
    mute_->setToolTip("Silence the receive audio (decoding, sync and the heard log carry on)");
    ctl->addWidget(mute_);
    ctl->addSpacing(12);
    analog_ = new QCheckBox("Analog", this);
    analog_->setToolTip("Hear the raw sideband instead of the decoded voice (freedv-gui's\n"
                        "\"Switch to Analog\") — for tuning, or someone calling on SSB.\n"
                        "RADE keeps decoding meanwhile (sync, SNR, callsigns).");
    ctl->addWidget(analog_);
    ctl->addSpacing(12);
    ctl->addWidget(new QLabel("TX level", this));
    txLevel_ = new QSlider(Qt::Horizontal, this);
    txLevel_->setRange(-20, 0);
    txLevel_->setFixedWidth(140);
    txLevel_->setToolTip("RADE modem level (freedv-gui's TX attenuation). Coarse drive is\n"
                         "the SignaLink TX knob; trim here so the ALC just moves.");
    ctl->addWidget(txLevel_);
    txLevelText_ = new QLabel("0 dB", this);
    txLevelText_->setFixedWidth(44);
    ctl->addWidget(txLevelText_);
    ctl->addSpacing(12);
    report_ = new QCheckBox("Report my station", this);
    report_->setToolTip("Appear on qso.freedv.org (call, grid, frequency, TX, who you decode)\n"
                        "and send the calls you decode to PSK Reporter — as freedv-gui does.");
    ctl->addWidget(report_);
    reportInfo_ = new QLabel(this);
    ctl->addWidget(reportInfo_);
    ctl->addStretch(1);
    lay->addLayout(ctl);
    auto* ctl2 = new QHBoxLayout;
    ctl2->addWidget(new QLabel("My message", this));
    msg_ = new QLineEdit(this);
    msg_->setPlaceholderText("shown to everyone in the station list, e.g. \"QRV 20 m, calling CQ\"");
    msg_->setMaxLength(80);
    ctl2->addWidget(msg_, 1);
    auto* send = new QPushButton("Send", this);
    ctl2->addWidget(send);
    auto* web = new QPushButton("qso.freedv.org", this);
    web->setToolTip("The FreeDV Reporter website in the browser");
    ctl2->addWidget(web);
    lay->addLayout(ctl2);

    // ---- station list + heard log --------------------------------------
    auto* split = new QSplitter(Qt::Vertical, this);
    auto* top = new QWidget(split);
    auto* tl = new QVBoxLayout(top);
    tl->setContentsMargins(0, 0, 0, 0);
    auto* tbar = new QHBoxLayout;
    auto* tlab = new QLabel("<b style='color:#e8b04a'>STATIONS ON FREEDV</b> — double-click to "
                            "QSY, right-click for more", top);
    tlab->setTextFormat(Qt::RichText);
    tbar->addWidget(tlab);
    tbar->addStretch(1);
    bandFilter_ = new QComboBox(top);
    bandFilter_->addItems({"This band", "All bands"});
    tbar->addWidget(bandFilter_);
    count_ = new QLabel(top);
    tbar->addWidget(count_);
    tl->addLayout(tbar);
    table_ = new QTableWidget(0, cN, top);
    table_->setHorizontalHeaderLabels({"Callsign", "Locator", "km", "Az", "kHz", "Mode", "Status",
                                       "Last TX", "Last heard", "Version", "Message"});
    table_->verticalHeader()->hide();
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(cCall, 90);
    table_->setColumnWidth(cGrid, 64);
    table_->setColumnWidth(cKm, 56);
    table_->setColumnWidth(cAz, 44);
    table_->setColumnWidth(cKhz, 72);
    table_->setColumnWidth(cMode, 64);
    table_->setColumnWidth(cStatus, 92);
    table_->setColumnWidth(cLastTx, 64);
    table_->setColumnWidth(cHeard, 120);
    table_->setColumnWidth(cVer, 130);
    tl->addWidget(table_);
    auto* bot = new QWidget(split);
    auto* bl = new QVBoxLayout(bot);
    bl->setContentsMargins(0, 0, 0, 0);
    auto* hbar = new QHBoxLayout;
    auto* hlab = new QLabel("<b style='color:#e8b04a'>HEARD</b> — callsigns from the end of "
                            "each over, this session", bot);
    hlab->setTextFormat(Qt::RichText);
    hbar->addWidget(hlab);
    hbar->addStretch(1);
    auto* logBtn = new QPushButton("Log QSO", bot);
    logBtn->setToolTip("Open the log window with the selected (or newest) call, its grid,\n"
                       "frequency and SNR, mode DIGITALVOICE");
    hbar->addWidget(logBtn);
    auto* clr = new QPushButton("Clear", bot);
    hbar->addWidget(clr);
    bl->addLayout(hbar);
    heard_ = new QTableWidget(0, 4, bot);
    heard_->setHorizontalHeaderLabels({"UTC", "Callsign", "SNR", "kHz"});
    heard_->verticalHeader()->hide();
    heard_->setSelectionBehavior(QAbstractItemView::SelectRows);
    heard_->setSelectionMode(QAbstractItemView::SingleSelection);
    heard_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    heard_->setColumnWidth(0, 90);
    heard_->setColumnWidth(1, 110);
    heard_->setColumnWidth(2, 70);
    heard_->setColumnWidth(3, 90);
    bl->addWidget(heard_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);

    // ---- wiring ---------------------------------------------------------
    connect(analog_, &QCheckBox::toggled, this, &FreeDvWindow::analogChanged);
    connect(vol_, &QSlider::valueChanged, this, [this](int v) {
        volText_->setText(QString("%1 dB").arg(v));
        emit volumeChanged(v, mute_->isChecked());
    });
    connect(mute_, &QCheckBox::toggled, this, [this](bool m) { emit volumeChanged(vol_->value(), m); });
    connect(txLevel_, &QSlider::valueChanged, this, [this](int v) {
        txLevelText_->setText(QString("%1 dB").arg(v));
        emit txLevelChanged(v);
    });
    connect(report_, &QCheckBox::toggled, this, &FreeDvWindow::reportChanged);
    const auto sendMsg = [this] { emit statusMessageSent(msg_->text().trimmed()); };
    connect(send, &QPushButton::clicked, this, sendMsg);
    connect(msg_, &QLineEdit::returnPressed, this, sendMsg);
    connect(web, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl("https://qso.freedv.org")); });
    connect(bandFilter_, &QComboBox::currentIndexChanged, this, [this] { dirty_ = true; rebuild(); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
        const auto* it = table_->item(r, cKhz);
        if (it) emit qsyTo(it->data(Qt::UserRole).toLongLong(), table_->item(r, cCall)->text());
    });
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        const int r = table_->rowAt(p.y());
        if (r < 0) return;
        const QString call = table_->item(r, cCall)->text();
        const QString sid = table_->item(r, cCall)->data(Qt::UserRole).toString();
        const qint64 hz = table_->item(r, cKhz)->data(Qt::UserRole).toLongLong();
        const QVariant az = table_->item(r, cAz)->data(Qt::UserRole);
        QMenu m(this);
        m.addAction(QString("QSY to %1 (%2 kHz)").arg(call).arg(hz / 1000.0, 0, 'f', 1),
                    this, [this, hz, call] { emit qsyTo(hz, call); });
        QAction* rq = m.addAction(QString("Ask %1 to come to my frequency").arg(call), this,
                                  [this, sid, call] { emit requestQsy(sid, call); });
        rq->setEnabled(report_->isChecked());
        if (!report_->isChecked()) rq->setText(rq->text() + "  (needs Report my station)");
        if (az.isValid())
            m.addAction(QString("Turn the rotor to %1°").arg(int(az.toDouble() + 0.5)), this,
                        [this, a = az.toDouble()] { emit turnRotor(a); });
        m.addAction(QString("Look up %1 on QRZ").arg(call), this, [this, call] { emit lookupCall(call); });
        m.exec(table_->viewport()->mapToGlobal(p));
    });
    connect(logBtn, &QPushButton::clicked, this, [this] {
        int r = heard_->currentRow();
        if (r < 0) r = 0;                          // newest is on top
        if (r >= heard_->rowCount()) return;
        const QString call = heard_->item(r, 1)->text();
        emit logQso(call, gridFor(call), heard_->item(r, 3)->data(Qt::UserRole).toLongLong(),
                    heard_->item(r, 2)->data(Qt::UserRole).toInt());
    });
    connect(clr, &QPushButton::clicked, this, [this] { heard_->setRowCount(0); });
    rebuildTimer_ = new QTimer(this);
    rebuildTimer_->setInterval(1000);              // the list ticks at most once a second
    connect(rebuildTimer_, &QTimer::timeout, this, [this] { if (dirty_) rebuild(); });
    rebuildTimer_->start();
}

void FreeDvWindow::setControls(bool analog, int txLevelDb, bool report, const QString& message) {
    const QSignalBlocker a(analog_), t(txLevel_), r(report_);
    analog_->setChecked(analog);
    txLevel_->setValue(txLevelDb);
    txLevelText_->setText(QString("%1 dB").arg(txLevelDb));
    report_->setChecked(report);
    msg_->setText(message);
}

void FreeDvWindow::setVolume(int db, bool mute) {
    const QSignalBlocker v(vol_), m(mute_);
    vol_->setValue(db);
    volText_->setText(QString("%1 dB").arg(vol_->value()));
    mute_->setChecked(mute);
}

void FreeDvWindow::setReportingLive(bool live, const QString& who) {
    reportInfo_->setText(live ? QString("<span style='color:#50e080'>● reporting as %1</span>").arg(who)
                              : report_->isChecked() ? QStringLiteral("(connecting…)") : QString());
    reportInfo_->setTextFormat(Qt::RichText);
}

void FreeDvWindow::setStatus(const Status& s) {
    if (s.tx) {
        sync_->setText("● TRANSMIT");
        sync_->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #ff5c46; }");
    } else if (s.sync) {
        sync_->setText("● SYNC");
        sync_->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #50e080; }");
    } else {
        sync_->setText("○ searching");
        sync_->setStyleSheet("QLabel { font-size: 16px; font-weight: bold; color: #8a96a3; }");
    }
    snr_->setValue(s.sync ? int(std::lround(std::clamp(s.snrDb, -5.f, 25.f))) : -5);
    snr_->setStyleSheet(s.snrDb >= 6 ? kBarGreen : kBarAmber);
    snrText_->setText(s.sync ? QString("%1 dB").arg(s.snrDb, 0, 'f', 0) : QStringLiteral("— dB"));
    foff_->setText(s.sync ? QString("offset %1 Hz").arg(s.freqOffsetHz, 0, 'f', 1) : QString());
    trx_->setText(s.tx ? "TX" : "RX");
    trx_->setStyleSheet(s.tx ? "QLabel { background: #8a2727; color: #ffe8e8; font-weight: bold; }"
                             : "QLabel { background: #1c3a2a; color: #a8f0c0; font-weight: bold; }");
    if (!s.lastCall.isEmpty())
        last_->setText(QString("heard: <b style='color:%1'>%2</b>")
                           .arg(callColor(statusFor_ ? statusFor_(s.lastCall, dialHz_) : '?'),
                                s.lastCall));
    // Mic: fast up, ~20 dB/s down, the peak held for 1 s (status ticks
    // every 250 ms); a clip lights the readout red for 2 s.
    micShown_ = std::max(s.micDb, micShown_ - 5.f);
    if (s.micDb >= micHold_ || --micHoldTicks_ <= 0) { micHold_ = s.micDb; micHoldTicks_ = 4; }
    if (s.micDb >= -1.f) clipTicks_ = 8;
    mic_->setValue(int(std::clamp(micShown_, -40.f, 0.f)));
    const char* micBar = micHold_ >= -1.f ? "QProgressBar::chunk { background: #e05050; }"
                       : micHold_ >= -6.f ? kBarAmber : kBarGreen;
    mic_->setStyleSheet(micBar);
    if (s.micDb <= -119.f) {
        micText_->setText("— (not transmitting RADE)");
        micText_->setStyleSheet("QLabel { color: #8fa3b8; }");
    } else if (clipTicks_ > 0) {
        --clipTicks_;
        micText_->setText(QString("CLIPPING — mic gain down (%1 dB)").arg(micHold_, 0, 'f', 0));
        micText_->setStyleSheet("QLabel { color: #ff5c46; font-weight: bold; }");
    } else {
        micText_->setText(micHold_ >= -6.f ? QString("hot, %1 dB").arg(micHold_, 0, 'f', 0)
                          : micHold_ >= -20.f ? QString("good, %1 dB").arg(micHold_, 0, 'f', 0)
                                              : QString("low, %1 dB").arg(micHold_, 0, 'f', 0));
        micText_->setStyleSheet(micHold_ >= -6.f ? "QLabel { color: #e8b04a; }"
                                                 : "QLabel { color: #a8f0c0; }");
    }
    modem_->setValue(int(std::clamp(s.modemOutDb, -40.f, 0.f)));
    modem_->setStyleSheet(s.modemOutDb > -3 ? kBarAmber : kBarGreen);
}

void FreeDvWindow::setStations(const QVector<FreeDvReporter::StationInfo>& st, qint64 dialHz) {
    st_ = st;
    dialHz_ = dialHz;
    dirty_ = true;
}

QString FreeDvWindow::gridFor(const QString& call) const {
    for (const auto& s : st_)
        if (s.call == call) return s.grid;
    return QString();
}

void FreeDvWindow::addHeard(const QString& call, int snrDb, qint64 hz) {
    heard_->insertRow(0);
    heard_->setItem(0, 0, item(QDateTime::currentDateTimeUtc().toString("HH:mm:ss")));
    auto* c = item(call);
    c->setForeground(QColor(callColor(statusFor_ ? statusFor_(call, hz) : '?')));
    QFont f = c->font();
    f.setBold(true);
    c->setFont(f);
    heard_->setItem(0, 1, c);
    auto* sn = item(QString("%1 dB").arg(snrDb), Qt::AlignRight | Qt::AlignVCenter);
    sn->setData(Qt::UserRole, snrDb);
    heard_->setItem(0, 2, sn);
    auto* k = item(QString::number(hz / 1000.0, 'f', 1), Qt::AlignRight | Qt::AlignVCenter);
    k->setData(Qt::UserRole, hz);
    heard_->setItem(0, 3, k);
}

void FreeDvWindow::rebuild() {
    dirty_ = false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QString band = LogbookIndex::bandForHz(dialHz_);
    double myLat = 0, myLon = 0;
    const bool haveMe = CtyLookup::gridToLatLon(myGrid_, myLat, myLon);
    QVector<FreeDvReporter::StationInfo> rows;
    for (const auto& s : st_) {
        if (s.hz < 1800000 || s.hz > 54000000) continue;
        if (bandFilter_->currentIndex() == 0 && !band.isEmpty()
            && LogbookIndex::bandForHz(s.hz) != band)
            continue;
        rows.push_back(s);
    }
    // On the air first, then the most recent transmitters, then listeners.
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.transmitting != b.transmitting) return a.transmitting;
        if (a.lastTx != b.lastTx) return a.lastTx > b.lastTx;
        return a.call < b.call;
    });
    const QString selCall = table_->currentRow() >= 0 && table_->item(table_->currentRow(), cCall)
        ? table_->item(table_->currentRow(), cCall)->text() : QString();
    table_->setUpdatesEnabled(false);
    table_->setRowCount(int(rows.size()));
    for (int r = 0; r < int(rows.size()); ++r) {
        const auto& s = rows[size_t(r)];
        auto* c = item(s.call);
        c->setData(Qt::UserRole, s.sid);
        c->setForeground(QColor(callColor(statusFor_ ? statusFor_(s.call, s.hz) : '?')));
        QFont f = c->font();
        f.setBold(true);
        c->setFont(f);
        table_->setItem(r, cCall, c);
        table_->setItem(r, cGrid, item(s.grid));
        double lat = 0, lon = 0;
        auto* km = item(QString(), Qt::AlignRight | Qt::AlignVCenter);
        auto* az = item(QString(), Qt::AlignRight | Qt::AlignVCenter);
        if (haveMe && !s.grid.isEmpty() && CtyLookup::gridToLatLon(s.grid, lat, lon)) {
            km->setText(QString::number(int(bearing::distanceKm(myLat, myLon, lat, lon))));
            const double a = bearing::initialDeg(myLat, myLon, lat, lon);
            az->setText(QString("%1°").arg(int(a + 0.5)));
            az->setData(Qt::UserRole, a);
        }
        table_->setItem(r, cKm, km);
        table_->setItem(r, cAz, az);
        auto* k = item(QString::number(s.hz / 1000.0, 'f', 1), Qt::AlignRight | Qt::AlignVCenter);
        k->setData(Qt::UserRole, s.hz);
        if (std::llabs(s.hz - dialHz_) < 500) k->setForeground(QColor("#e8b04a"));   // you're there
        table_->setItem(r, cKhz, k);
        table_->setItem(r, cMode, item(s.mode));
        auto* stt = item(s.transmitting ? QStringLiteral("ON AIR")
                         : s.rxOnly ? QStringLiteral("listening (RX)")
                         : s.lastTx > 0 && now - s.lastTx < 1200 ? QStringLiteral("TX lately")
                                                                 : QStringLiteral("listening"));
        if (s.transmitting) {
            stt->setForeground(QColor("#ff5c46"));
            QFont sf = stt->font();
            sf.setBold(true);
            stt->setFont(sf);
        }
        table_->setItem(r, cStatus, stt);
        table_->setItem(r, cLastTx, item(ago(s.lastTx, now)));
        table_->setItem(r, cHeard, item(s.lastRxCall.isEmpty() ? QString()
                                        : QString("%1 %2 dB").arg(s.lastRxCall).arg(s.lastRxSnr)));
        table_->setItem(r, cVer, item(s.version));
        table_->setItem(r, cMsg, item(s.message));
        if (s.call == selCall) table_->selectRow(r);
    }
    table_->setUpdatesEnabled(true);
    count_->setText(QString("  %1 station%2").arg(rows.size()).arg(rows.size() == 1 ? "" : "s"));
}

} // namespace ttc
