// SPDX-License-Identifier: GPL-2.0-or-later
// The second-deck tool buttons, one self-contained builder per feature.
// Each runs once from the MainWindow constructor; new tools get a new
// function here (and a one-line call there) — buttons go on topLay2_,
// never the first deck, or the window minimum outgrows the screen.
#include "app/MainWindow.h"
#include "cw/AudioCwSource.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"
#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"
#include "cw/SkimServer.h"
#include "net/FldigiClient.h"
#include "ui/DigiWindow.h"
#include "ui/SkimmerWindow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QUdpSocket>
#include <QWidgetAction>
#include <algorithm>

namespace ttc {

// MASTER.SCP loader (shared logic): cqrlog ships the contest super-check
// list; the user's own copy wins if present.
static QSet<QString> loadMasterScp() {
    QSet<QString> out;
    for (const QString& p :
         {QDir::homePath() + "/.config/cqrlog/MASTER.SCP",
          QStringLiteral("/usr/share/cqrlog/ctyfiles/MASTER.SCP")}) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            out.insert(QString::fromLatin1(line).toUpper());
        }
        break;
    }
    return out;
}


void MainWindow::setupLogUi() {
    // "LOG" dropdown: one-click QSO logging into cqrlog. Sends the finished
    // QSO as a headerless ADIF UDP datagram to the cqrlog fork's always-on
    // console bridge (127.0.0.1:2334); cqrlog saves it through its normal
    // path, so DXCC/awards fields fill exactly as if typed there. Call and
    // park prefill from the last clicked spot; TIME_ON is when the call
    // landed in the field, TIME_OFF is when LOG is pressed.
    auto* logBtn = new QToolButton(topStrip_);
    logBtn->setText("LOG ▾");
    logBtn->setPopupMode(QToolButton::InstantPopup);
    logBtn->setFocusPolicy(Qt::NoFocus);
    logBtn->setStyleSheet(QString(kToolBtnStyle));
    logBtn->setToolTip("Log a QSO to cqrlog (call/park prefill from the last "
                       "clicked spot;\nEnter in any field = log)");
    auto* logMenu = new QMenu(logBtn);
    styleMenu(logMenu);
    auto* logW = new QWidget;
    logW->setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 13px; }"
        "QLineEdit { background: #1c2430; border: 1px solid #2a3644;"
        " border-radius: 3px; padding: 4px 8px; }"
        "QLabel { color: #8fa3b8; font-weight: bold; font-size: 12px; }"
        "QPushButton { background: #2f6d9e; border: 1px solid #5db2f0;"
        " border-radius: 3px; padding: 5px 14px; font-weight: bold; }"
        "QPushButton:hover { background: #3a7fb5; }");
    auto* lg = new QGridLayout(logW);
    lg->setContentsMargins(14, 12, 14, 12);
    lg->setHorizontalSpacing(10);
    lg->setVerticalSpacing(9);
    lg->addWidget(new QLabel("CALL", logW), 0, 0);
    logCall_ = new QLineEdit(logW);
    logCall_->setMaxLength(14);
    lg->addWidget(logCall_, 0, 1, 1, 3);
    lg->addWidget(new QLabel("RST S", logW), 1, 0);
    logRstS_ = new QLineEdit(logW);
    logRstS_->setFixedWidth(64);
    lg->addWidget(logRstS_, 1, 1);
    lg->addWidget(new QLabel("RST R", logW), 1, 2);
    logRstR_ = new QLineEdit(logW);
    logRstR_->setFixedWidth(64);
    lg->addWidget(logRstR_, 1, 3);
    lg->addWidget(new QLabel("PARK", logW), 2, 0);
    logPark_ = new QLineEdit(logW);
    logPark_->setMaxLength(20);
    logPark_->setToolTip("POTA park reference of the station you worked "
                         "(prefilled from POTA spots) — leave blank otherwise");
    lg->addWidget(logPark_, 2, 1, 1, 3);
    auto* logGo = new QPushButton("Log to cqrlog", logW);
    lg->addWidget(logGo, 3, 0, 1, 4);
    auto* logAct = new QWidgetAction(logMenu);
    logAct->setDefaultWidget(logW);
    logMenu->addAction(logAct);
    logBtn->setMenu(logMenu);
    topLay2_->addWidget(logBtn);
    logUdp_ = new QUdpSocket(this);
    // RST defaults follow the mode each time the panel opens (only when the
    // field still holds a default — a typed report is never overwritten).
    connect(logMenu, &QMenu::aboutToShow, this, [this] {
        const bool cw = rigMode_ == Mode::CWU || rigMode_ == Mode::CWL;
        const QStringList defs{"", "59", "599"};
        if (defs.contains(logRstS_->text().trimmed()))
            logRstS_->setText(cw ? "599" : "59");
        if (defs.contains(logRstR_->text().trimmed()))
            logRstR_->setText(cw ? "599" : "59");
        logCall_->setFocus();
    });
    // QSO start clock: arms when a call first lands in the field.
    connect(logCall_, &QLineEdit::textEdited, this, [this](const QString& t) {
        if (!t.trimmed().isEmpty() && !qsoStartUtc_.isValid())
            qsoStartUtc_ = QDateTime::currentDateTimeUtc();
        if (t.trimmed().isEmpty()) qsoStartUtc_ = QDateTime();
        if (cwWin_) cwWin_->setHisCall(t.trimmed().toUpper());  // %c macro
    });
    const auto doLog = [this, logMenu] {
        const QString call = logCall_->text().trimmed().toUpper();
        if (call.isEmpty()) {
            statusBar()->showMessage("LOG: no callsign", 3000);
            return;
        }
        const auto tag = [](const char* t, const QString& v) {
            return QString("<%1:%2>%3").arg(t).arg(v.size()).arg(v);
        };
        QString mode, submode;
        switch (rigMode_) {
            case Mode::CWU: case Mode::CWL: mode = "CW"; break;
            case Mode::USB: mode = "SSB"; submode = "USB"; break;
            case Mode::LSB: mode = "SSB"; submode = "LSB"; break;
            case Mode::AM:  mode = "AM"; break;
            case Mode::FM:  mode = "FM"; break;
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QDateTime on  = qsoStartUtc_.isValid() ? qsoStartUtc_ : now;
        QString adif = tag("CALL", call)
            + tag("FREQ", QString::number(centerHz_ / 1e6, 'f', 6))
            + tag("MODE", mode);
        if (!submode.isEmpty()) adif += tag("SUBMODE", submode);
        adif += tag("QSO_DATE", on.toString("yyyyMMdd"))
              + tag("TIME_ON",  on.toString("hhmm"))
              + tag("TIME_OFF", now.toString("hhmm"));
        if (!logRstS_->text().trimmed().isEmpty())
            adif += tag("RST_SENT", logRstS_->text().trimmed());
        if (!logRstR_->text().trimmed().isEmpty())
            adif += tag("RST_RCVD", logRstR_->text().trimmed());
        const QString park = logPark_->text().trimmed().toUpper();
        if (!park.isEmpty())
            adif += tag("SIG", "POTA") + tag("SIG_INFO", park);
        adif += "<EOR>";
        logUdp_->writeDatagram(adif.toUtf8(), QHostAddress::LocalHost,
            quint16(QSettings().value("log/port", 2334).toInt()));
        statusBar()->showMessage(
            QString("QSO %1 sent to cqrlog (%2 %3 MHz)")
                .arg(call, mode)
                .arg(centerHz_ / 1e6, 0, 'f', 4), 6000);
        logCall_->clear();
        logPark_->clear();
        qsoStartUtc_ = QDateTime();
        logMenu->hide();
        logbook_.refreshSoon(3000);       // recolor spots with the new QSO
    };
    connect(logGo, &QPushButton::clicked, this, doLog);
    for (QLineEdit* e : {logCall_, logRstS_, logRstR_, logPark_})
        connect(e, &QLineEdit::returnPressed, this, doLog);
    // Spot click -> prefill call (and park for POTA spots), arm the clock.
    connect(pan_, &PanadapterWidget::spotClicked, this,
            [this](const QString& call, QChar kind, const QString& tg) {
                logCall_->setText(call);
                logPark_->setText(kind == QChar('P') ? tg : QString());
                qsoStartUtc_ = QDateTime::currentDateTimeUtc();
                if (cwWin_) cwWin_->setHisCall(call);
            });

}

