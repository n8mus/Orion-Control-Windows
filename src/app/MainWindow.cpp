// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/MainWindow.h"
#include "app/Bands.h"
#include "app/MainWindowInternal.h"

#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QWidget>
#include <QTimer>
#include <QToolButton>
#include <QMenu>
#include <QActionGroup>
#include <QWidgetAction>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QApplication>
#include <QHostAddress>
#include <QPushButton>
#include <QInputDialog>
#include <QLineEdit>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUdpSocket>
#include <QFile>
#include <QThread>
#include <cstring>
#include "ui/DisplayPanel.h"
#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"
#include "cw/SkimServer.h"
#include "net/FldigiClient.h"
#include "net/SpotClient.h"
#include "ui/DigiWindow.h"
#include "ui/SkimmerWindow.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ttc {

// kLoOffsetHz / edgesFromRig / styleMenu etc. live in MainWindowInternal.h
// now that MainWindow spans several translation units.

// Spotter-area presets: ctys = CC Cluster spotter-origin country prefixes
// (SET/FILTER DOC/PASS list, coarse-but-practical major spotting countries
// per region); parks = POTA park country codes for the same region. Empty
// ctys = no filter. The Custom menu entry takes any DOC list verbatim.
struct SpotArea { const char* name; const char* ctys; const char* parks; };
static const SpotArea kSpotAreas[] = {
    {"All spotters",  "", ""},
    {"North America", "K,VE,XE,KL,KP4,KP2,CM,C6,ZF,6Y,HI,TI,V3,HP,YS,HR,TG,YN",
                      "US,CA,MX"},
    {"Europe",        "G,GM,GW,GI,GD,GU,GJ,EI,F,DL,PA,ON,LX,HB,OE,I,EA,CT,OK,OM,"
                      "SP,HA,YO,LZ,SV,S5,9A,E7,YU,Z3,OH,SM,LA,OZ,ES,YL,LY,EW,UR,UA",
                      "GB,IE,FR,DE,NL,BE,LU,CH,AT,IT,ES,PT,CZ,SK,PL,HU,RO,BG,GR,"
                      "SI,HR,RS,FI,SE,NO,DK,EE,LV,LT,UA"},
    {"Asia",          "JA,HL,BY,BV,VR,VU,4X,UA9,UA0,HS,9V,9M2,DU,YB,A4,A6,A7,A9,"
                      "HZ,EP,TA",
                      "JP,KR,CN,TW,IN,IL,TH,SG,MY,PH,ID"},
    {"Oceania",       "VK,ZL,KH6,KH2,FK,3D2,P2",
                      "AU,NZ,FJ,PG"},
    {"South America", "PY,LU,CE,CX,HK,YV,OA,HC,ZP,P4,PJ2,PJ4,8R,9Y",
                      "BR,AR,CL,UY,CO,VE,PE,EC,PY"},
    {"Africa",        "ZS,5Z,SU,CN,EA8,V5,9J,7X,3B8,5R,TR,TU,6W,Z2,A2",
                      "ZA,KE,EG,MA,NA,ZM,MU,MG,SN,ZW,BW"},
};

