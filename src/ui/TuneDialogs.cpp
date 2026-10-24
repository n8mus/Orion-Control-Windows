// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/TuneDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "app/Bands.h"

namespace ttc {

namespace {
// Same dark scheme as the SWR history browser, plus the editors the
// entry form needs.
const char* kTuneStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QGroupBox { color: #e8b04a; font-weight: bold; border: 1px solid"
    " #2a3644; margin-top: 10px; padding-top: 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; }"
    "QCheckBox { color: #c8d4e0; }"
    "QTableWidget { background: #0b1016; color: #c8d4e0; border: 1px"
    " solid #2a3644; font-family: monospace; font-size: 12px;"
    " gridline-color: #1c2430; }"
    "QHeaderView::section { background: #1c2430; color: #8fa3b8;"
    " border: none; padding: 3px 8px; font-weight: bold; }"
    "QComboBox, QPushButton, QLineEdit {"
    " background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
    " padding: 3px 6px; }"
    // Colors only: a border/padding rule would make Qt drop the native
    // up/down arrows, and the knob values are stepped with them.
    "QSpinBox, QDoubleSpinBox { background: #1c2430; color: #c8d4e0;"
    " selection-background-color: #3a4a5c; }"
    "QPushButton:default { border-color: #e8b04a; }"
    "QPushButton:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,"
    " QComboBox:disabled { color: #4a5868; }";

QString antLabel(const QString& a) {
    return a.isEmpty() ? QStringLiteral("(unlabeled)") : a;
}

QString bandOfHz(qint64 hz) {
    const int b = bandIndexOf(static_cast<uint64_t>(hz));
    return b >= 0 ? QLatin1String(kBands[b].label) : QString();
}

QDoubleSpinBox* tenthsBox(QWidget* parent) {
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(1.0, 10.0);
    s->setDecimals(1);
    s->setSingleStep(0.1);
    s->setFixedWidth(80);
    return s;
}
}  // namespace

QString tuneZText(const TunePoint& p) {
    return QString("%1 %2").arg(p.highZ ? "HiZ" : "LoZ").arg(p.zPos);
}

QString tuneTunerText(const TunePoint& p) {
    if (p.bypass) return QStringLiteral("BYPASS");
    return QString("Cap %1   %2   Ind %3")
        .arg(p.cap, 0, 'f', 1).arg(tuneZText(p)).arg(p.ind, 0, 'f', 1);
}

QString tuneAmpText(const TunePoint& p) {
    return QString("Tune %1   Load %2")
        .arg(p.ampTune, 0, 'f', 1).arg(p.ampLoad, 0, 'f', 1);
}

// ---------------------------------------------------------------- entry

TuneEntryDialog::TuneEntryDialog(const QString& antenna, const TunePoint& start,
                                 qint64 replacesHz, const QString& meterName,
                                 QWidget* parent)
    : QDialog(parent), ant_(antenna), replacesHz_(replacesHz),
      meterName_(meterName) {
    setWindowTitle(QString("Tuner / amp — %1").arg(antLabel(antenna)));
    setStyleSheet(kTuneStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* head = new QLabel(this);
    head->setText(QString("<span style='color:#e8b04a; font-size:14px;"
                          " font-weight:bold'>%1</span>")
                      .arg(antLabel(antenna).toHtmlEscaped()));
    lay->addWidget(head);

    auto* fRow = new QHBoxLayout;
    fRow->addWidget(new QLabel("Frequency", this));
    freq_ = new QDoubleSpinBox(this);
    freq_->setRange(1.8, 54.0);
    freq_->setDecimals(3);
    freq_->setSingleStep(0.001);
    freq_->setSuffix(" MHz");
    freq_->setFixedWidth(130);
    freq_->setValue(TuneTable::roundKhz(start.hz) / 1e6);
    fRow->addWidget(freq_);
    band_ = new QLabel(this);
    fRow->addWidget(band_);
    fRow->addStretch(1);
    lay->addLayout(fRow);

    auto* tuner = new QGroupBox("TUNER  (238C)", this);
    auto* tg = new QGridLayout(tuner);
    bypass_ = new QCheckBox("Bypass — antenna needs no tuner here", tuner);
    tg->addWidget(bypass_, 0, 0, 1, 2);
    tg->addWidget(new QLabel("Capacitor", tuner), 1, 0);
    cap_ = tenthsBox(tuner);
    tg->addWidget(cap_, 1, 1);
    tg->addWidget(new QLabel("Impedance", tuner), 2, 0);
    z_ = new QComboBox(tuner);
    for (int i = 1; i <= 5; ++i) z_->addItem(QString("Low Z %1").arg(i));
    for (int i = 1; i <= 5; ++i) z_->addItem(QString("High Z %1").arg(i));
    z_->setFixedWidth(110);
    tg->addWidget(z_, 2, 1);
    tg->addWidget(new QLabel("Inductor", tuner), 3, 0);
    ind_ = tenthsBox(tuner);
    ind_->setRange(1.0, 30.0);
    tg->addWidget(ind_, 3, 1);
    tg->setColumnStretch(2, 1);
    lay->addWidget(tuner);

    auto* amp = new QGroupBox("AMP  (Titan III)", this);
    auto* ag = new QGridLayout(amp);
    ag->addWidget(new QLabel("Tune", amp), 0, 0);
    // The Titan III's dials read 0-10 (operator, 2026-10-24: "I need to
    // enter a zero for the amp tune and it won't go below 1.0").
    tune_ = tenthsBox(amp);
    tune_->setRange(0.0, 10.0);
    ag->addWidget(tune_, 0, 1);
    ag->addWidget(new QLabel("Load", amp), 1, 0);
    load_ = tenthsBox(amp);
    load_->setRange(0.0, 10.0);
    ag->addWidget(load_, 1, 1);
    ag->setColumnStretch(2, 1);
    lay->addWidget(amp);

    auto* sRow = new QHBoxLayout;
    sRow->addWidget(new QLabel("SWR", this));
    swr_ = new QDoubleSpinBox(this);
    swr_->setRange(0.0, 9.99);
    swr_->setDecimals(2);
    swr_->setSingleStep(0.01);
    swr_->setSpecialValueText("—");                // 0 = none recorded
    swr_->setFixedWidth(80);
    swr_->setToolTip("Follows the wattmeter while it sees your carrier and\n"
                     "holds at key-up. Save while still transmitting to\n"
                     "record the SWR this setting gives. You can type it\n"
                     "too; — = none.");
    sRow->addWidget(swr_);
    live_ = new QLabel(this);
    sRow->addWidget(live_, 1);
    lay->addLayout(sRow);

    auto* nRow = new QHBoxLayout;
    nRow->addWidget(new QLabel("Note", this));
    note_ = new QLineEdit(this);
    note_->setMaxLength(80);
    nRow->addWidget(note_, 1);
    lay->addLayout(nRow);

    auto* bRow = new QHBoxLayout;
    bRow->addStretch(1);
    auto* cancel = new QPushButton("Cancel", this);
    save_ = new QPushButton("Save", this);
    save_->setDefault(true);
    bRow->addWidget(cancel);
    bRow->addWidget(save_);
    lay->addLayout(bRow);

    bypass_->setChecked(start.bypass);
    cap_->setValue(start.cap);
    z_->setCurrentIndex((start.highZ ? 5 : 0) + std::clamp(start.zPos, 1, 5) - 1);
    ind_->setValue(start.ind);
    tune_->setValue(start.ampTune);
    load_->setValue(start.ampLoad);
    swr_->setValue(start.swr >= 1.0 ? start.swr : 0.0);
    note_->setText(start.note);

    liveTimer_ = new QTimer(this);
    liveTimer_->setInterval(250);
    connect(liveTimer_, &QTimer::timeout, this, [this] { updateLive(); });
    liveTimer_->start();
    updateLive();

    connect(freq_, &QDoubleSpinBox::valueChanged, this, [this] { updateBand(); });
    connect(bypass_, &QCheckBox::toggled, this, [this] { updateBypass(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save_, &QPushButton::clicked, this, [this] {
        emit saveRequested(ant_, collect(), replacesHz_);
        accept();
    });
    updateBand();
    updateBypass();
}

void TuneEntryDialog::setLiveSwr(double swr) {
    if (swr < 1.0) return;
    swr_->setValue(std::min(swr, 9.99));
    const bool wasHeld = !sinceLive_.isValid() || sinceLive_.elapsed() >= 700;
    sinceLive_.restart();
    if (wasHeld) updateLive();                     // flip to LIVE right away
}

// The LIVE tag drops back to "held" a beat after the last carrier
// reading — that IS key-up, as far as the form can tell.
void TuneEntryDialog::updateLive() {
    if (sinceLive_.isValid() && sinceLive_.elapsed() < 700) {
        live_->setText(QString("<span style='color:#50e678;"
                               " font-weight:bold'>LIVE</span>"
                               " from the %1").arg(meterName_));
    } else if (sinceLive_.isValid()) {
        live_->setText("held at key-up");
    } else {
        live_->setText(meterName_.isEmpty()
            ? QStringLiteral("no wattmeter — type it in")
            : QString("key a carrier: reads the %1").arg(meterName_));
    }
}

void TuneEntryDialog::updateBand() {
    const QString b = bandOfHz(static_cast<qint64>(std::llround(freq_->value() * 1e6)));
    band_->setText(b.isEmpty()
        ? QStringLiteral("<span style='color:#eb503c'>outside the ham bands</span>")
        : QString("%1 m").arg(b));
    save_->setEnabled(!b.isEmpty());
}

void TuneEntryDialog::updateBypass() {
    const bool on = !bypass_->isChecked();
    cap_->setEnabled(on);
    z_->setEnabled(on);
    ind_->setEnabled(on);
}

TunePoint TuneEntryDialog::collect() const {
    TunePoint p;
    p.hz      = static_cast<qint64>(std::llround(freq_->value() * 1e6));
    p.bypass  = bypass_->isChecked();
    p.cap     = cap_->value();
    p.highZ   = z_->currentIndex() >= 5;
    p.zPos    = z_->currentIndex() % 5 + 1;
    p.ind     = ind_->value();
    p.ampTune = tune_->value();
    p.ampLoad = load_->value();
    p.swr     = swr_->value();
    p.note    = note_->text();
    p.savedMs = QDateTime::currentMSecsSinceEpoch();
    return p;
}

// ---------------------------------------------------------------- table

namespace {
constexpr int kColBand = 0, kColFreq = 1, kColTuner = 2, kColZ = 3,
              kColInd = 4, kColTune = 5, kColLoad = 6, kColSwr = 7,
              kColNote = 8, kColSaved = 9, kColCount = 10;
}

TuneTableDialog::TuneTableDialog(const TuneTable* table, QWidget* parent)
    : QDialog(parent), table_(table) {
    setWindowTitle("Tuner / amp settings");
    resize(860, 460);
    setStyleSheet(kTuneStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel("Antenna:", this));
    antSel_ = new QComboBox(this);
    antSel_->setMinimumWidth(160);
    top->addWidget(antSel_);
    top->addWidget(new QLabel("Band:", this));
    bandSel_ = new QComboBox(this);
    bandSel_->addItem("(all bands)", QString());
    for (int i = 0; i < kBandCount; ++i)
        bandSel_->addItem(QString("%1 m").arg(kBands[i].label),
                          QLatin1String(kBands[i].label));
    top->addWidget(bandSel_);
    top->addStretch(1);
    lay->addLayout(top);

    grid_ = new QTableWidget(0, kColCount, this);
    grid_->setHorizontalHeaderLabels({"Band", "MHz", "Tuner", "Z", "Ind",
                                      "Tune", "Load", "SWR", "Note", "Saved"});
    grid_->verticalHeader()->hide();
    grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid_->horizontalHeader()->setSectionResizeMode(kColNote,
                                                    QHeaderView::Stretch);
    lay->addWidget(grid_, 1);

    auto* bRow = new QHBoxLayout;
    auto* addBtn = new QPushButton("Add…", this);
    editBtn_ = new QPushButton("Edit…", this);
    delBtn_  = new QPushButton("Delete", this);
    qsyBtn_  = new QPushButton("QSY there", this);
    qsyBtn_->setToolTip("Tune VFO A to this entry's frequency");
    auto* closeBtn = new QPushButton("Close", this);
    bRow->addWidget(addBtn);
    bRow->addWidget(editBtn_);
    bRow->addWidget(delBtn_);
    bRow->addWidget(qsyBtn_);
    bRow->addStretch(1);
    bRow->addWidget(closeBtn);
    lay->addLayout(bRow);

    connect(antSel_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(bandSel_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(grid_, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool any = selectedRow() >= 0;
        editBtn_->setEnabled(any);
        delBtn_->setEnabled(any);
        qsyBtn_->setEnabled(any);
    });
    connect(grid_, &QTableWidget::cellDoubleClicked, this, [this] {
        if (const int r = selectedRow(); r >= 0)
            emit editRequested(selectedAnt(), rows_[r].second);
    });
    connect(addBtn, &QPushButton::clicked, this,
            [this] { emit addRequested(selectedAnt()); });
    connect(editBtn_, &QPushButton::clicked, this, [this] {
        if (const int r = selectedRow(); r >= 0)
            emit editRequested(selectedAnt(), rows_[r].second);
    });
    connect(delBtn_, &QPushButton::clicked, this, [this] {
        const int r = selectedRow();
        if (r < 0) return;
        const auto& row = rows_[r];
        if (QMessageBox::question(
                this, "Delete entry",
                QString("Delete the %1 entry at %2 MHz for %3?")
                    .arg(row.first + " m")
                    .arg(row.second.hz / 1e6, 0, 'f', 3)
                    .arg(antLabel(selectedAnt())))
            != QMessageBox::Yes)
            return;
        emit deleteRequested(selectedAnt(), row.first, row.second.hz);
    });
    connect(qsyBtn_, &QPushButton::clicked, this, [this] {
        if (const int r = selectedRow(); r >= 0)
            emit qsyRequested(rows_[r].second.hz);
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

void TuneTableDialog::setChoices(const QStringList& antennas,
                                 const QString& curAnt,
                                 const QString& curBand) {
    {
        const QSignalBlocker b1(antSel_), b2(bandSel_);
        antSel_->clear();
        QStringList all = antennas;
        if (!all.contains(QString())) all.prepend(QString());
        if (!all.contains(curAnt)) all << curAnt;
        for (const QString& a : all) antSel_->addItem(antLabel(a), a);
        antSel_->setCurrentIndex(std::max(0, antSel_->findData(curAnt)));
        bandSel_->setCurrentIndex(std::max(0, bandSel_->findData(curBand)));
    }
    refresh();
}

QString TuneTableDialog::selectedAnt() const {
    return antSel_->currentData().toString();
}

int TuneTableDialog::selectedRow() const {
    const auto sel = grid_->selectionModel()->selectedRows();
    if (sel.isEmpty()) return -1;
    const int r = sel.first().row();
    return r >= 0 && r < rows_.size() ? r : -1;
}

void TuneTableDialog::refresh() {
    const QString ant = selectedAnt();
    const QString onlyBand = bandSel_->currentData().toString();
    rows_.clear();
    for (int i = 0; i < kBandCount; ++i) {         // band order, low to high
        const QString b = QLatin1String(kBands[i].label);
        if (!onlyBand.isEmpty() && b != onlyBand) continue;
        for (const TunePoint& p : table_->points(ant, b)) rows_.append({b, p});
    }
    grid_->setRowCount(static_cast<int>(rows_.size()));
    for (int r = 0; r < rows_.size(); ++r) {
        const TunePoint& p = rows_[r].second;
        auto cell = [&](int c, const QString& t, bool right = false) {
            auto* it = new QTableWidgetItem(t);
            if (right) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid_->setItem(r, c, it);
        };
        cell(kColBand, rows_[r].first);
        cell(kColFreq, QString::number(p.hz / 1e6, 'f', 3), true);
        cell(kColTuner, p.bypass ? QStringLiteral("BYPASS")
                                 : QString("Cap %1").arg(p.cap, 0, 'f', 1));
        cell(kColZ, p.bypass ? QString() : tuneZText(p));
        cell(kColInd, p.bypass ? QString() : QString::number(p.ind, 'f', 1), true);
        cell(kColTune, QString::number(p.ampTune, 'f', 1), true);
        cell(kColLoad, QString::number(p.ampLoad, 'f', 1), true);
        cell(kColSwr, p.swr >= 1.0 ? QString::number(p.swr, 'f', 2)
                                   : QStringLiteral("—"), true);
        cell(kColNote, p.note);
        cell(kColSaved, p.savedMs > 0
            ? QDateTime::fromMSecsSinceEpoch(p.savedMs).toString("yyyy-MM-dd")
            : QString());
    }
    grid_->resizeColumnsToContents();
    grid_->horizontalHeader()->setSectionResizeMode(kColNote,
                                                    QHeaderView::Stretch);
    grid_->clearSelection();
    editBtn_->setEnabled(false);
    delBtn_->setEnabled(false);
    qsyBtn_->setEnabled(false);
}

} // namespace ttc