void MainWindow::setupCwUi() {
    // "CW" button: the WinKeyer sending window (type-ahead + memories).
    // The keyer hardware keeps the paddle in charge; this is the keyboard.
    auto* cwBtn = new QToolButton(topStrip_);
    cwBtn->setText("CW");
    cwBtn->setFocusPolicy(Qt::NoFocus);
    cwBtn->setStyleSheet(QString(kToolBtnStyle));
    cwBtn->setToolTip("CW keyboard/memories via the WinKeyer\n(paddle always "
                      "wins — touching it dumps the buffer)");
    topLay2_->addSpacing(8);
    topLay2_->addWidget(cwBtn);
    connect(cwBtn, &QToolButton::clicked, this, [this] {
        if (!cwWin_) {
            cwWin_ = new CwWindow(this);
            cwWin_->setMyCall(QSettings()
                .value("station/callsign", "N8EM").toString());
            cwWin_->setHisCall(logCall_ ? logCall_->text() : QString());
            if (cwDec_) {                  // SDR-fed CW reader plumbing
                connect(cwDec_, &CwDecoder::textDecoded,
                        cwWin_, &CwWindow::appendRx, Qt::QueuedConnection);
                connect(cwDec_, &CwDecoder::wpmEstimated,
                        cwWin_, &CwWindow::setRxWpm, Qt::QueuedConnection);
                // Two selectable ears for the same reader: the SDR at
                // the dial (default; AF can be zero) or the RADIO's audio
                // via the SignaLink — the input real fldigi gets, and the
                // weak-signal winner while the SDR rides the passive tap.
                // One decoder instance per source; exactly one enabled.
                const int pitch =
                    QSettings().value("cw/pitchHz", 550).toInt();
                audioDec_ = new CwDecoder(48000.0, double(pitch), this);
                audioSrc_ = new AudioCwSource(audioDec_, this);
                connect(audioDec_, &CwDecoder::textDecoded,
                        cwWin_, &CwWindow::appendRx, Qt::QueuedConnection);
                connect(audioDec_, &CwDecoder::wpmEstimated,
                        cwWin_, &CwWindow::setRxWpm, Qt::QueuedConnection);
                connect(audioSrc_, &AudioCwSource::pitchMeasured,
                        cwWin_, &CwWindow::setRxPitch);
                connect(audioSrc_, &AudioCwSource::pitchMeasured, this,
                        [this](double hz) { pitchTrimFeed(hz); });
                connect(audioSrc_, &AudioCwSource::statusChanged, this,
                        [this](const QString& t) {
                            statusBar()->showMessage(t, 6000);
                        });
                rxRadio_ = QSettings().value("cw/rxRadio", false).toBool();
                const auto applyRouting = [this] {
                    cwDec_->setEnabled(rxWanted_ && !rxRadio_);
                    audioDec_->setEnabled(rxWanted_ && rxRadio_);
                    // Capture runs whenever decode is on, regardless of
                    // source: the pitch readout measures the radio's audio
                    // even while the SDR does the decoding.
                    if (rxWanted_) audioSrc_->start();
                    else audioSrc_->stop();
                };
                connect(cwWin_, &CwWindow::rxDecodeWanted, this,
                        [this, applyRouting](bool on) {
                            rxWanted_ = on;
                            applyRouting();
                        });
                audioSrc_->setNr(QSettings().value("cw/nr", false).toBool());
                connect(cwWin_, &CwWindow::rxNrChanged, this,
                        [this](bool on) { audioSrc_->setNr(on); });
                connect(cwWin_, &CwWindow::zeroBeatRequested, this,
                        [this] { zeroBeat(); });
                connect(cwWin_, &CwWindow::txImminent, this, [this] {
                    txPredictMs_ = QDateTime::currentMSecsSinceEpoch();
                });
                connect(cwWin_, &CwWindow::rxSourceChanged, this,
                        [this, applyRouting](bool radio) {
                            rxRadio_ = radio;
                            applyRouting();
                        });
                // Decode-engine adjustments: apply the persisted state now,
                // then live-follow the window's controls. This tuned reader
                // is the only instance that runs the fldigi engine — the
                // skimmer's channels stay on the legacy path.
                const auto applyCfg = [this](bool eng, bool som, bool deep,
                                             int atk, int dcy) {
                    for (CwDecoder* d : {cwDec_, audioDec_}) {
                        d->setEngineMode(eng);
                        d->setSom(som);
                        d->setDeep(deep);
                        d->setAttack(atk);
                        d->setDecay(dcy);
                    }
                };
                QSettings cs;
                applyCfg(cs.value("cw/engine", true).toBool(),
                         cs.value("cw/som", true).toBool(),
                         cs.value("cw/deep", false).toBool(),
                         cs.value("cw/attack", 1).toInt(),
                         cs.value("cw/decay", 1).toInt());
                connect(cwWin_, &CwWindow::rxDecodeConfigChanged, this,
                        applyCfg);
            }
        }
        cwWin_->show();
        cwWin_->raise();
        cwWin_->activateWindow();
    });

}