// Radio driver factory: TTC_RADIO env overrides the persisted picker choice.
// "orion" = Ten-Tec Orion 565/566 (ASCII CAT); "omni8" = Omni VII 588
// (binary CAT, RTS/CTS) — the console's Omni 8 personality.
static RadioController* makeRadio(QObject* parent) {
    QString model = qEnvironmentVariable("TTC_RADIO");
    if (model.isEmpty()) model = QSettings().value("radio/model", "orion").toString();
    if (model == "omni8" || model == "omni7")
        return new TenTecOmni7(parent);
    return new TenTecOrion(parent);
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), radio_(makeRadio(this)), rigctld_(radio_) {
    setWindowTitle(radio_->caps().dualReceiver ? "Orion III SDR Console"
                                               : "Omni 8 SDR Console");

    pan_ = new PanadapterWidget(this);
    pan_->setPassband(-1200, 1200);
    pan_->setCenterHz(centerHz_);                  // grid labels need the dial freq
    smeter_ = new SMeterWidget(this);
    // Meter-face lettering names the connected radio (same branch the
    // window title uses).
    smeter_->setWordmark(radio_->caps().dualReceiver ? QStringLiteral("ORION")
                                                     : QStringLiteral("OMNI-VII"));
    panel_  = new ControlPanel(this);
    // Readout colors follow the TX role (matching the routing buttons and
    // panadapter flags): red = the transmitting VFO, green = the other one.
    // Green dimmed to match the red's visual weight — at equal brightness
    // the saturated green reads "bigger" than red at identical pixel size.
    const QColor vfoTxRed(230, 95, 95), vfoRxGreen(62, 190, 112);
    freqDisp_ = new FrequencyDisplay("VFO A", vfoTxRed, this);
    freqDisp_->setFrequency(centerHz_);            // TX starts on A
    freqDispB_ = new FrequencyDisplay("VFO B", vfoRxGreen, this);
    freqDispB_->setFrequency(7000000);             // real value polled at startup
    auto applyVfoColors = [this, vfoTxRed, vfoRxGreen] {
        freqDisp_->setAccent(txVfo_ == 'B' ? vfoRxGreen : vfoTxRed);
        freqDispB_->setAccent(txVfo_ == 'B' ? vfoTxRed : vfoRxGreen);
        // Split reads faster as a word than as a color swap mid-pileup.
        freqDispB_->setBadge(txVfo_ == 'B' ? QStringLiteral("SPLIT") : QString());
    };
    curBand_ = bandIndexOf(centerHz_);
    if (curBand_ >= 0) {
        QSettings s;
        curReg_ = std::clamp(
            s.value(QString("band/%1/reg").arg(kBands[curBand_].label), 0).toInt(),
            0, kStackCount - 1);
    }
    panel_->showBand(curBand_);
    panel_->showBandStack(curBand_ >= 0 ? curReg_ : -1);

    // S-meter above the panadapter, control sidebar on the right.
    auto* central = new QWidget(this);
    auto* lay = new QHBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* left = new QVBoxLayout;
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(0);
    // The meter is compact and fixed-size; park it left in a dark strip so the
    // rest of the row doesn't show the default window color.
    auto* topStrip = new QWidget(this);
    topStrip->setAutoFillBackground(true);
    QPalette strip = topStrip->palette();
    strip.setColor(QPalette::Window, QColor(12, 16, 22));
    topStrip->setPalette(strip);
    // Two-deck arrangement on a GRID so deck 1 items can align exactly with
    // deck 2 columns: row 0 = S-meter, zoom (in VFO B's grid column, so it
    // sits precisely over B's fields), menus at the right edge; row 1 = the
    // VFO columns with the routing matrices between them. `topLay` stays the
    // name of the menu corner so the menu-button code below is untouched.
    // Column plan: 0 smeter/stretch | 1 colA | 2 gap | 3 routing | 4 gap
    //              | 5 colB + zoom | 6 stretch | 7 menus
    auto* topGrid = new QGridLayout(topStrip);
    topGrid->setContentsMargins(10, 4, 10, 4);
    topGrid->setHorizontalSpacing(0);
    topGrid->setVerticalSpacing(3);
    // Symmetry plan: the routing matrices (col 3, with the A/B transfer
    // buttons at their middle) sit DEAD CENTER over the panadapter. That
    // needs both flanks equal: same minimum width for the VFO columns (1/5),
    // same gaps (2/4), same minimum for the outer columns (0/7), and the
    // leftover space split evenly by the two stretch columns (0/6). The VFO
    // boxes anchor inward (A hugs the matrices from the left, B from the
    // right); clock/meter and the menus fall away to the far edges.
    topGrid->setColumnStretch(0, 1);
    topGrid->setColumnStretch(6, 1);
    topGrid->setColumnMinimumWidth(0, 280);
    topGrid->setColumnMinimumWidth(7, 280);
    topGrid->setColumnMinimumWidth(2, 24);
    topGrid->setColumnMinimumWidth(4, 24);
    // Columns 1/5 get equal minimums computed AFTER the VFO columns are
    // built (B is naturally wider — VIEW button), see below.
    auto* topLay = new QHBoxLayout;
    topLay->setContentsMargins(0, 0, 0, 0);
    // Column 0, spanning both decks: just the meter, pushed right so it sits
    // beside VFO A. (The clock moved to the bottom of the radio panel — its
    // fixed width was pushing the window past narrow screens.)
    {
        auto* col0 = new QHBoxLayout;
        col0->setContentsMargins(0, 0, 0, 0);
        col0->addStretch(1);
        col0->addWidget(smeter_, 0, Qt::AlignVCenter);
        col0->addSpacing(14);
        topGrid->addLayout(col0, 0, 0, 2, 1);
    }
    // One ticker updates the clock and keeps the VFO band labels honest
    // (external clients can move the dials any time).
    auto* uiTick = new QTimer(this);
    connect(uiTick, &QTimer::timeout, this, [this] {
        const QDateTime now = QDateTime::currentDateTime();
        panel_->showClock(now.toUTC().toString("HH:mm:ss' UTC'"),
                          now.toString("HH:mm:ss' local'"));
        const int ba = bandIndexOf(centerHz_);
        freqDisp_->setBandText(ba >= 0 ? QString("%1m").arg(kBands[ba].label)
                                       : QString());
        const int bb = bandIndexOf(vfoBHz_);
        freqDispB_->setBandText(bb >= 0 ? QString("%1m").arg(kBands[bb].label)
                                        : QString());
    });
    uiTick->start(1000);
    topGrid->addLayout(topLay, 0, 7, Qt::AlignRight | Qt::AlignVCenter);
    // Second menu deck, in the empty row under the first: LOG/CW landed in
    // row 0 originally and pushed the window minimum past the screen (the
    // maximize button vanished — same width creep as the old clock). New
    // buttons go here, not in topLay.
    auto* topLay2 = new QHBoxLayout;
    topLay2->setContentsMargins(0, 0, 0, 0);
    topGrid->addLayout(topLay2, 1, 7, Qt::AlignRight | Qt::AlignVCenter);
    topStrip_ = topStrip;              // the tool builders hang things here
    topLay2_  = topLay2;
    // Zoom (log scale, 0 = full span .. 100 = deepest zoom); Ctrl+wheel on
    // the panadapter still works and keeps the slider in sync. Level with
    // the S-meter, spanning VFO B's column: label at B's left edge, slider
    // running to the column's right edge (the VIEW button's edge).
    auto* zoomLbl = new QLabel("ZOOM", topStrip);
    zoomLbl->setStyleSheet("color: #8fa3b8; font-size: 10px; font-weight: bold;");
    auto* zoom = new QSlider(Qt::Horizontal, topStrip);
    zoom->setRange(0, 100);
    zoom->setToolTip("Zoom (span) — Ctrl+wheel on the panadapter does the same");
    zoom->setStyleSheet(
        "QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;"
        " background: #6aa5d8; }");
    auto* zoomRow = new QHBoxLayout;
    zoomRow->setContentsMargins(0, 0, 0, 0);
    zoomRow->addWidget(zoomLbl);
    zoomRow->addSpacing(4);
    zoomRow->addWidget(zoom, 1);                    // fills the rest of B's column
    topGrid->addLayout(zoomRow, 0, 5);
    // Each VFO is a column: readout on top, its volume + mute right below
    // (KE9NS-style rounded buttons, matching the sidebar controls).
    auto makeVolRow = [&](int i) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        volSl_[i] = new QSlider(Qt::Horizontal, topStrip);
        volSl_[i]->setRange(0, 100);
        volSl_[i]->setFixedWidth(128);
        volSl_[i]->setFocusPolicy(Qt::NoFocus);
        volSl_[i]->setStyleSheet(
            "QSlider::groove:horizontal { height: 5px; background: #2a3644; border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px;"
            " background: #6aa5d8; }");
        volLbl_[i] = new QLabel("--", topStrip);
        volLbl_[i]->setFixedWidth(22);
        volLbl_[i]->setStyleSheet("color: #8fa3b8; font-size: 11px; font-weight: bold;");
        muteBtn_[i] = new QToolButton(topStrip);
        muteBtn_[i]->setText("MUTE");
        muteBtn_[i]->setCheckable(true);
        muteBtn_[i]->setFocusPolicy(Qt::NoFocus);
        muteBtn_[i]->setFixedHeight(19);
        muteBtn_[i]->setStyleSheet(
            "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
            " border-radius: 3px; color: #8fa3b8; font-size: 10px; font-weight: bold;"
            " padding: 0 8px; }"
            "QToolButton:hover { border-color: #4a5a6e; }"
            "QToolButton:checked { background: #8a2727; border-color: #e05d5d;"
            " color: #ffecec; }");
        // LOCK: sends the Orion's *AL/*BL (freezes the front-panel knob) and
        // makes the console refuse its own tune gestures on that VFO — the
        // radio still accepts CAT tuning while locked (live-verified), so
        // both sides have to hold the line.
        lockBtn_[i] = new QToolButton(topStrip);
        lockBtn_[i]->setText("LOCK");
        lockBtn_[i]->setCheckable(true);
        lockBtn_[i]->setFocusPolicy(Qt::NoFocus);
        lockBtn_[i]->setFixedHeight(19);
        lockBtn_[i]->setToolTip(QString("Lock VFO %1: front-panel knob and all "
                                        "console tuning (filter edges stay live)")
                                    .arg(i == 0 ? 'A' : 'B'));
        lockBtn_[i]->setStyleSheet(
            "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
            " border-radius: 3px; color: #8fa3b8; font-size: 10px; font-weight: bold;"
            " padding: 0 8px; }"
            "QToolButton:hover { border-color: #4a5a6e; }"
            "QToolButton:checked { background: #8a6a27; border-color: #e0b45d;"
            " color: #fff6e0; }");
        connect(lockBtn_[i], &QToolButton::toggled, this, [this, i](bool on) {
            (i == 0 ? vfoLockA_ : vfoLockB_) = on;
            radio_->setVfoLock(i == 0 ? 'A' : 'B', on);
            pan_->setVfoLocks(vfoLockA_, vfoLockB_);
            statusBar()->showMessage(QString("VFO %1 %2")
                                         .arg(i == 0 ? 'A' : 'B')
                                         .arg(on ? "LOCKED" : "unlocked"));
        });
        row->addSpacing(4);
        row->addWidget(volSl_[i]);
        row->addWidget(volLbl_[i]);
        row->addWidget(muteBtn_[i]);
        row->addWidget(lockBtn_[i]);
        row->addStretch(1);
        return row;
    };
    auto* colA = new QVBoxLayout;
    colA->setSpacing(3);
    colA->addWidget(freqDisp_);
    colA->addLayout(makeVolRow(0));
    topGrid->addLayout(colA, 1, 1, Qt::AlignRight);   // hug the center block
    routing_ = new RoutingPanel(topStrip);
    topGrid->addWidget(routing_, 1, 3);
    auto* colB = new QVBoxLayout;
    colB->setSpacing(3);
    colB->addWidget(freqDispB_);
    auto* rowB = makeVolRow(1);
    // VIEW: show/hide VFO B on the panadapter — declutters the display when
    // the sub VFO isn't in play. B keeps tracking underneath; right-clicking
    // to drop B on a station turns the view back on.
    bViewBtn_ = new QToolButton(topStrip);
    bViewBtn_->setText("VIEW");
    bViewBtn_->setCheckable(true);
    bViewBtn_->setFocusPolicy(Qt::NoFocus);
    bViewBtn_->setFixedHeight(19);
    bViewBtn_->setToolTip("Show VFO B on the panadapter (uncheck to hide it "
                          "when the second VFO isn't needed)");
    bViewBtn_->setStyleSheet(
        "QToolButton { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; color: #8fa3b8; font-size: 10px; font-weight: bold;"
        " padding: 0 8px; }"
        "QToolButton:hover { border-color: #4a5a6e; }"
        "QToolButton:checked { background: #2f6d9e; border-color: #5db2f0;"
        " color: #eaf6ff; }");
    bViewBtn_->setChecked(QSettings().value("display/showVfoB", true).toBool());
    pan_->setVfoBVisible(bViewBtn_->isChecked());
    connect(bViewBtn_, &QToolButton::toggled, this, [this](bool on) {
        QSettings().setValue("display/showVfoB", on);
        pan_->setVfoBVisible(on);
        statusBar()->showMessage(on ? "VFO B shown on the panadapter"
                                    : "VFO B hidden from the panadapter");
    });
    rowB->addWidget(bViewBtn_);
    colB->addLayout(rowB);
    topGrid->addLayout(colB, 1, 5, Qt::AlignLeft);    // hug the center block
    {   // Equalize the VFO columns so the transfer buttons (center of the
        // routing block) sit exactly over the panadapter's center line.
        const int sym = std::max(colA->sizeHint().width(),
                                 colB->sizeHint().width());
        topGrid->setColumnMinimumWidth(1, sym);
        topGrid->setColumnMinimumWidth(5, sym);
    }

    auto spanFromSlider = [this](int v) {
        const double ratio = double(pan_->minViewSpanHz()) / pan_->maxViewSpanHz();
        return static_cast<int>(std::lround(pan_->maxViewSpanHz() * std::pow(ratio, v / 100.0)));
    };
    auto sliderFromSpan = [this](int spanHz) {
        const double ratio = double(pan_->minViewSpanHz()) / pan_->maxViewSpanHz();
        const double f = std::log(double(spanHz) / pan_->maxViewSpanHz()) / std::log(ratio);
        return std::clamp(static_cast<int>(std::lround(f * 100.0)), 0, 100);
    };
    connect(zoom, &QSlider::valueChanged, this, [this, spanFromSlider](int v) {
        pan_->setViewSpanHz(spanFromSlider(v));
        statusBar()->showMessage(
            QString("zoom -> span %1 kHz").arg(pan_->viewSpanHz() / 1000.0, 0, 'f', 1));
    });
    connect(pan_, &PanadapterWidget::viewSpanChanged, this,
            [zoom, sliderFromSpan](int spanHz) {
                QSignalBlocker block(zoom);
                zoom->setValue(sliderFromSpan(spanHz));
            });

    // All toolbar dropdowns share the console's dark look — without this the
    // QMenu frame and plain actions render in the system (light) theme.

    // "DISPLAY" dropdown: Flex/KE9NS-style viewer settings (ref level, range,
    // averaging, waterfall speed, palette, fill, peak hold), applied live and
    // persisted. Sits in a QWidgetAction so the popup stays open while dragging.
    auto* dispBtn = new QToolButton(topStrip);
    dispBtn->setText("DISPLAY ▾");
    dispBtn->setPopupMode(QToolButton::InstantPopup);
    dispBtn->setFocusPolicy(Qt::NoFocus);
    dispBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* dispMenu = new QMenu(dispBtn);
    styleMenu(dispMenu);
    auto* dispPanel = new DisplayPanel(dispMenu);
    auto* dispAction = new QWidgetAction(dispMenu);
    dispAction->setDefaultWidget(dispPanel);
    dispMenu->addAction(dispAction);
    dispBtn->setMenu(dispMenu);
    topLay->addSpacing(8);
    topLay->addWidget(dispBtn);
    auto saveDisplay = [](const DisplaySettings& d) {
        QSettings s;
        s.setValue("display/refDb",   d.refDb);
        s.setValue("display/rangeDb", d.rangeDb);
        s.setValue("display/palette", d.palette);
        s.setValue("display/avg",     d.avgFrames);
        s.setValue("display/wfSpeed", d.wfSpeed);
        s.setValue("display/fill",    d.fillTrace);
        s.setValue("display/peak",    d.peakHold);
        s.setValue("display/split",   d.split);
        s.setValue("display/background", d.background);
        s.setValue("display/mapDay",     d.mapDay);
        s.setValue("display/mapNight",   d.mapNight);
        s.setValue("display/grid",       d.showGrid);
        s.setValue("display/callsign",   d.showCall);
        s.setValue("display/solar",      d.showSolar);
        s.setValue("display/rose",       d.showRose);
        s.setValue("display/bandplan",   d.showBandPlan);
        s.setValue("display/trace",      d.traceColor);
        s.setValue("display/bigVfo",     d.bigVfo);
        s.setValue("display/clock",      d.showClock);
        s.setValue("display/wfTime",     d.showWfTime);
        s.setValue("display/cwZap",      d.cwZap);
    };
    // Station callsign: drives the watermark and defaults the cluster login.
    // Editable in the DISPLAY panel (CALL field), persisted immediately.
    QString stationCall;
    {
        QSettings s;
        stationCall = s.value("station/callsign", "N8EM").toString();
    }
    dispPanel->setCallsign(stationCall);
    pan_->setCallsign(stationCall);
    // Station grid square -> QTH for the compass rose; cty.dat places DX
    // calls for the bearing pointer. EN82 (SE Michigan) until Jon sets his.
    cty_.load();
    {
        QSettings s;
        const QString grid = s.value("station/grid", "EN83al").toString();
        dispPanel->setGrid(grid);
        double la, lo;
        if (CtyLookup::gridToLatLon(grid, la, lo)) pan_->setQth(la, lo);
    }
    connect(dispPanel, &DisplayPanel::gridChanged, this, [this](const QString& g) {
        QSettings().setValue("station/grid", g);
        double la, lo;
        if (CtyLookup::gridToLatLon(g, la, lo)) {
            pan_->setQth(la, lo);
            statusBar()->showMessage(QString("QTH grid %1 (%2, %3)")
                                         .arg(g).arg(la, 0, 'f', 1).arg(lo, 0, 'f', 1));
        }
    });
    // US 60 m allocation per FCC 25-60, effective 2026-02-13 (KE9NS-style
    // zone boxes): contiguous 5351.5-5366.5 kHz at 15 W EIRP / 2.8 kHz max
    // bandwidth / any compliant mode, plus the four retained USB channels
    // (2.8 kHz wide, 100 W ERP, USB/CW/digital). Old channel 3 (5358.5) is
    // gone as a channel — it sits inside the band at the lower power limit.
    pan_->setBandZones({
        {5330600, 5333400, "60m CH1 100W"},
        {5346600, 5349400, "60m CH2 100W"},
        {5351500, 5366500, "60m 5351.5-5366.5 · 15W EIRP · ≤2.8k"},
        {5371600, 5374400, "60m CH4 100W"},
        {5403600, 5406400, "60m CH5 100W"},
    });
    connect(dispPanel, &DisplayPanel::callsignChanged, this,
            [this](const QString& call) {
                QSettings().setValue("station/callsign", call);
                pan_->setCallsign(call);
            });
    {   // restore persisted display settings before first paint
        QSettings s;
        DisplaySettings d;
        d.refDb     = s.value("display/refDb",   d.refDb).toFloat();
        d.rangeDb   = s.value("display/rangeDb", d.rangeDb).toFloat();
        d.palette   = s.value("display/palette", d.palette).toInt();
        d.avgFrames = s.value("display/avg",     d.avgFrames).toInt();
        d.wfSpeed   = s.value("display/wfSpeed", d.wfSpeed).toInt();
        d.fillTrace = s.value("display/fill",    d.fillTrace).toBool();
        d.peakHold  = s.value("display/peak",    d.peakHold).toBool();
        d.split     = s.value("display/split",   d.split).toFloat();
        d.background = s.value("display/background", d.background).toInt();
        d.mapDay     = s.value("display/mapDay",     d.mapDay).toInt();
        d.mapNight   = s.value("display/mapNight",   d.mapNight).toInt();
        d.showGrid   = s.value("display/grid",       d.showGrid).toBool();
        d.showCall   = s.value("display/callsign",   d.showCall).toBool();
        d.showSolar  = s.value("display/solar",      d.showSolar).toBool();
        d.showRose   = s.value("display/rose",       d.showRose).toBool();
        d.showBandPlan = s.value("display/bandplan", d.showBandPlan).toBool();
        d.traceColor = s.value("display/trace",      d.traceColor).toInt();
        d.bigVfo     = s.value("display/bigVfo",     d.bigVfo).toBool();
        d.showClock  = s.value("display/clock",      d.showClock).toBool();
        d.showWfTime = s.value("display/wfTime",     d.showWfTime).toBool();
        d.cwZap      = s.value("display/cwZap",      d.cwZap).toBool();
        cwZap_       = d.cwZap;
        dispPanel->setSettings(d);
        pan_->setDisplaySettings(d);
        freqDisp_->setLargeDigits(d.bigVfo);
        freqDispB_->setLargeDigits(d.bigVfo);
        panel_->setClockVisible(d.showClock);
        // The "Solar data panel" toggle governs everything solar (corner
        // panel AND the map sun marker), so it alone gates the NOAA poller.
        solarClient_.setEnabled(d.showSolar);
    }
    connect(&solarClient_, &SolarClient::updated, this, [this] {
        const SolarData sd = solarClient_.data();
        pan_->setSolarInfo(sd.sfi, sd.aIdx, sd.kIdx, sd.ssn, sd.xray);
    });
    connect(dispPanel, &DisplayPanel::settingsChanged, this,
            [this, saveDisplay](const DisplaySettings& d) {
                pan_->setDisplaySettings(d);
                freqDisp_->setLargeDigits(d.bigVfo);
                freqDispB_->setLargeDigits(d.bigVfo);
                panel_->setClockVisible(d.showClock);
                solarClient_.setEnabled(d.showSolar);
                cwZap_ = d.cwZap;
                saveDisplay(d);
            });
    // In-widget edits (dB-axis drag/wheel, divider drag) flow back the other
    // way: persist them and keep the DISPLAY panel's sliders in sync.
    connect(pan_, &PanadapterWidget::displaySettingsEdited, this,
            [this, dispPanel, saveDisplay](const DisplaySettings& d) {
                dispPanel->setSettings(d);
                saveDisplay(d);
                // In-widget edits are easy to make without noticing — say so.
                statusBar()->showMessage(
                    QString("display: REF %1 dB  RANGE %2 dB")
                        .arg(d.refDb, 0, 'f', 0).arg(d.rangeDb, 0, 'f', 0));
            });

    // Pinned frequency markers: Shift+right-click on the panadapter toggles
    // one (label prompt on create). The list lives in QSettings as
    // "hz|label" strings and survives restarts — that permanence is what
    // separates a marker from a spot.
    {
        QVector<FreqMarker> mk;
        for (const QString& e :
             QSettings().value("panadapter/markers").toStringList()) {
            const int bar = e.indexOf('|');
            FreqMarker m;
            m.hz = e.left(bar < 0 ? e.size() : bar).toLongLong();
            if (bar >= 0) m.label = e.mid(bar + 1);
            if (m.hz > 0) mk.append(m);
        }
        markers_ = mk;
        pan_->setMarkers(markers_);
    }
    connect(pan_, &PanadapterWidget::markerToggleRequested, this,
            [this](qint64 hz) {
                // Near an existing marker (a few pixels at the current
                // zoom) = remove it; otherwise create one here.
                const qint64 tol =
                    std::max<qint64>(300, pan_->viewSpanHz() / 250);
                for (int i = 0; i < markers_.size(); ++i) {
                    if (std::llabs(markers_[i].hz - hz) <= tol) {
                        statusBar()->showMessage(
                            QString("marker removed: %1 kHz")
                                .arg(markers_[i].hz / 1000.0, 0, 'f', 1));
                        markers_.removeAt(i);
                        saveMarkers();
                        return;
                    }
                }
                bool ok = false;
                const QString label = QInputDialog::getText(
                    this, "Pin frequency marker",
                    QString("Label for %1 kHz (empty shows the frequency):")
                        .arg(hz / 1000.0, 0, 'f', 1),
                    QLineEdit::Normal, QString(), &ok);
                if (!ok) return;                   // canceled: no marker
                markers_.append({hz, label.trimmed()});
                saveMarkers();
            });

    // "SPOTS" dropdown: DX-cluster callsign labels on the panadapter. The feed
    // is a plain cluster telnet login (default VE7CC); host/port/login live in
    // QSettings (spots/host, spots/port, spots/login) for other nodes or RBN.
    auto* spotsBtn = new QToolButton(topStrip);
    spotsBtn->setText("SPOTS ▾");
    spotsBtn->setPopupMode(QToolButton::InstantPopup);
    spotsBtn->setFocusPolicy(Qt::NoFocus);
    spotsBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* spotsMenu = new QMenu(spotsBtn);
    styleMenu(spotsMenu);
    auto* spotsOn   = spotsMenu->addAction("Show spots");
    spotsOn->setCheckable(true);
    // Per-source toggles, so any mix works: just FT8 for a digital session,
    // just POTA for park hunting, everything for a contest.
    auto* dxOn = spotsMenu->addAction("DX spots");
    dxOn->setCheckable(true);
    dxOn->setToolTip("Classic human cluster spots (yellow labels)");
    auto* potaOn = spotsMenu->addAction("POTA spots");
    potaOn->setCheckable(true);
    potaOn->setToolTip("Park activations from api.pota.app (green labels "
                       "with the park reference)");
    auto* ft8On = spotsMenu->addAction("FT8 spots");
    ft8On->setCheckable(true);
    ft8On->setToolTip("FT8 skimmer spots from the cluster node (cyan labels, "
                      "placed at the station's actual offset in the watering hole)");
    // Spotter area: only show what's being heard in a chosen part of the
    // world (server-side CC Cluster spotter-origin filter); the same region
    // choice narrows POTA to that region's parks. Custom takes any CC
    // country-prefix list for slicing the world however the operator likes.
    auto* areaMenu = spotsMenu->addMenu("Spotter area");
    styleMenu(areaMenu);
    auto* areaGrp = new QActionGroup(spotsMenu);
    QVector<QAction*> areaActs;
    for (const SpotArea& a : kSpotAreas) {
        auto* act = areaMenu->addAction(a.name);
        act->setCheckable(true);
        act->setActionGroup(areaGrp);
        areaActs.push_back(act);
    }
    auto* areaCustom = areaMenu->addAction("Custom…");
    areaCustom->setCheckable(true);
    areaCustom->setActionGroup(areaGrp);
    auto* spotsClear = spotsMenu->addAction("Clear spots");
    spotsMenu->addSeparator();
    // DX watch: patterns matched against every spot; hits get an orange
    // alert ring, a status-bar callout and (optionally) a beep.
    watch_.setPatterns(QSettings().value("spots/watch").toString());
    auto* watchEdit = spotsMenu->addAction(
        QString("Watch list… (%1)").arg(watch_.count()));
    watchEdit->setToolTip(
        "Alert patterns, one per line or space-separated:\n"
        "  W1AW      exact call\n"
        "  JA*       prefix\n"
        "  US-1234   POTA park\n"
        "  NEW       anything you never worked (red spots)\n"
        "Hits ring orange on the panadapter and call out in the status bar.");
    auto* watchBeep = spotsMenu->addAction("Watch alert beep");
    watchBeep->setCheckable(true);
    watchBeep->setChecked(QSettings().value("spots/watchBeep", true).toBool());
    connect(watchBeep, &QAction::toggled, this, [](bool on) {
        QSettings().setValue("spots/watchBeep", on);
    });
    spotsMenu->addSeparator();
    {   // Worked-before legend (colors fed from the cqrlog logbook).
        auto* legend = spotsMenu->addAction(
            "Call colors: red new · amber new band · green worked · gray cfmd");
        legend->setEnabled(false);
        legend->setToolTip(
            "Spot callsigns are colored from your cqrlog logbook:\n"
            "red = never worked, amber = worked but not on this band,\n"
            "green = worked this band, gray = confirmed (card/LoTW/eQSL).\n"
            "POTA spots color by the PARK (red = never hunted).\n"
            "A small edge bar keeps the source color (yellow DX / green "
            "POTA / cyan FT8 /\nviolet SKIM).\nPlain colors mean no logbook "
            "data yet — start cqrlog once to load it.");
    }
    spotsMenu->addSeparator();
    {
        QSettings s;
        const QString host  = s.value("spots/host", "dxc.ve7cc.net").toString();
        const quint16 port  = static_cast<quint16>(s.value("spots/port", 23).toUInt());
        const QString login = s.value("spots/login", stationCall).toString();
        auto* info = spotsMenu->addAction(QString("source: %1:%2 as %3")
                                              .arg(host).arg(port).arg(login));
        info->setEnabled(false);
        spotClient_.configure(host, port, login);
        spotsOn->setChecked(s.value("spots/enabled", true).toBool());
        dxOn->setChecked(s.value("spots/dx", true).toBool());
        potaOn->setChecked(s.value("spots/pota", true).toBool());
        ft8On->setChecked(s.value("spots/ft8", true).toBool());
    }
    spotsBtn->setMenu(spotsMenu);
    topLay->addSpacing(8);
    topLay->addWidget(spotsBtn);

    // The second-deck tools (LOG/CW/SKIM/DIGI/ROT) each build in their own
    // function in MainWindowTools.cpp — one call per feature here, so
    // parallel feature work doesn't collide in this file.
    setupLogUi();
    setupCwUi();
    setupSkimUi(stationCall);
    setupDigiUi();
    setupRotorUi();

    // "AUDIO" dropdown: per-receiver volume + mute and the Orion's output
    // routing (*UC) — including one-VFO-per-ear for split pileups.
    auto* audioBtn = new QToolButton(topStrip);
    audioBtn->setText("AUDIO ▾");
    audioBtn->setPopupMode(QToolButton::InstantPopup);
    audioBtn->setFocusPolicy(Qt::NoFocus);
    audioBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* audioMenu = new QMenu(audioBtn);
    styleMenu(audioMenu);
    audioPanel_ = new AudioPanel(audioMenu);
    auto* audioAction = new QWidgetAction(audioMenu);
    audioAction->setDefaultWidget(audioPanel_);
    audioMenu->addAction(audioAction);
    audioBtn->setMenu(audioMenu);
    topLay->addSpacing(8);
    topLay->addWidget(audioBtn);

    // Per-VFO volume + mute (the rows under the readouts). Sliding a muted
    // receiver's volume implicitly unmutes it; mute remembers the level.
    for (int i = 0; i < 2; ++i) {
        const Rx rx = i == 0 ? Rx::Main : Rx::Sub;
        volTx_[i] = new QTimer(this);
        volTx_[i]->setSingleShot(true);
        volTx_[i]->setInterval(60);
        connect(volTx_[i], &QTimer::timeout, this,
                [this, i, rx] { radio_->setAfVolume(rx, pendVol_[i]); });
        connect(volSl_[i], &QSlider::valueChanged, this, [this, i, rx](int v) {
            volLbl_[i]->setText(QString::number(v));
            vol_[i] = v;
            if (muted_[i]) {                        // touch = unmute
                muted_[i] = false;
                const QSignalBlocker b(muteBtn_[i]);
                muteBtn_[i]->setChecked(false);
            }
            pendVol_[i] = v;
            volTx_[i]->start();
            statusBar()->showMessage(QString("volume %1 -> %2")
                                         .arg(i == 0 ? "A" : "B").arg(v));
        });
        connect(muteBtn_[i], &QToolButton::toggled, this, [this, i, rx](bool on) {
            muted_[i] = on;
            if (on) {
                preMute_[i] = vol_[i];
                radio_->setAfVolume(rx, 0);
            } else {
                radio_->setAfVolume(rx, preMute_[i]);
                vol_[i] = preMute_[i];
                const QSignalBlocker b(volSl_[i]);
                volSl_[i]->setValue(preMute_[i]);
                volLbl_[i]->setText(QString::number(preMute_[i]));
            }
            statusBar()->showMessage(QString("VFO %1 audio %2")
                                         .arg(i == 0 ? "A" : "B")
                                         .arg(on ? "MUTED" : "unmuted"));
        });
    }
    connect(audioPanel_, &AudioPanel::routingEdited, this,
            [this](char l, char r, char s) {
                radio_->setAudioRouting(l, r, s);
                statusBar()->showMessage(
                    QString("audio: phones L=%1 R=%2  speaker=%3").arg(l).arg(r).arg(s));
            });
    connect(radio_, &RadioController::audioRoutingReported, this,
            [this](char l, char r, char s) { audioPanel_->showRouting(l, r, s); });

    // Cluster spots carry no location: place them by cty.dat prefix (country
    // center — good enough for a bearing), memoized per call so the FT8-rate
    // feed never rescans the prefix table.
    const auto ctyPlace = [this](SpotLabel& l) {
        auto it = ctyMemo_.constFind(l.call);
        if (it == ctyMemo_.constEnd()) {
            double la = 999.0, lo = 999.0;
            cty_.lookup(l.call, la, lo);
            it = ctyMemo_.insert(l.call, qMakePair(la, lo));
        }
        l.lat = it->first;
        l.lon = it->second;
    };
    // Worked-before status from the cqrlog logbook: POTA spots color by the
    // park (hunted or not — that's the POTA question), everything else by
    // call + band. No log data yet = '?' = plain kind colors.
    const auto logStatus = [this](const SpotLabel& l) -> char {
        if (!logbook_.ready()) return '?';
        if (l.kind == 'P')
            return logbook_.parkHunted(l.tag) ? 'W' : 'N';
        return logbook_.status(l.call, LogbookIndex::bandForHz(l.hz))
            .toLatin1();
    };
    auto pushSpots = [this, spotsOn, dxOn, potaOn, ft8On, ctyPlace, logStatus,
                      watchBeep] {
        QVector<SpotLabel> labels;
        if (spotsOn->isChecked()) {
            QSet<QString> seen;                    // POTA API entry wins (has
            if (potaOn->isChecked())               // the park ref) over the
                for (const Spot& s : potaClient_.spots()) {  // cluster's copy
                    // The spotter-area choice narrows POTA to that region's
                    // parks (ref prefix, "US-2654" -> "US").
                    if (!parkFilter_.isEmpty()
                        && !parkFilter_.contains(s.tag.section('-', 0, 0)))
                        continue;
                    SpotLabel l{s.call, s.hz, s.atSecs, s.kind, s.tag,
                                s.lat, s.lon};
                    l.status = logStatus(l);
                    labels.push_back(l);
                    seen.insert(s.call);
                }
            for (const Spot& s : spotClient_.spots()) {
                if (s.kind == 'D' && !dxOn->isChecked()) continue;
                if (s.kind == 'F' && !ft8On->isChecked()) continue;
                if (s.kind == 'P' && !potaOn->isChecked()) continue;
                if (seen.contains(s.call)) continue;
                SpotLabel l{s.call, s.hz, s.atSecs, s.kind, s.tag, s.lat, s.lon};
                if (l.lat > 500.0) ctyPlace(l);
                l.status = logStatus(l);
                labels.push_back(l);
            }
        }
        // Skimmer finds ride along whenever the skimmer runs — they're this
        // station's own decodes, independent of the cluster feeds.
        if (skim_->enabled())
            for (const auto& s : skim_->spots()) {
                SpotLabel l{s.call, s.hz, s.atSecs, 'S',
                            s.wpm > 0 ? QString("%1w").arg(s.wpm) : QString()};
                ctyPlace(l);
                l.status = logStatus(l);
                labels.push_back(l);
            }
        // DX watch: flag hits, call out fresh ones (once per call per
        // half hour so a busy feed doesn't nag).
        if (!watch_.empty()) {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            for (SpotLabel& l : labels) {
                l.alert = watch_.matches(
                    l.call, l.kind == 'P' ? l.tag : QString(),
                    QChar::fromLatin1(l.status));
                if (!l.alert) continue;
                const qint64 last = watchNotified_.value(l.call, 0);
                if (now - last < 1800) continue;
                watchNotified_[l.call] = now;
                statusBar()->showMessage(
                    QString("WATCH: %1 on %2 kHz")
                        .arg(l.call).arg(l.hz / 1000.0, 0, 'f', 1),
                    10000);
                if (std::getenv("TTC_SELFTEST"))
                    std::fprintf(stderr, "[watch] %s %.1f kHz\n",
                                 qPrintable(l.call), l.hz / 1000.0);
                if (watchBeep->isChecked()) QApplication::beep();
            }
        }
        pan_->setSpots(labels);
    };
    connect(skim_, &SkimmerEngine::spotsChanged, this, pushSpots);
    connect(watchEdit, &QAction::triggered, this,
            [this, watchEdit, pushSpots] {
                bool ok = false;
                const QString t = QInputDialog::getMultiLineText(
                    this, "DX watch list",
                    "Alert patterns — calls, prefixes (JA*), parks "
                    "(US-1234), NEW for\nnever-worked; one per line:",
                    watch_.patterns(), &ok);
                if (!ok) return;
                watch_.setPatterns(t);
                QSettings().setValue("spots/watch", t);
                watchEdit->setText(
                    QString("Watch list… (%1)").arg(watch_.count()));
                watchNotified_.clear();
                pushSpots();
            });
    connect(skimEnable_, &QAction::toggled, this, [this, pushSpots](bool on) {
        QSettings().setValue("skim/enabled", on);
        skim_->setEnabled(on);
        const quint16 sp =
            quint16(QSettings().value("skim/telnetPort", 7300).toUInt());
        if (on) skimSrv_->start(sp);
        else skimSrv_->stop();
        statusBar()->showMessage(on ? "CW skimmer ON (violet spots)"
                                    : "CW skimmer off", 5000);
        pushSpots();
    });
    if (skimEnable_->isChecked())
        skimSrv_->start(
            quint16(QSettings().value("skim/telnetPort", 7300).toUInt()));
    // Recolor live when the logbook loads or a bridged QSO lands in it.
    connect(&logbook_, &LogbookIndex::updated, this, pushSpots);
    logbook_.start();
    connect(&spotClient_, &SpotClient::spotsChanged, this, pushSpots);
    connect(&spotClient_, &SpotClient::statusChanged, this,
            [this](const QString& s) { statusBar()->showMessage(s, 4000); });
    connect(&potaClient_, &PotaClient::spotsChanged, this, pushSpots);
    connect(&potaClient_, &PotaClient::statusChanged, this,
            [this](const QString& s) { statusBar()->showMessage(s, 4000); });
    connect(spotsOn, &QAction::toggled, this, [this, pushSpots, potaOn](bool on) {
        QSettings().setValue("spots/enabled", on);
        spotClient_.setEnabled(on);
        potaClient_.setEnabled(on && potaOn->isChecked());
        pushSpots();
    });
    connect(potaOn, &QAction::toggled, this, [this, pushSpots, spotsOn](bool on) {
        QSettings().setValue("spots/pota", on);
        potaClient_.setEnabled(on && spotsOn->isChecked());
        pushSpots();
    });
    connect(ft8On, &QAction::toggled, this, [this, pushSpots](bool on) {
        QSettings().setValue("spots/ft8", on);
        spotClient_.setFt8Wanted(on);              // reconfigures the node live
        pushSpots();
    });
    connect(dxOn, &QAction::toggled, this, [pushSpots](bool on) {
        QSettings().setValue("spots/dx", on);
        pushSpots();
    });
    // Spotter area: push the country list to the node (live if connected),
    // remember the park scope for POTA, and re-filter what's on screen.
    const auto applyArea = [this, pushSpots](const QString& ctys,
                                             const QString& parks,
                                             const QString& label) {
        spotClient_.setSpotterFilter(ctys);
        parkFilter_ = parks.isEmpty() ? QStringList()
                                      : parks.split(',', Qt::SkipEmptyParts);
        pushSpots();
        statusBar()->showMessage(ctys.isEmpty()
            ? QStringLiteral("spots: all spotters worldwide")
            : QString("spots: spotters filtered to %1").arg(label));
    };
    for (int i = 0; i < static_cast<int>(std::size(kSpotAreas)); ++i)
        connect(areaActs[i], &QAction::triggered, this, [applyArea, i] {
            QSettings().setValue("spots/area", kSpotAreas[i].name);
            applyArea(kSpotAreas[i].ctys, kSpotAreas[i].parks, kSpotAreas[i].name);
        });
    connect(areaCustom, &QAction::triggered, this,
            [this, applyArea, areaActs, areaCustom] {
        QSettings s;
        bool ok = false;
        const QString list = QInputDialog::getText(this,
            "Custom spotter filter",
            "Spotter country prefixes (CC Cluster list, e.g. K,VE,XE):",
            QLineEdit::Normal,
            s.value("spots/areaCustomCty", "K,VE,XE").toString(), &ok)
            .trimmed().toUpper();
        if (!ok || list.isEmpty()) {               // canceled: restore the check
            const QString cur = s.value("spots/area", "North America").toString();
            for (int i = 0; i < static_cast<int>(std::size(kSpotAreas)); ++i)
                if (cur == kSpotAreas[i].name) areaActs[i]->setChecked(true);
            if (cur == "Custom") areaCustom->setChecked(true);
            return;
        }
        s.setValue("spots/area", "Custom");
        s.setValue("spots/areaCustomCty", list);
        applyArea(list, QString(), "custom (" + list + ")");
    });
    connect(spotsClear, &QAction::triggered, this, [this] { spotClient_.clear(); });
    {   // Restore the persisted area choice (default: North America).
        QSettings s;
        const QString cur = s.value("spots/area", "North America").toString();
        bool found = false;
        for (int i = 0; i < static_cast<int>(std::size(kSpotAreas)); ++i)
            if (cur == kSpotAreas[i].name) {
                areaActs[i]->setChecked(true);
                applyArea(kSpotAreas[i].ctys, kSpotAreas[i].parks,
                          kSpotAreas[i].name);
                found = true;
            }
        if (!found) {                              // stored custom list
            areaCustom->setChecked(true);
            applyArea(s.value("spots/areaCustomCty", "").toString(), QString(),
                      "custom");
        }
    }
    spotClient_.setFt8Wanted(ft8On->isChecked());  // before connect: config
    spotClient_.setEnabled(spotsOn->isChecked());  // rides the login sequence
    potaClient_.setEnabled(spotsOn->isChecked() && potaOn->isChecked());