void MainWindow::setupSkimUi(const QString& stationCall) {
    // "SKIM" dropdown: the CW skimmer — a bank of decoder channels parked on
    // the strongest signals in the band's CW segment, mining the decoded
    // text for callsigns. Found calls land on the panadapter as violet
    // spots, worked-before colored like everything else. The engine exists
    // even without an SDR build; it just never gets IQ.
    skim_ = new SkimmerEngine(                     // rate must match the SDR
        500000.0,
        std::clamp(QSettings().value("skim/channels", 24).toInt(), 4, 64),
        this);
    skim_->setCallValidator([this](const QString& call) {
        double la = 0.0, lo = 0.0;                 // decode artifacts have
        return cty_.lookup(call, la, lo);          // no country prefix
    });
    skim_->setKnownCalls(loadMasterScp());
    auto* skimBtn = new QToolButton(topStrip_);
    skimBtn->setText("SKIM ▾");
    skimBtn->setPopupMode(QToolButton::InstantPopup);
    skimBtn->setFocusPolicy(Qt::NoFocus);
    skimBtn->setStyleSheet(QString(kToolBtnStyle));
    skimBtn->setToolTip("CW skimmer: decode the whole CW segment at once;\n"
                        "found callsigns appear as violet spots");
    auto* skimMenu = new QMenu(skimBtn);
    styleMenu(skimMenu);
    skimEnable_ = skimMenu->addAction("Skim the CW segment");
    skimEnable_->setCheckable(true);
    skimEnable_->setChecked(QSettings().value("skim/enabled", false).toBool());
    skimEnable_->setToolTip(
        QString("Run %1 decoder channels over the CW segment of the current "
                "band.\nCalls are validated against the country file and "
                "must repeat (or follow DE)\nbefore they're spotted.")
            .arg(skim_->channelCount()));
    skim_->setEnabled(skimEnable_->isChecked());
    // Band map: the skimmer's finds as a frequency-sorted click-to-tune
    // list (separate window, so it can live on all session).
    auto* skimMap = skimMenu->addAction("Band map…");
    connect(skimMap, &QAction::triggered, this, [this] {
        if (!skimWin_) {
            skimWin_ = new SkimmerWindow(
                skim_,
                [this](const QString& call, qint64 hz) {
                    if (!logbook_.ready()) return QChar('?');
                    return logbook_.status(call, LogbookIndex::bandForHz(hz));
                },
                this);
            connect(skimWin_, &SkimmerWindow::tuneTo, this,
                    [this](qint64 hz, const QString& call) {
                        tuneAbsolute(uint64_t(hz));
                        if (!call.isEmpty() && logCall_) {
                            logCall_->setText(call);
                            qsoStartUtc_ = QDateTime::currentDateTimeUtc();
                            if (cwWin_) cwWin_->setHisCall(call);
                        }
                    });
        }
        skimWin_->show();
        skimWin_->raise();
        skimWin_->activateWindow();
    });
    // Local RBN: serve the finds over cluster telnet while the skimmer
    // runs — point cqrlog's DX-cluster window at localhost:7300 and this
    // station spots for itself.
    skimSrv_ = new SkimServer(this);
    skimSrv_->setSpotterCall(stationCall);
    connect(skim_, &SkimmerEngine::spotFound, skimSrv_, &SkimServer::announce);
    auto* skimTelnet = skimMenu->addAction(
        QString("Telnet feed on localhost:%1 (for cqrlog)")
            .arg(QSettings().value("skim/telnetPort", 7300).toInt()));
    skimTelnet->setEnabled(false);
    skimTelnet->setToolTip(
        "While the skimmer runs, its finds are served in DX-cluster telnet\n"
        "format. In cqrlog: DX cluster -> connect to localhost port 7300 —\n"
        "your own receiver becomes a spotting node.");
    skimStatus_ = new QLabel(skimMenu);
    skimStatus_->setTextFormat(Qt::RichText);
    skimStatus_->setStyleSheet("QLabel { background: #141b24; color: #c8d4e0;"
                               " padding: 8px 12px; }");
    auto* skimStatAct = new QWidgetAction(skimMenu);
    skimStatAct->setDefaultWidget(skimStatus_);
    skimMenu->addSeparator();
    skimMenu->addAction(skimStatAct);
    skimBtn->setMenu(skimMenu);
    topLay2_->addSpacing(8);
    topLay2_->addWidget(skimBtn);
    const auto refreshSkim = [this, skimMenu] {
        if (!skimMenu->isVisible()) return;
        QString h = "<pre style='margin:0; font-size:12px;'>"
                    "<span style='color:#8fa3b8;'>  kHz      WPM CALL     "
                    "DECODE</span>\n";
        for (const auto& c : skim_->channelInfo()) {
            if (!c.active) { h += "<span style='color:#4a5a6e;'>  —</span>\n"; continue; }
            h += QString("  %1 %2 %3 %4\n")
                     .arg(c.hz / 1000.0, -8, 'f', 1)
                     .arg(c.wpm > 0 ? QString::number(c.wpm) : QString("--"), 3)
                     .arg(c.call.isEmpty()
                              ? QString("<span style='color:#4a5a6e;'>?"
                                        "       </span>")
                              : QString("<span style='color:#cd8cff;'>%1</span>")
                                    .arg(c.call.leftJustified(8)),
                          -8)
                     .arg(c.text.toHtmlEscaped());
        }
        h += "</pre>";
        skimStatus_->setText(h);
    };
    connect(skimMenu, &QMenu::aboutToShow, this, [this, refreshSkim] {
        skimStatus_->setText(" ");                 // sized before first tick
        refreshSkim();
    });
    // One clock drives channel (re)assignment from the latest averaged
    // spectrum and the menu readout.
    auto* skimTick = new QTimer(this);
    skimTick->setInterval(2500);
    connect(skimTick, &QTimer::timeout, this, [this, refreshSkim] {
#ifdef HAVE_SDRPLAY
        if (skim_->enabled() && !lastSpectrum_.empty())
            skim_->updateFromSpectrum(lastSpectrum_, sdrSpanHz_,
                                      qint64(centerHz_), kLoOffsetHz);
#endif
        refreshSkim();
    });
    skimTick->start();
    connect(skim_, &SkimmerEngine::spotFound, this,
            [this](const QString& call, qint64 hz, int wpm) {
                statusBar()->showMessage(
                    QString("SKIM: %1 on %2 kHz%3")
                        .arg(call)
                        .arg(hz / 1000.0, 0, 'f', 1)
                        .arg(wpm > 0 ? QString("  %1 WPM").arg(wpm)
                                     : QString()),
                    8000);
            });

}

void MainWindow::setupDigiUi() {
    // "DIGI" button: the fldigi companion window (modem/carrier readout,
    // decoded text, click-to-carrier). fldigi already follows the dial
    // through rigctld; this is the audio-domain half of the link.
    auto* digiBtn = new QToolButton(topStrip_);
    digiBtn->setText("DIGI");
    digiBtn->setFocusPolicy(Qt::NoFocus);
    digiBtn->setStyleSheet(QString(kToolBtnStyle));
    digiBtn->setToolTip("fldigi link: decoded text + click a passband trace "
                        "to set fldigi's carrier\n(fldigi must have XML-RPC "
                        "on, its default)");
    topLay2_->addSpacing(8);
    topLay2_->addWidget(digiBtn);
    connect(digiBtn, &QToolButton::clicked, this, [this] {
        if (!digiWin_) {
            fldigi_ = new FldigiClient(this);
            fldigi_->setEndpoint(
                QSettings().value("digi/host", "127.0.0.1").toString(),
                quint16(QSettings().value("digi/port", 7362).toUInt()));
            digiWin_ = new DigiWindow(fldigi_, this);
        }
        digiWin_->show();
        digiWin_->raise();
        digiWin_->activateWindow();
    });

}

void MainWindow::setupRotorUi() {
    // "ROT" dropdown: antenna rotator through rotctld (:4533) — the rose is
    // the pointing device. Clicking a spot (or the rose) sets the target;
    // TURN sends it, or auto-follow turns on every point. The cyan needle
    // on the rose is the antenna's actual heading.
    auto* rotBtn = new QToolButton(topStrip_);
    rotBtn->setText("ROT ▾");
    rotBtn->setPopupMode(QToolButton::InstantPopup);
    rotBtn->setFocusPolicy(Qt::NoFocus);
    rotBtn->setStyleSheet(QString(kToolBtnStyle));
    rotBtn->setToolTip("Antenna rotator via rotctld :4533\n(run rotctld -m "
                       "<model> -r <device>; the compass rose shows the "
                       "antenna as a cyan needle)");
    auto* rotMenu = new QMenu(rotBtn);
    styleMenu(rotMenu);
    auto* rotOn = rotMenu->addAction("Connect to rotctld");
    rotOn->setCheckable(true);
    auto* rotFollow = rotMenu->addAction("Antenna follows the rose");
    rotFollow->setCheckable(true);
    rotFollow->setToolTip("Every rose point (incl. spot clicks) turns the "
                          "rotator immediately.\nOff: manual rose clicks "
                          "still turn; spot clicks point only —\n"
                          "Ctrl+click a callsign to tune AND turn.");
    rotMenu->addSeparator();
    auto* rotW = new QWidget;
    rotW->setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 13px; }"
        "QLabel { color: #8fa3b8; font-weight: bold; font-size: 12px; }"
        "QSpinBox { background: #1c2430; color: #c8d4e0; border: 1px solid"
        " #2a3644; border-radius: 3px; padding: 2px 6px; }"
        "QPushButton { background: #2f6d9e; border: 1px solid #5db2f0;"
        " border-radius: 3px; padding: 5px 14px; font-weight: bold; }"
        "QPushButton:hover { background: #3a7fb5; }"
        "QPushButton#stop { background: #8a2727; border-color: #e05d5d; }");
    auto* rg = new QGridLayout(rotW);
    rg->setContentsMargins(14, 12, 14, 12);
    rg->setHorizontalSpacing(10);
    rg->setVerticalSpacing(9);
    auto* rotState = new QLabel("rotor: not connected", rotW);
    rg->addWidget(rotState, 0, 0, 1, 3);
    auto* rotTurn = new QPushButton("TURN to rose", rotW);
    rotTurn->setToolTip("Send the rotator to the rose's red pointer");
    rg->addWidget(rotTurn, 1, 0, 1, 2);
    auto* rotStop = new QPushButton("STOP", rotW);
    rotStop->setObjectName("stop");
    rg->addWidget(rotStop, 1, 2);
    rg->addWidget(new QLabel("MANUAL", rotW), 2, 0);
    auto* rotAz = new QSpinBox(rotW);
    rotAz->setRange(0, 359);
    rotAz->setSuffix("°");
    rotAz->setWrapping(true);
    rg->addWidget(rotAz, 2, 1);
    auto* rotGo = new QPushButton("GO", rotW);
    rg->addWidget(rotGo, 2, 2);
    auto* rotAct = new QWidgetAction(rotMenu);
    rotAct->setDefaultWidget(rotW);
    rotMenu->addAction(rotAct);
    rotBtn->setMenu(rotMenu);
    topLay2_->addSpacing(8);
    topLay2_->addWidget(rotBtn);
    const auto rotRefresh = [this, rotState] {
        QString s = rotor_.connected()
            ? (rotor_.azimuth() >= 0.0
                   ? QString("rotor: %1°").arg(qRound(rotor_.azimuth()))
                   : QString("rotor: connected"))
            : QString("rotor: not connected");
        if (lastRoseBearing_ >= 0.0)
            s += QString("   target: %1 %2°")
                     .arg(lastRoseLabel_)
                     .arg(qRound(lastRoseBearing_));
        rotState->setText(s);
    };
    rotor_.setEndpoint(
        QSettings().value("rotor/host", "127.0.0.1").toString(),
        quint16(QSettings().value("rotor/port", 4533).toUInt()));
    rotOn->setChecked(QSettings().value("rotor/enabled", false).toBool());
    rotFollow->setChecked(QSettings().value("rotor/track", false).toBool());
    rotor_.setActive(rotOn->isChecked());
    connect(rotOn, &QAction::toggled, this, [this, rotRefresh](bool on) {
        QSettings().setValue("rotor/enabled", on);
        rotor_.setActive(on);
        rotRefresh();
    });
    connect(rotFollow, &QAction::toggled, this, [](bool on) {
        QSettings().setValue("rotor/track", on);
    });
    connect(&rotor_, &RotorClient::azimuthChanged, this,
            [this, rotRefresh](double az) {
                pan_->setRotorAz(az);
                rotRefresh();
            });
    connect(&rotor_, &RotorClient::connectedChanged, this,
            [this, rotRefresh](bool on) {
                statusBar()->showMessage(
                    on ? "rotor: rotctld connected" : "rotor: link lost",
                    4000);
                rotRefresh();
            });
    connect(pan_, &PanadapterWidget::roseBearingChanged, this,
            [this, rotFollow, rotRefresh](double b, const QString& label) {
                lastRoseBearing_ = b;
                lastRoseLabel_ = label;
                // A MANUAL rose click is an order, not a suggestion —
                // it turns immediately (operator: "make it go after I set
                // it instead of opening the panel"). Spot points still
                // only turn with auto-follow on, or via Ctrl+click.
                const bool go = b >= 0.0 && rotor_.connected()
                    && (rotFollow->isChecked()
                        || label == QLatin1String("MAN"));
                if (go) {
                    rotor_.turnTo(b);
                    statusBar()->showMessage(
                        QString("rotor -> %1° (%2)").arg(qRound(b))
                            .arg(label), 4000);
                }
                rotRefresh();
            });
    // Ctrl+click on a spot callsign: tune AND turn (optional per click —
    // never automatic; plain click stays tune-only).
    connect(pan_, &PanadapterWidget::spotTurnRequested, this,
            [this, rotRefresh](double b, const QString& call) {
                if (b < 0.0) {
                    statusBar()->showMessage(
                        QString("no bearing for %1 (location unknown)")
                            .arg(call), 4000);
                    return;
                }
                if (!rotor_.connected()) {
                    statusBar()->showMessage(
                        "rotor: not connected (ROT menu)", 4000);
                    return;
                }
                rotor_.turnTo(b);
                statusBar()->showMessage(
                    QString("rotor -> %1° (%2)").arg(qRound(b)).arg(call),
                    5000);
                rotRefresh();
            });
    connect(rotTurn, &QPushButton::clicked, this, [this, rotRefresh] {
        if (lastRoseBearing_ < 0.0) {
            statusBar()->showMessage(
                "rotor: point the rose first (click a spot or the rose)",
                4000);
            return;
        }
        rotor_.turnTo(lastRoseBearing_);
        statusBar()->showMessage(QString("rotor -> %1° (%2)")
                                     .arg(qRound(lastRoseBearing_))
                                     .arg(lastRoseLabel_), 4000);
        rotRefresh();
    });
    connect(rotStop, &QPushButton::clicked, this,
            [this] { rotor_.stop(); statusBar()->showMessage("rotor: STOP", 3000); });
    connect(rotGo, &QPushButton::clicked, this, [this, rotAz] {
        rotor_.turnTo(rotAz->value());
        statusBar()->showMessage(
            QString("rotor -> %1°").arg(rotAz->value()), 4000);
    });
    connect(rotMenu, &QMenu::aboutToShow, this, rotRefresh);

}

} // namespace ttc