#ifdef HAVE_SDRPLAY
    // Compact "SDR" dropdown for RSP2 hardware controls (hidden until clicked).
    // Antenna A = Orion spare jack, B = Omni VII spare jack — switch it when you
    // switch radios; it retunes the panadapter input live.
    auto* sdrBtn = new QToolButton(topStrip);
    sdrBtn->setText("SDR ▾");
    sdrBtn->setPopupMode(QToolButton::InstantPopup);
    sdrBtn->setFocusPolicy(Qt::NoFocus);
    sdrBtn->setStyleSheet(
        "QToolButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;"
        " border-radius: 3px; font-size: 11px; padding: 2px 8px; }"
        "QToolButton::menu-indicator { image: none; }");
    auto* sdrMenu = new QMenu(sdrBtn);
    styleMenu(sdrMenu);
    // Radio picker: which driver the console runs. Takes effect on the next
    // launch (the driver, serial framing, and rigctld identity are built at
    // startup). TTC_RADIO env still overrides for testing.
    {
        auto* radioGroup = new QActionGroup(this);
        radioGroup->setExclusive(true);
        auto* rOrion = radioGroup->addAction(sdrMenu->addAction("Radio: Orion III  (565)"));
        auto* rOmni  = radioGroup->addAction(sdrMenu->addAction("Radio: Omni 8  (588)"));
        rOrion->setCheckable(true);
        rOmni->setCheckable(true);
        const bool isOmni = !radio_->caps().dualReceiver;
        (isOmni ? rOmni : rOrion)->setChecked(true);
        connect(rOrion, &QAction::triggered, this, [this] {
            QSettings().setValue("radio/model", "orion");
            statusBar()->showMessage("radio: Orion III — restart the console to switch");
        });
        connect(rOmni, &QAction::triggered, this, [this] {
            QSettings().setValue("radio/model", "omni8");
            statusBar()->showMessage("radio: Omni 8 — restart the console to switch");
        });
        sdrMenu->addSeparator();
    }
    auto* antGroup = new QActionGroup(this);
    antGroup->setExclusive(true);
    auto* antA = antGroup->addAction(sdrMenu->addAction("Antenna A  (Orion)"));
    auto* antB = antGroup->addAction(sdrMenu->addAction("Antenna B  (Omni VII)"));
    antA->setCheckable(true);
    antB->setCheckable(true);
    sdrMenu->addSeparator();
    auto* bcNotch = sdrMenu->addAction("Broadcast (MW) notch");
    bcNotch->setCheckable(true);
    sdrMenu->addSeparator();
    // Band recorder: raw capture to disk so the whole pipeline (skimmer
    // included) can be re-run on a real recording. Park on the band first
    // — the recording stops itself if the dial moves.
    iqRec_ = new IqRecorder(this);
    recIqAct_ = sdrMenu->addAction("Record band IQ (~2 MB/s)");
    recIqAct_->setCheckable(true);
    recIqAct_->setToolTip(
        "Write the raw 500 kHz capture to disk (.tciq). Everything the "
        "panadapter\nsees, replayable through the skimmer/decoder later. "
        "Park on the band first;\ntuning stops the recording.");
    connect(recIqAct_, &QAction::toggled, this, [this](bool on) {
        if (on) {
            const QString path = IqRecorder::defaultDir()
                + QString("/%1-%2kHz.tciq")
                      .arg(QDateTime::currentDateTimeUtc()
                               .toString("yyyyMMdd-hhmmss"))
                      .arg(centerHz_ / 1000);
            if (iqRec_->start(path, 500000.0,
                              double(centerHz_ + kLoOffsetHz)))
                statusBar()->showMessage("IQ recording -> " + path);
            else {
                statusBar()->showMessage("IQ record: cannot open " + path,
                                         6000);
                const QSignalBlocker b(recIqAct_);
                recIqAct_->setChecked(false);
            }
        } else if (iqRec_->active()) {
            iqRec_->stop();
            statusBar()->showMessage(
                QString("IQ recording saved (%1 MB): %2")
                    .arg(iqRec_->bytesWritten() / 1048576)
                    .arg(iqRec_->path()), 10000);
        }
    });
    connect(iqRec_, &IqRecorder::progress, this,
            [this](qint64 bytes, double secs) {
                static int n = 0;
                if (++n % 25 == 0)         // every ~5 s, not every drain
                    statusBar()->showMessage(
                        QString("IQ recording  %1 s   %2 MB")
                            .arg(int(secs)).arg(bytes / 1048576));
            });
    // Timed unattended capture (RX only): tune + record at a UTC time.
    schedIqAct_ = sdrMenu->addAction("Schedule IQ recording…");
    schedIqAct_->setToolTip(
        "Arm a one-shot recording: at the set UTC time the console tunes\n"
        "to the chosen dial and records band IQ for the duration (W1AW\n"
        "code practice, overnight opening studies). Select again to cancel.");
    connect(schedIqAct_, &QAction::triggered, this,
            &MainWindow::scheduleIqRecordingDialog);
    sdrMenu->addSeparator();

    // Front-end gain, live-adjustable: too much gain = ADC overload warnings
    // and a taller DC spike; too little buries weak signals. IF slider shows
    // gain (right = hotter, mapped to the API's 59..20 dB reduction); LNA is
    // the coarse attenuator (0 = max gain .. 8 = max attenuation).
    auto* gainW = new QWidget(sdrMenu);
    gainW->setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 13px; }"
        "QLabel[cap=\"1\"] { color: #8fa3b8; font-size: 12px; font-weight: bold; }"
        "QSlider::groove:horizontal { height: 6px; background: #2a3644; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;"
        " background: #6aa5d8; }"
        "QComboBox { background: #1c2430; border: 1px solid #2a3644; border-radius: 3px;"
        " padding: 3px 8px; }"
        "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
        " selection-background-color: #2a3644; }");
    auto* gg = new QGridLayout(gainW);
    gg->setContentsMargins(14, 10, 14, 10);
    auto* ifCap = new QLabel("IF GAIN", gainW);
    ifCap->setProperty("cap", "1");
    gg->addWidget(ifCap, 0, 0);
    auto* ifGain = new QSlider(Qt::Horizontal, gainW);
    ifGain->setRange(20, 59);                       // stored as reduction dB
    ifGain->setInvertedAppearance(true);            // right = more gain
    ifGain->setFixedWidth(170);
    auto* ifVal = new QLabel(gainW);
    ifVal->setFixedWidth(48);
    gg->addWidget(ifGain, 0, 1);
    gg->addWidget(ifVal, 0, 2);
    auto* lnaCap = new QLabel("LNA", gainW);
    lnaCap->setProperty("cap", "1");
    gg->addWidget(lnaCap, 1, 0);
    auto* lna = new QComboBox(gainW);
    for (int i = 0; i <= 8; ++i)
        lna->addItem(QString("%1%2").arg(i).arg(i == 0 ? "  (max gain)"
                                              : i == 8 ? "  (max attn)" : ""));
    gg->addWidget(lna, 1, 1, 1, 2);
    // Auto LNA: the RF AGC that keeps ADC peaks in the sweet spot found
    // by hand (LNA 6 = deaf, LNA 2 = clipping mutants, LNA 3 = three
    // skimmed calls a minute). Touching the LNA box takes manual control
    // back and switches this off.
    auto* autoLnaBox = new QCheckBox("Auto LNA (holds best skimmer level)",
                                     gainW);
    autoLnaBox->setToolTip(
        "Steps the LNA to keep wideband ADC peaks between -16 and -4 dBFS:\n"
        "up fast near clipping, down after 30 s of unused headroom.\n"
        "The S-meter compensates automatically. Manual LNA changes turn "
        "this off.");
    gg->addWidget(autoLnaBox, 2, 0, 1, 3);
    auto* gainAction = new QWidgetAction(sdrMenu);
    gainAction->setDefaultWidget(gainW);
    sdrMenu->addAction(gainAction);

    sdrBtn->setMenu(sdrMenu);
    topLay->addSpacing(8);
    topLay->addWidget(sdrBtn);

    {   // restore persisted RSP2 state before start() applies it
        QSettings s;
        const bool useB = s.value("sdr/antennaB", false).toBool();
        const bool bcn  = s.value("sdr/broadcastNotch", false).toBool();
        const int  gr   = std::clamp(s.value("sdr/gRdB", 40).toInt(), 20, 59);
        const int  ls   = std::clamp(s.value("sdr/lna", 6).toInt(), 0, 8);
        (useB ? antB : antA)->setChecked(true);
        bcNotch->setChecked(bcn);
        sdr_.setAntennaB(useB);
        sdr_.setBroadcastNotch(bcn);
        sdr_.setGain(gr, ls);
        const QSignalBlocker b1(ifGain), b2(lna);
        ifGain->setValue(gr);
        lna->setCurrentIndex(ls);
        ifVal->setText(QString("-%1 dB").arg(gr));
    }
    auto applySdrGain = [this, ifGain, ifVal, lna] {
        const int gr = ifGain->value(), ls = lna->currentIndex();
        ifVal->setText(QString("-%1 dB").arg(gr));
        sdr_.setGainLive(gr, ls);
        QSettings s;
        s.setValue("sdr/gRdB", gr);
        s.setValue("sdr/lna", ls);
        statusBar()->showMessage(QString("SDR gain: IF -%1 dB  LNA state %2").arg(gr).arg(ls));
    };
    connect(ifGain, &QSlider::valueChanged, this, applySdrGain);
    connect(lna, &QComboBox::currentIndexChanged, this, applySdrGain);
    // --- Auto LNA wiring ---
    autoLnaBox->setChecked(QSettings().value("sdr/autoLna", true).toBool());
    autoLna_.reset(lna->currentIndex(), QDateTime::currentMSecsSinceEpoch());
    connect(autoLnaBox, &QCheckBox::toggled, this, [this, lna](bool on) {
        QSettings().setValue("sdr/autoLna", on);
        if (on)
            autoLna_.reset(lna->currentIndex(),
                           QDateTime::currentMSecsSinceEpoch());
        statusBar()->showMessage(on ? "Auto LNA ON" : "Auto LNA off (manual)",
                                 4000);
    });
    connect(lna, &QComboBox::currentIndexChanged, this,
            [this, autoLnaBox](int) {
                // The operator's hand wins: a manual LNA change takes the
                // loop out (re-check the box to hand it back).
                if (autoLnaBox->isChecked()) {
                    const QSignalBlocker b(autoLnaBox);
                    autoLnaBox->setChecked(false);
                    QSettings().setValue("sdr/autoLna", false);
                    statusBar()->showMessage(
                        "Auto LNA off — manual control", 5000);
                }
            });
    auto* agcTick = new QTimer(this);
    agcTick->setInterval(1000);
    connect(agcTick, &QTimer::timeout, this,
            [this, lna, ifGain, autoLnaBox] {
                const float pk = iqPeak_.exchange(0.0f);
                if (pk <= 0.0f || replayThread_) return;   // no live stream
                if (txMonOn_ || radioTx_) return;  // TX peaks aren't band state
                if (!autoLnaBox->isChecked()) return;
                const double dbfs = 20.0 * std::log10(double(pk));
                const auto d = autoLna_.tick(
                    dbfs, QDateTime::currentMSecsSinceEpoch());
                if (!d.step) return;
                sdr_.setGainLive(ifGain->value(), d.lna);
                QSettings().setValue("sdr/lna", d.lna);
                {
                    const QSignalBlocker b(lna);   // don't trip manual-off
                    lna->setCurrentIndex(d.lna);
                }
                statusBar()->showMessage(
                    QString("auto LNA -> %1  (%2, peaks %3 dBFS)")
                        .arg(d.lna).arg(d.why).arg(dbfs, 0, 'f', 1), 6000);
            });
    agcTick->start();
    // TX monitor (Option A): while transmitting, slam the RSP2 to maximum
    // attenuation so the panadapter shows the operator's own signal at the
    // "0.9 W look" instead of ADC-clipped garbage — the tap runs ~20-25 dB
    // past clipping at 100 W. Triggers, OR'd: the radio's TX-format meter
    // reply (paddle/mic PTT, ~a poll cycle), console-keyed PTT (manual
    // tune, DVR — instant), and a BURST of ADC overload events within one
    // 100 ms tick (milliseconds; a burst is required so a single RX
    // overload from a broadcaster stays Auto LNA's business). A 1 s hang
    // keeps the gain from flapping between CW elements and SSB syllables.
    txMonAct_ = sdrMenu->addAction("TX monitor (gain drop on transmit)");
    txMonAct_->setCheckable(true);
    txMonAct_->setChecked(QSettings().value("sdr/txMon", true).toBool());
    txMonAct_->setToolTip(
        "While transmitting, drop the RSP2 gain hard so your own signal\n"
        "shows cleanly on the panadapter (keying envelope, speech width)\n"
        "instead of overload hash. Restores receive gain ~1 s after unkey.");
    sdr_.setTxPanic(txMonAct_->isChecked());
    connect(txMonAct_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("sdr/txMon", on);
        sdr_.setTxPanic(on);
        if (!on && txMonOn_) {
            txMonOn_ = false;
            sdr_.setGainLive(txSaveGr_, txSaveLna_);
        }
    });
    // Hang time before RX gain returns. QSK CW unkeys between elements
    // and the Orion's meter poll flaps TX/RX with it — the SDR must NOT
    // chase that (live-found: gain bouncing during QSK strings). Longer
    // hang = calmer QSK at the cost of a deafer beat after the over.
    txMonHangMs_ = QSettings().value("sdr/txMonHangS", 1).toInt() * 1000;
    auto* hangMenu = sdrMenu->addMenu("TX monitor hang");
    styleMenu(hangMenu);
    auto* hangGrp = new QActionGroup(this);
    for (int secs : {1, 2, 3}) {
        auto* a = hangMenu->addAction(QString("%1 s").arg(secs));
        a->setCheckable(true);
        a->setActionGroup(hangGrp);
        a->setChecked(txMonHangMs_ == secs * 1000);
        connect(a, &QAction::triggered, this, [this, secs] {
            txMonHangMs_ = secs * 1000;
            QSettings().setValue("sdr/txMonHangS", secs);
            statusBar()->showMessage(
                QString("TX monitor hang %1 s").arg(secs), 4000);
        });
    }
    if (!hangGrp->checkedAction())         // stale setting: default 1 s
        hangGrp->actions().first()->setChecked(true);
    auto* txTick = new QTimer(this);
    txTick->setInterval(100);
    connect(txTick, &QTimer::timeout, this, [this, ifGain, lna] {
        if (replayThread_) return;
        const unsigned ov = sdr_.overloadCount();
        const unsigned burst = ov - lastOverloads_;
        lastOverloads_ = ov;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool predicted = now - txPredictMs_ < 1500;
        const bool tx = radioTx_ || tuning_ || dvrTxPlayback_ || predicted
                        || burst >= 3;
        if (burst > 0) panicked_ = true;   // callback may have slammed gain
        if (tx) lastTxMs_ = now;
        if (!txMonAct_->isChecked()) return;
        // A lone RX overload (broadcast peak) also fires the callback
        // panic; if no TX evidence follows within ~300 ms, quietly put the
        // logical receive gains back — a sub-half-second dip instead of a
        // full hang. Real TX gets confirmed and promoted below.
        if (panicked_ && !txMonOn_ && !tx
            && now - sdr_.lastOverloadMs() > 300) {
            panicked_ = false;
            sdr_.setGainLive(sdr_.gainReduction(), sdr_.lnaState());
        }
        if (tx && !txMonOn_) {
            txMonOn_ = true;
            txSaveGr_ = sdr_.gainReduction();
            txSaveLna_ = sdr_.lnaState();
            sdr_.setGainLive(59, 8);
            statusBar()->showMessage("TX monitor: gain dropped");
        } else if (txMonOn_ && now - lastTxMs_ > txMonHangMs_) {
            txMonOn_ = false;
            panicked_ = false;
            sdr_.setGainLive(txSaveGr_, txSaveLna_);
            iqPeak_.exchange(0.0f);        // don't feed TX peaks to Auto LNA
            {
                const QSignalBlocker b(lna);
                lna->setCurrentIndex(txSaveLna_);
            }
            statusBar()->showMessage("TX monitor: receive gain restored",
                                     4000);
        }
    });
    txTick->start();
    connect(antA, &QAction::triggered, this, [this] {
        sdr_.setAntennaB(false);
        QSettings().setValue("sdr/antennaB", false);
        statusBar()->showMessage("RSP2 antenna A (Orion)");
    });
    connect(antB, &QAction::triggered, this, [this] {
        sdr_.setAntennaB(true);
        QSettings().setValue("sdr/antennaB", true);
        statusBar()->showMessage("RSP2 antenna B (Omni VII)");
    });
    connect(bcNotch, &QAction::toggled, this, [this](bool on) {
        sdr_.setBroadcastNotch(on);
        QSettings().setValue("sdr/broadcastNotch", on);
    });
#endif
    left->addWidget(topStrip);
    left->addWidget(pan_, 1);
    txBar_ = new TxBar(this);
    {
        QSettings s;
        txBar_->setAmpMode(s.value("amp/enabled", false).toBool(),
                           s.value("amp/limit", 40).toInt());
    }
    left->addWidget(txBar_);
    lay->addLayout(left, 1);
    lay->addWidget(panel_);
    setCentralWidget(central);

    connect(pan_, &PanadapterWidget::tuneRequested,    this, &MainWindow::onTuneRequested);
    connect(pan_, &PanadapterWidget::passbandEditBegan, this, [this](int lo, int hi) {
        anchorLoHz_ = lo;  anchorHiHz_ = hi;       // where the overlay was grabbed
        anchorBwHz_ = rigBwHz_;                    // radio's real state, from polling
        anchorPbtHz_ = rigPbtHz_;
        sinceFilterEdit_.restart();
    });
    connect(pan_, &PanadapterWidget::passbandChanged,  this, &MainWindow::onPassbandChanged);
    connect(pan_, &PanadapterWidget::viewSpanChanged,  this, [this](int spanHz) {
        statusBar()->showMessage(QString("zoom -> span %1 kHz").arg(spanHz / 1000.0, 0, 'f', 1));
    });
    // Wheel tuning, step auto-set by mode: SSB/AM/FM 100 Hz per notch (10 Hz
    // with Shift), CW 10 Hz (1 Hz with Shift) — matching how tight each mode
    // actually tunes. SAM uses the CW steps for the carrier zero-beat.
    // Ctrl+wheel still zooms; the wheel follows whichever VFO was tuned last
    // (right-click/drag on B hands it to B; any A tune takes it back).
    const auto wheelUnit = [this](bool fine) {
        const bool tight = samActive_
                        || rigMode_ == Mode::CWU || rigMode_ == Mode::CWL;
        return tight ? (fine ? 1 : 10) : (fine ? 10 : 100);
    };
    connect(pan_, &PanadapterWidget::tuneStepRequested, this,
            [this, wheelUnit](int steps, bool fine) {
        // exact: wheel steps are deliberate nudges — never zap-snap them.
        onTuneRequested(steps * wheelUnit(fine), true);
    });
    connect(pan_, &PanadapterWidget::vfoBStepRequested, this,
            [this, wheelUnit](int steps, bool fine) {
        if (vfoLockB_) { statusBar()->showMessage("VFO B is LOCKED"); return; }
        vfoBHz_ = static_cast<uint64_t>(
            static_cast<qint64>(vfoBHz_) + steps * wheelUnit(fine));
        freqDispB_->setFrequency(vfoBHz_);
        sinceVfoBEdit_.restart();
        pushVfoB();
        pendBHz_ = vfoBHz_;                        // coalesce like B drags
        bfDirty_ = true;
        if (!bfTx_->isActive()) bfTx_->start();
        statusBar()->showMessage(QString("VFO B -> %1 MHz%2")
                                     .arg(vfoBHz_ / 1e6, 0, 'f', 6)
                                     .arg(txVfo_ == 'B' ? "  (TX)" : ""));
    });

    // Coalesce drag-to-filter writes: the Orion's UART services on a ~100 ms cycle,
    // so raw mouse-move rates (100+ cmd pairs/sec) would flood it. Send only the
    // latest edges, at most ~25x/sec, trailing edge included.
    filterTx_ = new QTimer(this);
    filterTx_->setSingleShot(true);
    filterTx_->setInterval(40);
    connect(filterTx_, &QTimer::timeout, this, &MainWindow::sendPendingFilter);
    notchTx_ = new QTimer(this);
    notchTx_->setSingleShot(true);
    notchTx_->setInterval(40);
    connect(notchTx_, &QTimer::timeout, this, &MainWindow::sendPendingNotch);

    // Radio state -> mode-sided passband overlay (LSB hangs below the carrier).
    connect(radio_, &RadioController::modeReported, this, [this](Rx rx, Mode m) {
        if (rx == Rx::Sub) { subMode_ = m; return; }
        rigctld_.cacheMode(m);                      // clients always see true mode
        if (samActive_ && m == samEngine_)          // SAM's engine reporting in:
            panel_->clearModeSelection();           // keep SAM as the lit mode
        else
            panel_->showMode(m);                    // sidebar mirrors the front panel
        if (m != rigMode_) {
            rigMode_ = m;
            refreshPassbandOverlay();
            refreshNotchOverlay();                  // marker side flips with sideband
            if (bandStamp_) bandStamp_->start();    // keep the stack register current
            if (samActive_ && m != samEngine_) {    // front panel left SAM's mode
                samActive_ = false;
                panel_->showSam(false);
                panel_->setSamLabel("SAM");
            }
        }
    });

    // Manual notch / SAF: drag the marker to move it, wheel over it for width,
    // sidebar buttons to engage. One DSP engine in the radio — NOTCH rejects
    // the band, SAF peaks it — sharing center/width. Live-verified: *R.NS1
    // engages SAF (NM then reads 1 too), *R.NM1 steals the engine back to
    // notch (NS drops to 0), *R.NS0 shuts the whole engine down.
    connect(pan_, &PanadapterWidget::notchDragged, this, [this](int rfOffsetHz) {
        const int audio = std::clamp(pbtRfSign(rigMode_) * rfOffsetHz, 20, 4000);
        notchCenter_ = audio;
        sinceNotchEdit_.restart();
        syncNotchUi();
        notchDirty_ = true;
        pendNotchHz_ = audio;
        if (!notchTx_->isActive()) notchTx_->start();  // coalesce like the filter drag
    });
    connect(pan_, &PanadapterWidget::notchWidthAdjustRequested, this, [this](int steps) {
        notchWidth_ = std::clamp(notchWidth_ + steps * 10, 10, 300);
        radio_->setNotchWidth(Rx::Main, notchWidth_);
        sinceNotchEdit_.restart();
        syncNotchUi();
        statusBar()->showMessage(QString("%1 width -> %2 Hz")
                                     .arg(safOn_ ? "SAF" : "notch").arg(notchWidth_));
    });
    connect(panel_, &ControlPanel::notchToggled, this, [this](bool on) {
        radio_->setNotchEngaged(Rx::Main, on);
        notchOn_ = on;
        safOn_ = false;                            // NM either steals or stops the engine
        sinceNotchEdit_.restart();
        syncNotchUi();
    });
    connect(panel_, &ControlPanel::safToggled, this, [this](bool on) {
        radio_->setSaf(Rx::Main, on);
        safOn_ = on;
        notchOn_ = on;                             // NS1 -> NM reads 1; NS0 kills both
        sinceNotchEdit_.restart();
        syncNotchUi();
        statusBar()->showMessage(on ? "SAF: peaking the marked band"
                                    : "SAF off");
    });
    connect(panel_, &ControlPanel::hwNbToggled, this,
            [this](bool on) { radio_->setHardwareNb(Rx::Main, on); });
    connect(radio_, &RadioController::notchCenterReported, this, [this](Rx rx, int hz) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        notchCenter_ = hz;
        syncNotchUi();
    });
    connect(radio_, &RadioController::notchWidthReported, this, [this](Rx rx, int hz) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        notchWidth_ = hz;
        syncNotchUi();
    });
    connect(radio_, &RadioController::notchEngagedReported, this, [this](Rx rx, bool on) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        notchOn_ = on;
        if (!on) safOn_ = false;                   // engine off implies SAF off
        syncNotchUi();
    });
    connect(radio_, &RadioController::safReported, this, [this](Rx rx, bool on) {
        if (rx != Rx::Main) return;
        if (sinceNotchEdit_.isValid() && sinceNotchEdit_.elapsed() < 2000) return;
        safOn_ = on;
        if (on) notchOn_ = true;                   // SAF active = engine engaged
        syncNotchUi();
    });
    connect(radio_, &RadioController::hardwareNbReported, this, [this](Rx rx, bool on) {
        if (rx == Rx::Main) panel_->showHwNb(on);
    });

    // Radio state -> control surface (front-panel changes reflect on screen).
    connect(radio_, &RadioController::sMeterReported, this, [this](int mainRaw, int) {
        if (std::getenv("TTC_SELFTEST")) {
            static int n = 0;
            if (++n % 4 == 1) std::fprintf(stderr, "[radio] S-meter main raw %d\n", mainRaw);
        }
        smeter_->setRawLevel(mainRaw);
        lastRadioDbS9_ = SMeterWidget::rawToDbS9(mainRaw);   // for SDR-meter cal
        rigctld_.cachePtt(false);                   // radio answered in receive
    });
    // SDR-source S-meter calibration: pin the SDR's passband-power reading to
    // the radio's meter at a moment the radio is honest (RF gain full up).
    // One offset, stored; the gRdB/LNA compensation keeps it valid across
    // every gain-slider move afterward.
    sdrCalDb_ = QSettings().value("sdr/smeterCalDb", 0.0).toDouble();
    connect(smeter_, &SMeterWidget::calibrateRequested, this, [this] {
        if (std::isnan(lastRadioDbS9_) || std::isnan(lastSdrMeasDb_)) {
            statusBar()->showMessage(
                "S-meter cal needs a radio reading and a running SDR — "
                "connect both, then try again", 6000);
            return;
        }
        sdrCalDb_ = lastRadioDbS9_ - lastSdrMeasDb_;
        QSettings().setValue("sdr/smeterCalDb", sdrCalDb_);
        smeter_->useSdrSource();          // calibrating implies wanting it
        statusBar()->showMessage(
            QString("S-meter calibrated and switched to SDR source "
                    "(offset %1 dB) — RF gain can go back down")
                .arg(sdrCalDb_, 0, 'f', 1), 8000);
    });
    // TX monitor state: the radio answers the meter poll in TX format
    // only while keyed — the cleanest transmit indicator we have.
    connect(radio_, &RadioController::txMeterReported, this,
            [this](double, double, double) { radioTx_ = true; });
    connect(radio_, &RadioController::sMeterReported, this,
            [this](int, int) { radioTx_ = false; });
    connect(radio_, &RadioController::txMeterReported, this,
            [this](double fwd, double ref, double swr) {
                smeter_->setTxLevel(fwd, ref, swr);
                rigctld_.cachePtt(true);            // radio is provably keyed
                // Amp protection, fast path: actual forward power well above
                // the drive limit while keyed -> command the power back down.
                if (txBar_->ampMode() && fwd > txBar_->ampLimit() * 1.15
                    && (!sinceEnforce_.isValid() || sinceEnforce_.elapsed() > 1000)) {
                    sinceEnforce_.restart();
                    radio_->setTxPower(txBar_->ampLimit());
                    txBar_->showTxPower(txBar_->ampLimit());
                    statusBar()->showMessage(
                        QString("AMP LIMIT: %1 W measured, drive forced to %2")
                            .arg(fwd, 0, 'f', 0).arg(txBar_->ampLimit()));
                }
            });

    // TX bar <-> radio.
    connect(txBar_, &TxBar::txPowerChanged, this,
            [this](int p) { radio_->setTxPower(p); });
    connect(txBar_, &TxBar::micGainChanged, this,
            [this](int p) { radio_->setMicGain(p); });
    connect(audioPanel_, &AudioPanel::monitorChanged, this,
            [this](int p) { radio_->setMonitor(p); });
    connect(txBar_, &TxBar::txFilterChanged, this, [this](int hz) {
        txBwHz_ = hz;
        radio_->setTxFilter(hz);
    });
    connect(txBar_, &TxBar::speechProcChanged, this, [this](int l) {
        if (!digital_) lastSpeechProc_ = l;
        radio_->setSpeechProc(l);
    });
    connect(txBar_, &TxBar::profileRecalled, this, &MainWindow::applyTxProfile);
    connect(txBar_, &TxBar::profileSaveRequested, this, &MainWindow::saveTxProfile);
    connect(txBar_, &TxBar::tunerEnableToggled, this, [this](bool on) {
        tunerOn_ = on;
        radio_->setTunerEnabled(on);
    });
    tuneTimeout_ = new QTimer(this);
    tuneTimeout_->setSingleShot(true);
    tuneTimeout_->setInterval(15000);              // carrier never outlives 15 s
    connect(tuneTimeout_, &QTimer::timeout, this, &MainWindow::stopManualTune);
    connect(txBar_, &TxBar::tuneToggled, this, [this](bool on) {
        if (!on) { stopManualTune(); return; }
        if (tunerOn_) {
            // Internal tuner enabled: the radio runs its own cycle (*TTT
            // generates its own carrier and drops it when matched).
            radio_->startTune();
            statusBar()->showMessage("internal tuner: tune cycle started");
            txBar_->showTuneActive(false);         // momentary in this mode
            return;
        }
        startManualTune();
    });
    connect(txBar_, &TxBar::tuneLevelChanged, this, [](int w) {
        QSettings().setValue("tune/power", w);
    });
    txBar_->setTuneLevel(QSettings().value("tune/power", 20).toInt());

    // DVR + voice keyer. Audio rides the same SignaLink (USB codec) path
    // fldigi uses: off-air takes are captured from its input (the radio's
    // line out), VK/retransmit playback goes out its output into the radio
    // with CAT PTT held — the SignaLink's own VOX keys as backup, so this
    // works on both radios even where CAT TX audio config doesn't.
    dvr_ = new ClipDeck(this);
    {
        QSettings s;
        const QString radioMatch =
            s.value("dvr/radioAudioMatch", "USB_AUDIO_CODEC").toString();
        radioSink_   = ClipDeck::findSink(radioMatch);
        radioSource_ = ClipDeck::findSource(radioMatch);
        micSource_   = s.value("dvr/micSource").toString();
        // No mic configured: never fall back to the system default source —
        // on a SignaLink station that IS the radio codec, and the voice keyer
        // would record the receiver instead of the operator.
        if (micSource_.isEmpty())
            micSource_ = ClipDeck::findSourceExcluding(radioMatch);
    }
    QDir().mkpath(dvrDir());
    for (int i = 0; i < 4; ++i)
        txBar_->setVkLoaded(i, QFileInfo::exists(vkPath(i)));
    // Any DVR click while the deck runs means "stop that" — the finished()
    // handler clears the lights and drops PTT, so just stop and swallow. A
    // keyed-but-not-yet-playing state is the arming window (line-in switch
    // settling): unwind it directly, the deck has nothing to stop yet.
    const auto dvrBusy = [this] {
        if (dvr_->state() != ClipDeck::State::Idle) {
            dvr_->stop();
            return true;
        }
        if (dvrTxPlayback_) {
            dvrStopped();
            return true;
        }
        return false;
    };
    connect(txBar_, &TxBar::dvrRecordClicked, this, [this, dvrBusy] {
        if (dvrBusy()) return;
        if (radioSource_.isEmpty()) {
            statusBar()->showMessage("DVR: radio sound device (SignaLink) not found");
            return;
        }
        dvrLastRx_ = dvrDir() + QString("/rx-%1.wav").arg(
            QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        if (dvr_->record(dvrLastRx_, radioSource_)) {
            dvrJustRecorded_ = dvrLastRx_;
            txBar_->showDvrRecording(-1);
            statusBar()->showMessage("DVR: recording off air -> "
                + QFileInfo(dvrLastRx_).fileName() + "   (REC again to stop)");
        }
    });
    connect(txBar_, &TxBar::dvrPlayClicked, this, [this, dvrBusy](bool overAir) {
        if (dvrBusy()) return;
        QString f = dvrLastRx_;
        if (f.isEmpty()) {                         // newest take from an earlier run
            const QFileInfoList takes = QDir(dvrDir()).entryInfoList(
                {"rx-*.wav"}, QDir::Files, QDir::Name);
            if (!takes.isEmpty()) f = takes.last().filePath();
        }
        if (f.isEmpty() || !QFileInfo::exists(f)) {
            statusBar()->showMessage("DVR: nothing recorded yet");
            return;
        }
        if (overAir && radioSink_.isEmpty()) {
            statusBar()->showMessage("DVR: radio sound device (SignaLink) not found");
            return;
        }
        if (overAir) {
            dvrPlayOverAir(f, -1);
        } else if (dvr_->play(f, QString())) {
            txBar_->showDvrPlaying(-1);
            statusBar()->showMessage(
                QString("DVR: playing %1 on the speakers  (right-click PLAY "
                        "sends it over the air)").arg(QFileInfo(f).fileName()));
        }
    });
    connect(txBar_, &TxBar::vkClicked, this, [this, dvrBusy](int slot) {
        if (dvrBusy()) return;
        const QString f = vkPath(slot);
        if (!QFileInfo::exists(f)) {
            statusBar()->showMessage(QString(
                "VK%1 is empty — right-click it to record a message").arg(slot + 1));
            return;
        }
        if (radioSink_.isEmpty()) {
            statusBar()->showMessage("DVR: radio sound device (SignaLink) not found");
            return;
        }
        dvrPlayOverAir(f, slot);
        statusBar()->showMessage(QString(
            "VK%1 on the air — click it again to abort").arg(slot + 1));
    });
    connect(txBar_, &TxBar::vkRecordClicked, this, [this, dvrBusy](int slot) {
        if (dvrBusy()) return;
        if (dvr_->record(vkPath(slot), micSource_)) {
            dvrJustRecorded_ = vkPath(slot);
            txBar_->showDvrRecording(slot);
            statusBar()->showMessage(QString(
                "VK%1: recording from the mic — click it to stop").arg(slot + 1));
        }
    });
    connect(dvr_, &ClipDeck::finished, this, &MainWindow::dvrStopped);
    connect(dvr_, &ClipDeck::failed, this, [this](const QString& why) {
        statusBar()->showMessage("DVR: " + why);
    });

    // Digital/voice audio switch: line-in for digital, mic for voice.
    {
        QSettings s;
        lastMicGain_    = s.value("audio/voiceMic", 51).toInt();
        lastSpeechProc_ = s.value("audio/voiceSpeech", 2).toInt();
    }
    connect(panel_, &ControlPanel::digitalToggled, this,
            [this](bool on) { setDigitalMode(on); });
    connect(txBar_, &TxBar::ampModeChanged, this, [this](bool on, int limit) {
        QSettings s;
        s.setValue("amp/enabled", on);
        s.setValue("amp/limit", limit);
        if (on) radio_->queryTxPower();              // check the current drive now
        statusBar()->showMessage(on ? QString("amp mode: drive capped at %1 W").arg(limit)
                                    : "amp mode off");
    });
    connect(radio_, &RadioController::txPowerReported, this, [this](int p) {
        lastTxPwr_ = p;                             // for restore after manual tune
        if (tuning_) return;                        // don't fight the tune carrier
        // Amp protection, slow path: the periodic ?TP poll catches a front-
        // panel PWR change above the limit even when not transmitting.
        if (txBar_->ampMode() && p > txBar_->ampLimit()) {
            if (!sinceEnforce_.isValid() || sinceEnforce_.elapsed() > 1000) {
                sinceEnforce_.restart();
                radio_->setTxPower(txBar_->ampLimit());
                statusBar()->showMessage(
                    QString("AMP LIMIT: drive %1 forced back to %2 W")
                        .arg(p).arg(txBar_->ampLimit()));
            }
            txBar_->showTxPower(txBar_->ampLimit());
            return;
        }
        txBar_->showTxPower(p);
    });
    connect(radio_, &RadioController::micGainReported, this, [this](int p) {
        txBar_->showMicGain(p);
        micNow_ = p;
        // Learn the voice mic setting — but never learn 0: mic 0 is the
        // digital line-in switch, and adopting it (e.g. console started
        // while the radio was still in a digital session) poisons the
        // restore so DIG-off "resets" to zero.
        if (!digital_ && p > 0) lastMicGain_ = p;
    });
    connect(radio_, &RadioController::speechProcReported, this, [this](int l) {
        // Proc 0 is a legitimate voice setting, so gate the learn on the
        // radio actually being in voice (mic up) — otherwise a leftover
        // digital state (mic 0 + proc 0) records proc "off" as the setup.
        if (!digital_ && micNow_ > 0) lastSpeechProc_ = l;
        txBar_->showSpeechProc(l);
    });
    connect(radio_, &RadioController::txFilterReported, this, [this](int hz) {
        txBwHz_ = hz;
        txBar_->showTxFilter(hz);
    });
    connect(radio_, &RadioController::monitorReported, this,
            [this](int p) { audioPanel_->showMonitor(p); });
    connect(radio_, &RadioController::afVolumeReported, this, [this](Rx rx, int p) {
        const int i = rx == Rx::Main ? 0 : 1;
        if (muted_[i]) return;                     // muted: 0 expected, keep slider
        vol_[i] = p;
        const QSignalBlocker b(volSl_[i]);
        volSl_[i]->setValue(p);
        volLbl_[i]->setText(QString::number(p));
    });
    connect(radio_, &RadioController::tunerReported, this, [this](bool on) {
        tunerOn_ = on;
        txBar_->showTuner(on);
    });
    connect(radio_, &RadioController::vfoLockReported, this, [this](char vfo, bool locked) {
        const int i = vfo == 'B' ? 1 : 0;
        (i == 0 ? vfoLockA_ : vfoLockB_) = locked;
        const QSignalBlocker b(lockBtn_[i]);        // reflect, don't re-send
        lockBtn_[i]->setChecked(locked);
        pan_->setVfoLocks(vfoLockA_, vfoLockB_);
    });

    // rigctld PTT (WSJT-X / fldigi keying through :4532). The 't' reply
    // tracks the radio's true T/R state via the metering replies.
    connect(&rigctld_, &RigctldServer::pttRequested, this,
            [this](bool on) { radio_->setPtt(on); });
    connect(radio_, &RadioController::agcReported, this, [this](Rx rx, char a) {
        if (rx == Rx::Main) panel_->showAgc(a);
    });
    connect(radio_, &RadioController::rfGainReported, this, [this](Rx rx, int g) {
        if (rx == Rx::Main) panel_->showRfGain(g);
    });
    connect(radio_, &RadioController::attenReported, this, [this](Rx rx, int s) {
        if (rx == Rx::Main) panel_->showAtten(s);
    });
    connect(radio_, &RadioController::preampReported, this, [this](Rx rx, bool on) {
        if (rx == Rx::Main) panel_->showPreamp(on);
    });
    connect(radio_, &RadioController::nrReported, this, [this](Rx rx, int l) {
        if (rx == Rx::Main) panel_->showNr(l);
    });
    connect(radio_, &RadioController::nbReported, this, [this](Rx rx, int l) {
        if (rx == Rx::Main) panel_->showNb(l);
    });
    connect(radio_, &RadioController::autoNotchReported, this, [this](Rx rx, int l) {
        if (rx == Rx::Main) panel_->showAutoNotch(l);
    });

    // Control surface -> radio. Sets are fire-and-forget; polling confirms.
    connect(panel_, &ControlPanel::modeSelected, this, [this](Mode m) {
        if (samActive_) {                          // picking a real mode ends SAM
            samActive_ = false;
            panel_->showSam(false);
            panel_->setSamLabel("SAM");
        }
        applyMode(m);
        panel_->showMode(m);
    });

    // SAM (ECSS): the Orion has no true sync-AM, so receive the
    // AM signal in USB with the carrier zero-beaten instead. Engaging swaps
    // to USB + a wide filter; the panadapter wheel drops to 10 Hz (1 Hz with
    // Shift) for the careful zero-beat. Off restores the previous mode/filter.
    // Click cycle: off -> SAM-U -> SAM-L -> off. The sideband flip is the
    // real sync-AM listener's move — pick whichever side dodges the QRM.
    const auto samFilter = [this] {
        QTimer::singleShot(450, this, [this] {     // after the mode switch settles
            radio_->setBandwidthHz(Rx::Main, 4000); // AM-width audio
            radio_->setPbtHz(Rx::Main, 0);
        });
    };
    connect(panel_, &ControlPanel::samToggled, this, [this, samFilter](bool on) {
        if (on) {                                  // engage, upper sideband first
            samActive_ = true;
            samEngine_ = Mode::USB;
            preSamMode_ = rigMode_;
            preSamBw_   = rigBwHz_;
            applyMode(Mode::USB);                  // the ECSS engine, not the face
            panel_->clearModeSelection();          // SAM alone stays lit
            panel_->setSamLabel("SAM-U");
            samFilter();
            statusBar()->showMessage(
                "SAM (upper): zero-beat the carrier — wheel 10 Hz, Shift 1 Hz");
        } else if (samActive_ && samEngine_ == Mode::USB) {
            samEngine_ = Mode::LSB;                // second click: flip sideband
            panel_->showSam(true);                 // stay lit
            panel_->setSamLabel("SAM-L");
            applyMode(Mode::LSB);
            panel_->clearModeSelection();
            samFilter();                           // mode change recalls per-mode filter
            statusBar()->showMessage("SAM (lower sideband)");
        } else {                                   // third click: off
            samActive_ = false;
            panel_->setSamLabel("SAM");
            applyMode(preSamMode_);
            panel_->showMode(preSamMode_);
            QTimer::singleShot(450, this, [this] {
                radio_->setBandwidthHz(Rx::Main, preSamBw_);
            });
            statusBar()->showMessage("SAM off — previous mode restored");
        }
    });

    // Band buttons with Orion-style stack registers: a fresh band recalls its
    // last-used register; clicking the active band again cycles A->B->C->D.
    connect(panel_, &ControlPanel::bandSelected, this, [this](int idx) {
        if (idx < 0 || idx >= kBandCount) return;
        if (is60m(idx)) {
            // Channelized + locked: cycle CH1..CH5 from the hard-coded table.
            // Only "which channel was last used" persists, never the values.
            const int ch = (idx == lastBandPress_)
                ? (curReg_ + 1) % kChan60Count
                : std::clamp(QSettings().value("band/60/chan", 2).toInt(),
                             0, kChan60Count - 1);
            saveBandMemory();                       // stash the outgoing band
            recall60m(ch);
            lastBandPress_ = idx;
            return;
        }
        QSettings s;
        int reg;
        // Cycle A->B->C->D on repeated presses of the SAME button. Keyed off the
        // last button pressed (syncBandRegister clears it when the dial moves to
        // another band under us, so a stale press can't trigger a bogus cycle).
        if (idx == lastBandPress_) {
            reg = (curReg_ + 1) % kStackCount;      // same button again: next reg
        } else {
            reg = s.value(QString("band/%1/reg").arg(kBands[idx].label), 0).toInt();
            reg = std::clamp(reg, 0, kStackCount - 1);
        }
        saveBandMemory();                           // stash the outgoing register
        recallStack(idx, reg);
        lastBandPress_ = idx;                       // after recall (sync resets it)
    });

    // The Orion's CAT set has no way to read its internal band-stack registers,
    // so the console mirrors them instead: shortly after ANY dial/mode/filter
    // change settles (front-panel knob or band button, WSJT-X, click-to-tune),
    // stamp the state into the active register for that band. Our band buttons
    // then return to the same spot the radio's own band button would.
    bandStamp_ = new QTimer(this);
    bandStamp_->setSingleShot(true);
    bandStamp_->setInterval(1500);
    connect(bandStamp_, &QTimer::timeout, this, &MainWindow::saveBandMemory);

    // Frequency readout edits (digit wheel/click or type-in) tune the radio.
    connect(freqDisp_, &FrequencyDisplay::frequencyEdited, this,
            [this](uint64_t hz) { tuneAbsolute(hz); });
    // VFO B goes straight to the sub receiver's dial (*BF); the panadapter
    // stays on VFO A, whose IQ tap feeds it.
    connect(freqDispB_, &FrequencyDisplay::frequencyEdited, this,
            [this](uint64_t hz) {
                radio_->setFrequencyHz(Rx::Sub, hz);
                vfoBHz_ = hz;
                pushVfoB();
                sinceVfoBEdit_.restart();           // poll echo may still be stale
                statusBar()->showMessage(
                    QString("VFO B -> %1 MHz").arg(hz / 1e6, 0, 'f', 6));
            });
    pushVfoB();                                     // marker live from the start

    // Routing matrices + A/B dial transfers (the Orion front panel's VFO and
    // antenna button groups, driven over *KV / *KA).
    connect(routing_, &RoutingPanel::vfoAssignmentEdited, this,
            [this, applyVfoColors](char m, char s, char t) {
                radio_->setVfoAssignment(m, s, t);
                // One-click split setup (no separate split button): the moment a
                // click puts VFO B on TX or RX, park B above the dial — 5 kHz
                // in voice, 1 kHz in CW. The radio's own menu copies the mode
                // to the sub, so only the dial needs setting.
                const bool bEngaged = (t == 'B' && txVfo_ != 'B')
                                   || (m == 'B' && rxVfo_ != 'B');
                txVfo_ = t;
                rxVfo_ = m;
                if (bEngaged && !vfoLockB_) {       // locked B: engage without the park
                    const bool cw = rigMode_ == Mode::CWU || rigMode_ == Mode::CWL;
                    const uint64_t b = centerHz_ + (cw ? 1000 : 5000);
                    radio_->setFrequencyHz(Rx::Sub, b);
                    vfoBHz_ = b;
                    freqDispB_->setFrequency(b);
                    sinceVfoBEdit_.restart();
                }
                applyVfoColors();
                pushVfoB();
                statusBar()->showMessage(
                    QString("VFO routing: RX %1  SUB %2  TX %3%4")
                        .arg(m).arg(s).arg(t)
                        .arg(m != t ? "  (SPLIT)" : ""));
            });
    connect(routing_, &RoutingPanel::antennaRoutingEdited, this,
            [this](char a1, char a2, char rxa) {
                radio_->setAntennaRouting(a1, a2, rxa);
                statusBar()->showMessage(QString("antenna routing: ANT1=%1 ANT2=%2 RX=%3")
                                             .arg(a1).arg(a2).arg(rxa));
            });
    // Dial transfers carry MODE with the frequency (PowerSDR semantics: the
    // VFO is freq+mode). Mode commands trail the freq by 120+ ms — the Orion
    // drops commands that arrive while it's busy with a mode switch.
    connect(routing_, &RoutingPanel::copyABRequested, this, [this] {
        if (vfoLockB_) { statusBar()->showMessage("A>B blocked: VFO B is LOCKED"); return; }
        radio_->setFrequencyHz(Rx::Sub, centerHz_);
        vfoBHz_ = centerHz_;
        pushVfoB();
        freqDispB_->setFrequency(centerHz_);
        sinceVfoBEdit_.restart();
        // Sub mode: handled by the radio itself (menu set to copy the mode;
        // *RSM is dead on v3 firmware anyway).
        statusBar()->showMessage(QString("A>B: VFO B -> %1 MHz")
                                     .arg(centerHz_ / 1e6, 0, 'f', 6));
    });
    connect(routing_, &RoutingPanel::copyBARequested, this, [this] {
        if (vfoLockA_) { statusBar()->showMessage("B>A blocked: VFO A is LOCKED"); return; }
        tuneAbsolute(vfoBHz_);
        QTimer::singleShot(120, this, [this, m = subMode_] {
            applyMode(m);
            panel_->showMode(m);
        });
        statusBar()->showMessage(QString("B>A: VFO A -> %1 MHz")
                                     .arg(vfoBHz_ / 1e6, 0, 'f', 6));
    });
    connect(routing_, &RoutingPanel::swapABRequested, this, [this] {
        if (vfoLockA_ || vfoLockB_) {
            statusBar()->showMessage(QString("A<>B blocked: VFO %1 is LOCKED")
                                         .arg(vfoLockA_ ? 'A' : 'B'));
            return;
        }
        const uint64_t a = centerHz_, b = vfoBHz_;
        const Mode ma = rigMode_, mb = subMode_;
        radio_->setFrequencyHz(Rx::Sub, a);
        vfoBHz_ = a;
        pushVfoB();
        freqDispB_->setFrequency(a);
        sinceVfoBEdit_.restart();
        tuneAbsolute(b);
        if (mb != ma) {                              // bring B's mode to the main RX
            QTimer::singleShot(120, this, [this, mb] {
                applyMode(mb);
                panel_->showMode(mb);
            });
        }
        statusBar()->showMessage(QString("A<>B: %1 <-> %2 MHz")
                                     .arg(b / 1e6, 0, 'f', 6).arg(a / 1e6, 0, 'f', 6));
    });
    connect(radio_, &RadioController::vfoAssignmentReported, this,
            [this, applyVfoColors](char m, char s, char t) {
                routing_->showVfoAssignment(m, s, t);
                txVfo_ = t;
                rxVfo_ = m;
                applyVfoColors();
                pushVfoB();
            });
    connect(radio_, &RadioController::antennaRoutingReported, this,
            [this](char a1, char a2, char rxa) { routing_->showAntennaRouting(a1, a2, rxa); });
    if (std::getenv("TTC_TEST_XFER"))               // headless transfer exercise
        QTimer::singleShot(3000, routing_, &RoutingPanel::swapABRequested);
    connect(panel_, &ControlPanel::agcSelected, this,
            [this](char a) { radio_->setAgc(Rx::Main, a); });
    connect(panel_, &ControlPanel::agcThresholdChanged, this, [this](int uv) {
        radio_->setAgcThreshold(Rx::Main, uv);
        statusBar()->showMessage(QString("AGC threshold -> %1 µV").arg(uv));
        // Read back shortly: the radio quantizes, show its real value.
        QTimer::singleShot(300, this, [this] { radio_->queryAgcThreshold(Rx::Main); });
    });
    connect(radio_, &RadioController::agcThresholdReported, this,
            [this](Rx rx, double uv) {
                if (rx == Rx::Main) panel_->showAgcThreshold(uv);
            });
    connect(panel_, &ControlPanel::agcHangChanged, this, [this](int tenths) {
        radio_->setAgcHang(Rx::Main, tenths / 10.0);
        statusBar()->showMessage(tenths == 0
                                     ? QString("AGC hang -> off")
                                     : QString("AGC hang -> %1 s").arg(tenths / 10.0, 0, 'f', 1));
        QTimer::singleShot(300, this, [this] { radio_->queryAgcHang(Rx::Main); });
    });
    connect(radio_, &RadioController::agcHangReported, this,
            [this](Rx rx, double sec) {
                if (rx == Rx::Main) panel_->showAgcHang(sec);
            });
    connect(panel_, &ControlPanel::agcDecayChanged, this, [this](int rate) {
        radio_->setAgcDecay(Rx::Main, rate);
        statusBar()->showMessage(QString("AGC decay -> %1").arg(rate));
        QTimer::singleShot(300, this, [this] { radio_->queryAgcDecay(Rx::Main); });
    });
    connect(radio_, &RadioController::agcDecayReported, this,
            [this](Rx rx, int rate) {
                if (rx == Rx::Main) panel_->showAgcDecay(rate);
            });
    connect(panel_, &ControlPanel::attenSelected, this,
            [this](int s) { radio_->setAttenuator(Rx::Main, s); });
    connect(panel_, &ControlPanel::preampToggled, this,
            [this](bool on) { radio_->setPreamp(Rx::Main, on); });
    connect(panel_, &ControlPanel::rfGainChanged, this,
            [this](int g) { radio_->setRfGain(Rx::Main, g); });
    connect(panel_, &ControlPanel::nrChanged, this,
            [this](int v) { radio_->setNoiseReduction(Rx::Main, v); });
    connect(panel_, &ControlPanel::nbChanged, this,
            [this](int v) { radio_->setNoiseBlanker(Rx::Main, v); });
    connect(panel_, &ControlPanel::autoNotchChanged, this,
            [this](int v) { radio_->setAutoNotch(Rx::Main, v); });
    connect(radio_, &RadioController::bandwidthReported, this, [this](Rx rx, int bw) {
        if (rx == Rx::Sub) {
            if (sinceSubFilterEdit_.isValid()
                && sinceSubFilterEdit_.elapsed() < 2000) return;  // mid/just-dragged
            if (bw != subBwHz_) { subBwHz_ = bw; pushVfoB(); }
            return;
        }
        rigctld_.cacheBandwidth(bw);
        if (bw != rigBwHz_) { rigBwHz_ = bw; refreshPassbandOverlay(); bandStamp_->start(); }
    });
    connect(radio_, &RadioController::pbtReported, this, [this](Rx rx, int pbt) {
        if (rx == Rx::Sub) {
            if (sinceSubFilterEdit_.isValid()
                && sinceSubFilterEdit_.elapsed() < 2000) return;  // mid/just-dragged
            if (pbt != subPbtHz_) { subPbtHz_ = pbt; pushVfoB(); }
            return;
        }
        panel_->showPbt(pbt);
        if (pbt != rigPbtHz_) { rigPbtHz_ = pbt; refreshPassbandOverlay(); bandStamp_->start(); }
    });

    // Slide VFO B on the panadapter (grab its tint/line, or Shift+click to
    // drop it on a station) — the pileup "get my TX dead on the spot" moves.
    // Drags stream *BF coalesced to ~25 writes/sec like the filter drags.
    bfTx_ = new QTimer(this);
    bfTx_->setInterval(40);
    connect(bfTx_, &QTimer::timeout, this, [this] {
        if (!bfDirty_) { bfTx_->stop(); return; }
        bfDirty_ = false;
        radio_->setFrequencyHz(Rx::Sub, pendBHz_);
    });
    connect(pan_, &PanadapterWidget::vfoBDragged, this, [this](int off) {
        const uint64_t hz =
            static_cast<uint64_t>(static_cast<qint64>(centerHz_) + off);
        vfoBHz_ = hz;
        freqDispB_->setFrequency(hz);
        sinceVfoBEdit_.restart();
        pendBHz_ = hz;
        bfDirty_ = true;
        if (!bfTx_->isActive()) bfTx_->start();
        statusBar()->showMessage(QString("VFO B -> %1 MHz%2")
                                     .arg(hz / 1e6, 0, 'f', 6)
                                     .arg(txVfo_ == 'B' ? "  (TX)" : ""));
    });
    // Drag the A dial line: stream full retunes (radio + SDR + view recenter),
    // coalesced a bit slower than B since each one moves the whole display.
    afTx_ = new QTimer(this);
    afTx_->setInterval(60);
    connect(afTx_, &QTimer::timeout, this, [this] {
        if (!afDirty_) { afTx_->stop(); return; }
        afDirty_ = false;
        tuneAbsolute(pendAHz_);
    });
    connect(pan_, &PanadapterWidget::vfoADragged, this, [this](uint64_t hz) {
        pendAHz_ = hz;
        afDirty_ = true;
        if (!afTx_->isActive()) afTx_->start();
        statusBar()->showMessage(QString("VFO A -> %1 MHz").arg(hz / 1e6, 0, 'f', 6));
    });
    // B filter edge drags -> sub RX filter (*RSF/*RSP), decomposed with the
    // same exact edgesFromRig inverse as A (the sub runs the main's mode).
    // Only useful when the Orion's sub-filter tracking of the main is off —
    // with sync on, the radio re-slaves the sub and the drag won't stick.
    sfTx_ = new QTimer(this);
    sfTx_->setInterval(40);
    connect(sfTx_, &QTimer::timeout, this, [this] {
        if (!subFilterDirty_) { sfTx_->stop(); return; }
        subFilterDirty_ = false;
        radio_->setBandwidthHz(Rx::Sub, pendSubBw_);
        radio_->setPbtHz(Rx::Sub, pendSubPbt_);
        subBwHz_  = pendSubBw_;                     // optimistic; poll confirms
        subPbtHz_ = pendSubPbt_;
    });
    connect(pan_, &PanadapterWidget::vfoBEditBegan, this, [this](int lo, int hi) {
        anchorSubLoHz_ = lo;   anchorSubHiHz_ = hi;
        anchorSubBwHz_ = subBwHz_;  anchorSubPbtHz_ = subPbtHz_;
    });
    connect(pan_, &PanadapterWidget::vfoBPassbandChanged, this, [this](int lo, int hi) {
        int dWidth = (hi - lo) - (anchorSubHiHz_ - anchorSubLoHz_);
        if (rigMode_ == Mode::AM) dWidth /= 2;      // drawn = 2x indicated in AM
        int dPbt;
        switch (rigMode_) {
            case Mode::USB: dPbt = lo - anchorSubLoHz_;  break;
            case Mode::LSB: dPbt = anchorSubHiHz_ - hi;  break;
            default:
                dPbt = pbtRfSign(rigMode_)
                       * (((hi + lo) - (anchorSubHiHz_ + anchorSubLoHz_)) / 2);
                break;
        }
        pendSubBw_  = std::clamp(anchorSubBwHz_ + dWidth, 100, bwMaxFor(rigMode_));
        pendSubPbt_ = std::clamp(anchorSubPbtHz_ + dPbt, -8000, 8000);
        subFilterDirty_ = true;
        sinceSubFilterEdit_.restart();
        if (!sfTx_->isActive()) sfTx_->start();
        statusBar()->showMessage(QString("B filter -> bw %1 Hz  pbt %2 Hz")
                                     .arg(pendSubBw_).arg(pendSubPbt_));
    });
    connect(pan_, &PanadapterWidget::vfoBTuneRequested, this, [this](int off) {
        if (vfoLockB_) { statusBar()->showMessage("VFO B is LOCKED"); return; }
        if (!bViewBtn_->isChecked()) bViewBtn_->setChecked(true);  // deliberate B use
        const uint64_t hz =
            static_cast<uint64_t>(static_cast<qint64>(centerHz_) + off);
        radio_->setFrequencyHz(Rx::Sub, hz);
        vfoBHz_ = hz;
        freqDispB_->setFrequency(hz);
        sinceVfoBEdit_.restart();
        pushVfoB();
        statusBar()->showMessage(QString("VFO B -> %1 MHz%2")
                                     .arg(hz / 1e6, 0, 'f', 6)
                                     .arg(txVfo_ == 'B' ? "  (TX)" : ""));
    });

    // PBT back to the detent: sidebar button or double-click a passband edge.
    // Bandwidth and dial untouched — the filter just slides back to center.
    const auto zeroPbt = [this] {
        filterTx_->stop();                          // drop any stale pending PBT
        filterDirty_ = false;
        radio_->setPbtHz(Rx::Main, 0);
        rigPbtHz_ = 0;
        panel_->showPbt(0);
        sinceFilterEdit_.invalidate();              // explicit action: refresh now
        refreshPassbandOverlay();
        statusBar()->showMessage("PBT -> 0");
    };
    connect(panel_, &ControlPanel::pbtZeroRequested, this, zeroPbt);
    connect(pan_, &PanadapterWidget::pbtZeroRequested, this, zeroPbt);
    // Same detent snap for VFO B (double-click a B filter edge): the sub
    // PBT drifts just like the main's when working its filter edges.
    connect(pan_, &PanadapterWidget::vfoBPbtZeroRequested, this, [this] {
        sfTx_->stop();                              // drop any stale pending PBT
        subFilterDirty_ = false;
        radio_->setPbtHz(Rx::Sub, 0);
        subPbtHz_ = 0;
        sinceSubFilterEdit_.invalidate();           // explicit action: redraw now
        pushVfoB();
        statusBar()->showMessage("VFO B PBT -> 0");
    });
    // Sidebar PBT slider: deliberate shifts (already coalesced in the panel).
    connect(panel_, &ControlPanel::pbtChanged, this, [this](int hz) {
        radio_->setPbtHz(Rx::Main, hz);
        rigPbtHz_ = hz;
        sinceFilterEdit_.invalidate();              // explicit action: redraw now
        refreshPassbandOverlay();
        sinceFilterEdit_.restart();                 // ...then guard the poll echo
        statusBar()->showMessage(QString("PBT -> %1%2 Hz")
                                     .arg(hz > 0 ? "+" : "").arg(hz));
    });

    // The radio reported its frequency (startup sync or dial-follow poll): keep the
    // rigctld cache and the panadapter center locked to the physical VFO.
    connect(radio_, &RadioController::frequencyReported, this,
            [this](Rx rx, uint64_t hz) {
                if (rx == Rx::Sub) {                 // VFO B: display-follow only
                    if (!sinceVfoBEdit_.isValid() || sinceVfoBEdit_.elapsed() > 1500) {
                        vfoBHz_ = hz;
                        freqDispB_->setFrequency(hz);
                        pushVfoB();
                    }
                    return;
                }
                awaitingFreq_ = false;               // poll answered; next one may go
                if (std::getenv("TTC_SELFTEST"))
                    std::fprintf(stderr, "[radio] VFO-A reports %.4f MHz\n", hz / 1e6);
                if (hz != centerHz_) {
                    // Right after a click-to-tune the radio takes a few hundred ms to
                    // settle; stale reads must not "confirm" the old frequency and
                    // yank the tune back. Ignore dial-follow during that window.
                    if (sinceTune_.isValid() && sinceTune_.elapsed() < 1500) return;
                    // The Orion occasionally emits a mangled frame that still parses
                    // as a plausible frequency. Require two consecutive identical
                    // reads before following a dial change (~200 ms, imperceptible).
                    if (hz != pendingHz_) { pendingHz_ = hz; return; }
                    centerHz_ = hz;
                    rigctld_.cacheFrequency(hz);     // cache only debounce-confirmed values
                    freqDisp_->setFrequency(hz);
                    pan_->setCenterHz(hz);           // keep grid labels on the dial
                    syncBandRegister();              // mirror the move into the stack
#ifdef HAVE_SDRPLAY
                    sdr_.setCenterFrequency(static_cast<double>(hz + kLoOffsetHz));
#endif
                    statusBar()->showMessage(
                        QString("radio %1 MHz  |  panadapter following").arg(hz / 1e6, 0, 'f', 4));
                }
            });

    // CW keys — Z: 0-beat the strongest signal in the passband; X: CW⇄CWR
    // flip (same note pitch both ways = exactly zero-beat, works by ear).
    // Guarded so typing a Z or X into the CALL/GRID fields never fires them.
    const auto keyGuard = [](auto fn) {
        return [fn] {
            if (qobject_cast<QLineEdit*>(QApplication::focusWidget())) return;
            fn();
        };
    };
    connect(new QShortcut(QKeySequence(Qt::Key_Z), this), &QShortcut::activated,
            this, keyGuard([this] { zeroBeat(); }));
    connect(new QShortcut(QKeySequence(Qt::Key_X), this), &QShortcut::activated,
            this, keyGuard([this] { flipCwSideband(); }));

    // Start the interop seam. Clients (cqrlog/WSJT-X/fldigi/GridTracker) connect
    // here. TTC_RIGCTLD_PORT overrides for side-by-side protocol testing.
    const char* rcpEnv = std::getenv("TTC_RIGCTLD_PORT");
    const bool listening = rigctld_.listen(rcpEnv ? quint16(atoi(rcpEnv)) : 4532);
    statusBar()->showMessage(
        listening ? "rigctld emulation on :4532  |  radio: not connected (wire /dev/orion)"
                  : "FAILED to bind :4532 (already in use? stop flrig/rigctld)");

#ifdef HAVE_SDRPLAY
    // IQ -> FFT -> panadapter. The spectrum callback runs on the SDR thread, so
    // marshal the result onto the GUI thread before touching the widget.
    // PowerSDR-style offset LO (their if_freq / spur reduction): the RSP2
    // tunes kLoOffsetHz ABOVE the dial, so the zero-IF DC artifact — line,
    // phase-noise hump and recalibration transients — sits at a fixed
    // +60 kHz from the tuned frequency, never on it and never in a passband.
    // The capture is widened to 500 kHz so the dial-centered view still
    // reaches large spans (max symmetric span = 500k - 2*60k = 380 kHz).
    constexpr double kSampleRate = 2000000.0;
    constexpr int    kDecim      = 4;                 // 500 kHz capture
    const int spanHz = static_cast<int>(kSampleRate / kDecim);
    pan_->setSpanHz(spanHz);
    sdrSpanHz_ = spanHz;                              // CW zap peak finder
    pan_->setDialOffsetHz(kLoOffsetHz);
    spectrum_.setOutput([this](const std::vector<float>& db) {
        if (std::getenv("TTC_SELFTEST")) {          // headless evidence the FFT is live
            static int frames = 0;
            if (++frames % 20 == 1) {
                float mn = 1e9f, mx = -1e9f;
                for (float v : db) { mn = std::min(mn, v); mx = std::max(mx, v); }
                std::fprintf(stderr, "[spectrum] frame %d  bins=%zu  min %.1f dB  peak %.1f dB\n",
                             frames, db.size(), mn, mx);
                if (std::getenv("TTC_DCDUMP")) {    // profile the center hump
                    const int c = static_cast<int>(db.size()) / 2;
                    for (int o = -20; o <= 20; o += 2)
                        std::fprintf(stderr, "[dc] %+5d Hz  %.1f dB\n", o * 61, db[c + o]);
                }
            }
        }
        QMetaObject::invokeMethod(pan_, [this, spanHz, copy = db]() mutable {
            pan_->setSpectrum(copy);
            // SDR-source S-meter: integrate power across the RX passband.
            // These bins never pass through the radio's AGC or RF gain, so
            // the reading stays honest with RF gain backed down (the Omni
            // VII's own meter pins at the RF-gain floor — unfixable there).
            // gRdB is exact dB by API definition and the LNA states map to
            // published RSP2 gain figures, so riding the SDR gain slider
            // shifts nothing once the one-time cal offset is stored.
            int lo = 0, hi = 0;
            edgesFromRig(rigMode_, rigBwHz_, rigPbtHz_, lo, hi);
            const int    n     = static_cast<int>(copy.size());
            const double binHz = double(spanHz) / n;
            int i0 = n / 2 + static_cast<int>(std::floor((lo - kLoOffsetHz) / binHz));
            int i1 = n / 2 + static_cast<int>(std::ceil((hi - kLoOffsetHz) / binHz));
            i0 = std::clamp(i0, 0, n - 1);
            i1 = std::clamp(i1, i0, n - 1);
            double acc = 0.0;
            for (int i = i0; i <= i1; ++i)
                acc += std::pow(10.0, copy[i] / 10.0);
            if (acc <= 0.0) return;
            // RSP2 LNA-state gain reduction, 0-420 MHz table (SDRplay API spec).
            static const int kRsp2LnaDb[] = {0, 10, 15, 21, 24, 34, 39, 45, 64};
            const double meas = 10.0 * std::log10(acc)
                + sdr_.gainReduction()
                + kRsp2LnaDb[std::clamp(sdr_.lnaState(), 0, 8)];
            // Clipped frames are hash, not signal: while the ADC reports
            // overload (the first ms of TX before the panic slam lands),
            // freeze the display instead of painting the splash.
            if (QDateTime::currentMSecsSinceEpoch() - sdr_.lastOverloadMs()
                < 150) return;
            lastSdrMeasDb_ = meas;
            smeter_->setSdrLevel(meas + sdrCalDb_);
            lastSpectrum_ = std::move(copy);          // CW zap reads this
        }, Qt::QueuedConnection);
    });
    // CW reader taps the same IQ stream (SDR thread; a no-op until the CW
    // window turns it on). The tuned carrier is always -kLoOffsetHz from
    // the SDR LO, so the decoder's mixer never moves.
    cwDec_ = new CwDecoder(double(spanHz), -double(kLoOffsetHz), this);
    Q_ASSERT(spanHz == 500000);            // skim_ was built for this rate
    const auto iqHandler = [this](const IqBlock& iq) {
        spectrum_.addSamples(iq);
        // During ADC clip (TX onset) the stream is broadband hash — keep
        // it out of the decoders; the display freeze handles the eyes.
        const bool clipped =
            QDateTime::currentMSecsSinceEpoch() - sdr_.lastOverloadMs() < 150;
        if (!clipped) {
            cwDec_->processIq(iq.data(), iq.size());
            skim_->processIq(iq.data(), iq.size());
        }
        iqRec_->feed(iq);
        // Wideband peak for the Auto LNA loop (read+reset by a GUI timer).
        float m = 0.0f;
        for (const auto& z : iq)
            m = std::max({m, std::abs(z.real()), std::abs(z.imag())});
        float cur = iqPeak_.load(std::memory_order_relaxed);
        while (m > cur && !iqPeak_.compare_exchange_weak(
                              cur, m, std::memory_order_relaxed)) {}
    };
    // Replay mode: TTC_IQFILE feeds a band recording through the exact
    // pipeline the RSP2 would (paced to real time; TTC_IQFAST=1 free-runs).
    if (const char* rf = std::getenv("TTC_IQFILE")) {
        QFile* f = new QFile(QString::fromLocal8Bit(rf), this);
        char hdr[32];
        double fRate = 0.0, fCenter = 0.0;
        if (f->open(QIODevice::ReadOnly) && f->read(hdr, 32) == 32
            && std::memcmp(hdr, "TTCIQ01", 7) == 0) {
            std::memcpy(&fRate, hdr + 8, 8);
            std::memcpy(&fCenter, hdr + 16, 8);
            centerHz_ = uint64_t(fCenter) - kLoOffsetHz;
            freqDisp_->setFrequency(centerHz_);
            pan_->setCenterHz(centerHz_);
            const bool fast = std::getenv("TTC_IQFAST") != nullptr;
            replayThread_ = QThread::create([this, f, fast] {
                std::vector<int16_t> raw(16384 * 2);
                IqBlock blk(16384);
                while (!replayStop_.load(std::memory_order_relaxed)) {
                    const qint64 got = f->read(
                        reinterpret_cast<char*>(raw.data()),
                        qint64(raw.size() * 2));
                    if (got < qint64(raw.size() * 2)) break;   // EOF
                    for (size_t i = 0; i < blk.size(); ++i)
                        blk[i] = {raw[2 * i] / 32767.0f,
                                  raw[2 * i + 1] / 32767.0f};
                    spectrum_.addSamples(blk);
                    cwDec_->processIq(blk.data(), blk.size());
                    skim_->processIq(blk.data(), blk.size());
                    if (!fast) QThread::usleep(32768);         // real time
                }
                std::fprintf(stderr, "[replay] end of file\n");
            });
            replayThread_->start();
            statusBar()->showMessage(
                QString("REPLAY %1 (%2 MHz)").arg(rf)
                    .arg(centerHz_ / 1e6, 0, 'f', 4));
        } else {
            statusBar()->showMessage(QString("replay: bad file %1").arg(rf));
        }
    } else {
        sdr_.setIqCallback(iqHandler);
    }
    sdr_.setDecimation(kDecim);
    if (!replayThread_ && sdr_.apiOk()
        && sdr_.start(static_cast<double>(centerHz_ + kLoOffsetHz), kSampleRate)) {
        statusBar()->showMessage(QString("RSP2 panadapter %1 MHz, span %2 kHz  |  rigctld :4532")
                                     .arg(centerHz_ / 1e6, 0, 'f', 4).arg(spanHz / 1000));
        // Unattended capture: TTC_RECIQ=<secs> records from startup (pair
        // with TTC_SELFTEST a little longer to quit cleanly after).
        if (const char* rs = std::getenv("TTC_RECIQ")) {
            const int secs = atoi(rs);
            recIqAct_->setChecked(true);
            QTimer::singleShot(secs * 1000, this, [this] {
                if (recIqAct_->isChecked()) recIqAct_->setChecked(false);
            });
        }
    } else if (!replayThread_) {
        statusBar()->showMessage("SDR unavailable: " + QString::fromStdString(sdr_.lastError()));
    }
#endif

    // Open the radio so click-to-tune / drag-to-filter actually reach it, and
    // poll its VFO so the panadapter follows the physical dial. Device path:
    // TTC_RADIO_DEV env > radio/device setting > per-radio default (both
    // rigs live on the FT4232H quad adapter — Orion port 0, Omni port 3).
    const char* devEnv = std::getenv("TTC_RADIO_DEV");
    const std::string radioDev = devEnv ? devEnv
        : QSettings().value("radio/device",
              radio_->caps().dualReceiver
                  ? "/dev/orion"
                  : "/dev/serial/by-id/usb-FTDI_FT4232H_Device_FT73ZILE-if03-port0")
              .toString().toStdString();
    if (radio_->open(radioDev)) {
        radio_->queryFrequency(Rx::Main);            // one-shot sync at startup
        radio_->queryFrequency(Rx::Sub);             // VFO B readout
        radio_->queryVfoAssignment();                // routing matrices
        radio_->queryAntennaRouting();
        radio_->queryMode(Rx::Main);
        radio_->queryMode(Rx::Sub);                  // transfers carry the mode
        radio_->queryFilter(Rx::Sub);                // B's on-screen filter width
        radio_->queryFilter(Rx::Main);
        radio_->queryAgc(Rx::Main);                  // sync the control sidebar
        radio_->queryAgcThreshold(Rx::Main);
        radio_->queryAgcHang(Rx::Main);
        radio_->queryAgcDecay(Rx::Main);
        radio_->queryRfGain(Rx::Main);
        radio_->queryAttenuator(Rx::Main);
        radio_->queryPreamp(Rx::Main);
        radio_->queryDspLevels(Rx::Main);            // syncs NR/NB/AN/hw-NB sliders
        radio_->queryNotch(Rx::Main);                // syncs notch center/width/engage
        radio_->queryTxPower();                      // syncs the TX bar
        radio_->queryTxAudio();
        radio_->queryAfVolume(Rx::Sub);              // AUDIO panel (?UM in TxAudio)
        radio_->queryAudioRouting();
        // The startup burst can flood the Orion's UART servicing and drop a
        // few query responses — re-ask for the audio state once it settles.
        // (Seen live: mic slider stuck at 0 against the radio's 51 because
        // the barrage ate the ?TM answer.)
        QTimer::singleShot(2500, this, [this] {
            radio_->queryAfVolume(Rx::Main);
            radio_->queryAfVolume(Rx::Sub);
            radio_->queryAudioRouting();
            radio_->queryTxAudio();                  // mic / proc / TX BW / monitor
        });
        radio_->queryAuxInputGain();
        radio_->queryTuner();
        radio_->queryVfoLock('A');
        radio_->queryVfoLock('B');
        awaitingFreq_ = true;
        freqQueryAge_.start();
        auto* poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this] {
            if (!radio_->connected()) return;
            ++pollTick_;
            // 1 s rotation at 200 ms/tick, sized to the Orion's ~100 ms UART
            // servicing cycle: 2 Hz S-meter, 2 Hz frequency, mode and filter
            // alternating at 0.5 Hz each. Every 5 s one S-meter slot is lent
            // to the AGC/RF-gain/attenuator round-robin so front-panel knob
            // changes eventually reflect in the sidebar.
            const int phase = pollTick_ % 5;
            if (phase == 1 || phase == 3) {
                if (pollTick_ % 25 == 3) {
                    switch ((pollTick_ / 25) % 13) {
                        case 0:  radio_->queryAgc(Rx::Main);
                                 radio_->queryAgcThreshold(Rx::Main); break;
                        case 9:  radio_->queryAgcHang(Rx::Main);
                                 radio_->queryAgcDecay(Rx::Main);     break;
                        case 1:  radio_->queryRfGain(Rx::Main);     break;
                        case 2:  radio_->queryAttenuator(Rx::Main); break;
                        case 3:  radio_->queryPreamp(Rx::Main);     break;
                        case 4:  radio_->queryTxPower();            break;
                        case 5:  radio_->queryVfoAssignment();      break;
                        case 6:  radio_->queryAntennaRouting();     break;
                        case 7:  radio_->queryAfVolume(Rx::Main);   // front-panel knobs
                                 radio_->queryAfVolume(Rx::Sub);    break;
                        case 8:  radio_->queryAudioRouting();       break;
                        case 10: radio_->queryTxFilter();           // front-panel TX
                                 radio_->querySpeechProc();         // audio changes
                                 radio_->queryMicGain();
                                 break;
                        case 11: radio_->queryVfoLock('A');         // front-panel lock
                                 radio_->queryVfoLock('B');         // button presses
                                 break;
                        default: radio_->queryNotch(Rx::Main);      break;
                    }
                } else {
                    radio_->querySMeter();
                }
                return;
            }
            if (phase == 2) {
                switch ((pollTick_ / 5) % 5) {       // main mode/filter, VFO B side
                    case 0:  radio_->queryMode(Rx::Main);      break;
                    case 1:  radio_->queryFilter(Rx::Main);    break;
                    case 2:  radio_->queryFrequency(Rx::Sub);  break;
                    case 3:  radio_->queryMode(Rx::Sub);       break;
                    default: radio_->queryFilter(Rx::Sub);     break;
                }
                return;
            }
            // Only one ?AF in flight: the Orion's response tail runs ~110 ms. Re-arm
            // after 600 ms anyway in case a response was dropped.
            if (awaitingFreq_ && freqQueryAge_.elapsed() < 600) return;
            radio_->queryFrequency(Rx::Main);
            awaitingFreq_ = true;
            freqQueryAge_.restart();
        });
        poll->start(200);
    } else {
        statusBar()->showMessage("radio: could not open " + QString::fromStdString(radioDev));
    }

    // Capability gating — gray out what the connected radio can't do rather
    // than leave live-looking controls that silently do nothing. Omni 8:
    // single receiver, no hardware NB, no manual notch/SAF, and the whole
    // TX-audio/power/tuner group answers only in REMOTE mode.
    if (!radio_->caps().dualReceiver) {
        txBar_->setCatTxControlsEnabled(false);
        panel_->setReducedCatSet(true);
        volSl_[1]->setEnabled(false);              // one receiver: B has no audio
        volLbl_[1]->setEnabled(false);
        muteBtn_[1]->setEnabled(false);
        audioPanel_->setEnabled(false);            // Orion output routing / monitor
        routing_->setSplitOnly(true);              // ANT + sub rows are Orion-only
    }
}

MainWindow::~MainWindow() {
    stopManualTune();                            // never leave a carrier up
    dvr_->stop();                                // finalize an in-flight take
    if (dvrTxPlayback_) {
        radio_->setPtt(false);                   // never leave the rig keyed
        if (dvrAutoDig_) setDigitalMode(false);  // nor on line-in with mic parked
    }
    saveBandMemory();                            // remember this band across runs
    if (iqRec_ && iqRec_->active()) iqRec_->stop();  // finalize the file
    if (replayThread_) {                         // same teardown-order rule
        replayStop_.store(true, std::memory_order_relaxed);
        replayThread_->wait(2000);
    }
#ifdef HAVE_SDRPLAY
    // Stop the SDRplay streaming thread (Uninit drains callbacks) BEFORE the
    // spectrum_/dsp members it references are destroyed. Without this, member
    // teardown order frees the FFT under a still-running callback (use-after-free).
    sdr_.stop();
#endif
}
} // namespace ttc
