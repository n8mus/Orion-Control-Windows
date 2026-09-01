// SPDX-License-Identifier: GPL-2.0-or-later
// The second-deck tool buttons, one self-contained builder per feature.
// Each runs once from the MainWindow constructor; new tools get a new
// function here (and a one-line call there) — buttons go on topLay2_,
// never the first deck, or the window minimum outgrows the screen.
#include "app/MainWindow.h"
#include <QKeyEvent>
#include <QCheckBox>
#include "cw/AudioCwSource.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"
#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"
#include "cw/SkimServer.h"
#include "cw/SkimStft.h"
#include "cw/WinKeyer.h"
#include "net/FldigiClient.h"
#include "ui/DigiWindow.h"
#include "log/QslUploader.h"
#include "ui/GlobeWindow.h"
#include "ui/LogWindow.h"
#include "ui/LogbookWindow.h"
#include "ui/SpotTableWindow.h"
#include "ui/SkimmerWindow.h"
#include "ui/SkimViewWindow.h"
#include "ui/WinKeyerPanel.h"

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

// The big tool windows (CW, band map, SKIM waterfall, DIGI) get parked on
// other monitors in a shack. On Windows an OWNED window has no taskbar
// button and rides its owner's z-order, and dragging one across monitors
// with different DPI recreates the native window — live-found 2026-08-31
// on the all-in-one + second display: both windows stranded invisible
// behind the maximized console ("they are gone"). Parentless top-levels
// are the Windows convention for multi-window ham software (N1MM-style:
// own taskbar entry, free stacking on any monitor). Linux keeps the
// transient-for parent the operator's WM already handles well. The pair
// below implements the split: parent choice + the quit/lifetime fixups a
// parentless window needs (MainWindow's destructor deletes them).
static QWidget* toolWinParent(QWidget* mainWin) {
#ifdef Q_OS_WIN
    Q_UNUSED(mainWin);
    return nullptr;
#else
    return mainWin;
#endif
}
static void adoptToolWindow(QWidget* w) {
#ifdef Q_OS_WIN
    // The console closing must still quit the app even while a parentless
    // tool window is open.
    w->setAttribute(Qt::WA_QuitOnClose, false);
#else
    Q_UNUSED(w);
#endif
}

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
    auto* logBtn = new QToolButton(topStrip_);
    logBtn->setText("LOG");
    logBtn->setFocusPolicy(Qt::NoFocus);
    logBtn->setStyleSheet(QString(kToolBtnStyle));
    // Label stays "LOG" (operator call 2026-07-31: relabeling "messes up the
    // button"); the hover text carries the truth instead.
    logBtn->setToolTip("New QSO — the console's log entry window\n"
                       "(on Linux it also pops a fresh New QSO in cqrlog)\n"
                       "Right-click: logbook browser — search, edit, ADIF");
    topLay2_->addWidget(logBtn);
    logUdp_ = new QUdpSocket(this);
    // Pre-bind so the first datagram isn't lost (an unbound socket auto-binds
    // on first write and can drop that very first send).
    logUdp_->bind(QHostAddress::LocalHost, 0);
    // LOG -> the console's own New QSO window (SQLite station log), and the
    // same nudge to cqrlog as always — on the Linux box both stay in step,
    // on Windows the datagram lands on deaf ears by design (fire-and-forget).
    connect(logBtn, &QToolButton::clicked, this, [this] {
        openLogWindow();
        logUdp_->writeDatagram("CQRNEWQSO", QHostAddress::LocalHost,
            quint16(QSettings().value("log/port", 2334).toInt()));
        // "Fresh QSO" has to mean fresh on BOTH sides: cqrlog's form goes
        // blank, so the console's DX box can't keep pointing at the
        // station just worked (operator had to clear it by hand).
        if (cwWin_) cwWin_->setHisCall(QString());
    });
    logBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(logBtn, &QToolButton::customContextMenuRequested, this,
            [this](const QPoint&) { openLogbookWindow(); });
    // Spot click -> send the call (and POTA park/grid) to cqrlog's New QSO,
    // and pre-fill the console's own entry window when it's open.
    connect(pan_, &PanadapterWidget::spotClicked, this,
            [this](const QString& call, QChar kind, const QString& tg) {
                if (cwWin_) cwWin_->setHisCall(call);
                QString park, grid;
                if (kind == QChar('P')) {
                    park = tg;
                    for (const Spot& s : potaClient_.spots())
                        if (s.call == call) { grid = s.grid; break; }
                }
                sendCqrLookup(call, park, grid);
            });

}

namespace {
// The rig's mode as the ADIF mode the logbook stores.
QString adifModeText(Mode m) {
    switch (m) {
        case Mode::CWU: case Mode::CWL: return QStringLiteral("CW");
        case Mode::USB: case Mode::LSB: return QStringLiteral("SSB");
        case Mode::AM:  return QStringLiteral("AM");
        case Mode::FM:  return QStringLiteral("FM");
    }
    return QStringLiteral("SSB");
}
} // namespace

void MainWindow::openLogWindow(const QString& call, const QString& park,
                               const QString& grid) {
    if (!logWin_) {
        logWin_ = new LogWindow(logDb_, &logbook_, &cty_, &rotor_, qrz_,
                                toolWinParent(this));
        adoptToolWindow(logWin_);
        // Rose and globe follow whoever is in the window: country center
        // from cty.dat, sharpened to the real grid when WSJT-X or a QRZ
        // lookup provides one.
        connect(logWin_, &LogWindow::dxLocated, this,
                [this](double lat, double lon, const QString& call) {
                    pan_->pointRoseAt(lat, lon, call);
                    if (globeWin_ && globeWin_->isVisible()) {
                        double la = 0, lo = 0;
                        CtyLookup::gridToLatLon(
                            QSettings().value("station/grid", "EN83al")
                                .toString(), la, lo);
                        globeWin_->setStations(la, lo, lat, lon, call);
                    }
                });
        connect(logWin_, &LogWindow::globeRequested, this,
                [this] { openGlobeWindow(); });
        connect(logWin_, &LogWindow::qsoLogged, this,
                [this](qint64 id, const QString& c) {
                    statusBar()->showMessage("logged " + c, 3000);
                    if (cwWin_) cwWin_->setHisCall(QString());
                    if (uploader_) uploader_->pushQso(id);
                });
        // Dial and mode ride in once a second while the window is up — every
        // tune path (knob, band button, WSJT-X, click) funnels into
        // centerHz_/rigMode_, so polling beats hooking each one.
        auto* feed = new QTimer(logWin_);
        feed->setInterval(1000);
        connect(feed, &QTimer::timeout, logWin_, [this] {
            if (logWin_->isVisible())
                logWin_->setRig(qint64(centerHz_), adifModeText(rigMode_));
        });
        feed->start();
    }
    logWin_->setRig(qint64(centerHz_), adifModeText(rigMode_));
    if (!call.isEmpty()) logWin_->prefill(call, park, grid);
    logWin_->show();
    logWin_->raise();
    logWin_->activateWindow();
}

void MainWindow::openLogbookWindow() {
    if (!logbookWin_) {
        logbookWin_ = new LogbookWindow(logDb_, &cty_, uploader_,
                                        toolWinParent(this));
        adoptToolWindow(logbookWin_);
    }
    logbookWin_->show();
    logbookWin_->raise();
    logbookWin_->activateWindow();
}

void MainWindow::openGlobeWindow() {
    if (!globeWin_) {
        globeWin_ = new GlobeWindow(toolWinParent(this));
        adoptToolWindow(globeWin_);
    }
    globeWin_->show();
    globeWin_->raise();
    // Seed it with whatever the LOG window is showing right now.
    if (logWin_) logWin_->announceDx();
}

void MainWindow::openSpotTable() {
    if (!spotTable_) {
        spotTable_ = new SpotTableWindow(&logbook_, &rotor_,
                                         toolWinParent(this));
        adoptToolWindow(spotTable_);
        // Double-click = the band-map spot click, plus the QSY the band map
        // can't do (a table row may live on another band entirely).
        connect(spotTable_, &SpotTableWindow::spotActivated, this,
                [this](const QString& call, qint64 hz, QChar kind,
                       const QString& tag) {
                    if (hz > 0) radio_->setFrequencyHz(Rx::Main, hz);
                    if (cwWin_) cwWin_->setHisCall(call);
                    QString park, grid;
                    if (kind == QChar('P')) {
                        park = tag;
                        for (const Spot& s : potaClient_.spots())
                            if (s.call == call) { grid = s.grid; break; }
                    }
                    sendCqrLookup(call, park, grid);
                });
    }
    spotTable_->show();
    spotTable_->raise();
    spotTable_->activateWindow();
}

void MainWindow::sendCqrLookup(const QString& call, const QString& park,
                               const QString& grid) {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty() || !logUdp_) return;
    // Every path that feeds a call here (spot click, CW decode double-click,
    // skimmer windows) also pre-fills the console's own entry window.
    if (logWin_ && logWin_->isVisible()) logWin_->prefill(c, park, grid);
    QString msg = "CQRLOOKUP:" + c;
    if (!park.isEmpty()) msg += ";PARK:" + park.trimmed().toUpper();
    if (!grid.isEmpty()) msg += ";GRID:" + grid.trimmed();
    // Fire-and-forget on purpose: a click shouldn't nag when cqrlog is
    // closed — the LOG button already does the is-anyone-listening probe.
    logUdp_->writeDatagram(
        msg.toUtf8(), QHostAddress::LocalHost,
        quint16(QSettings().value("log/port", 2334).toInt()));
}

void MainWindow::setupCwUi() {
    // "CW" button: the WinKeyer sending window (type-ahead + memories).
    // The keyer hardware keeps the paddle in charge; this is the keyboard.
    auto* cwBtn = new QToolButton(topStrip_);
    cwBtn->setText("CW");
    cwBtn->setFocusPolicy(Qt::NoFocus);
    cwBtn->setStyleSheet(QString(kToolBtnStyle));
    cwBtn->setToolTip("CW keyboard/memories via the WinKeyer\n(paddle always "
                      "wins — touching it dumps the buffer)\n"
                      "Right-click: WinKeyer control (weighting, key "
                      "compensation, ratio…)");
    topLay2_->addSpacing(8);
    topLay2_->addWidget(cwBtn);
    connect(cwBtn, &QToolButton::clicked, this, [this] {
        if (!cwWin_) {
            cwWin_ = new CwWindow(radio_, toolWinParent(this));
            adoptToolWindow(cwWin_);
            cwWin_->setMyCall(QSettings()
                .value("station/callsign", "N8EM").toString());
            cwWin_->setHisCall(QString());
            // Double-clicked call in the decode pane rides the same rails
            // as a spot click: send it to cqrlog's New QSO, arm the %c macro.
            connect(cwWin_, &CwWindow::callDoubleClicked, this,
                    [this](const QString& call) {
                        cwWin_->setHisCall(call);
                        sendCqrLookup(call);
                        statusBar()->showMessage(
                            QString("cqrlog ← %1 (from CW copy)")
                                .arg(call), 5000);
                    });
            // Same rails for a call typed by hand in the DX box: the
            // console is the master of the callsign, cqrlog follows.
            // (There is no path the other way — cqrlog's bridge is
            // receive-only, so typing it THERE reaches nothing here.)
            connect(cwWin_, &CwWindow::hisCallEntered, this,
                    [this](const QString& call) {
                        sendCqrLookup(call);
                        statusBar()->showMessage(
                            QString("cqrlog ← %1 (typed)").arg(call), 5000);
                    });
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
                audioSrc_->setTargetPitch(pitch);   // never notch the target
                connect(audioDec_, &CwDecoder::textDecoded,
                        cwWin_, &CwWindow::appendRx, Qt::QueuedConnection);
                connect(audioDec_, &CwDecoder::wpmEstimated,
                        cwWin_, &CwWindow::setRxWpm, Qt::QueuedConnection);
                connect(audioSrc_, &AudioCwSource::pitchMeasured,
                        cwWin_, &CwWindow::setRxPitch);
                connect(audioSrc_, &AudioCwSource::pitchMeasured, this,
                        [this](double hz) {
                            lastPitchHz_ = hz;
                            lastPitchMs_ =
                                QDateTime::currentMSecsSinceEpoch();
                            pitchTrimFeed(hz);
                        });
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
                // then live-follow the window's controls. These knobs steer
                // the tuned reader (and its audio twin) only — the skimmer's
                // channels run the engine at its defaults (SOM off; see
                // SkimmerEngine's constructor for why).
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
                const auto applySql = [this](int sql) {
                    for (CwDecoder* d : {cwDec_, audioDec_})
                        d->setSquelch(double(sql));
                };
                QSettings cs;
                applyCfg(cs.value("cw/engine", true).toBool(),
                         cs.value("cw/som", true).toBool(),
                         cs.value("cw/deep", false).toBool(),
                         cs.value("cw/attack", 1).toInt(),
                         cs.value("cw/decay", 1).toInt());
                applySql(cs.value("cw/squelch", 12).toInt());
                connect(cwWin_, &CwWindow::rxDecodeConfigChanged, this,
                        applyCfg);
                connect(cwWin_, &CwWindow::rxSquelchChanged, this, applySql);
            }
            wireRigCwPanel();
        }
        cwWin_->show();
        cwWin_->raise();
        cwWin_->activateWindow();
    });

    // Right-click opens the WinKeyer control box. It is a right-click and
    // not a sixth button because the top strip has ~2 px of slack against
    // the width budget, and losing the maximize button has bitten twice —
    // same reason the SWR menu hangs off a right-click on TUNE.
    cwBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(cwBtn, &QToolButton::customContextMenuRequested, this,
            [this, cwBtn](const QPoint&) {
        // Reuse the left-click path wholesale so the keyer gets built and
        // opened exactly the way it always is.
        if (!cwWin_) cwBtn->click();
        auto* wk = qobject_cast<ttc::WinKeyer*>(
            cwWin_ ? cwWin_->keyer() : nullptr);
        if (!wk) {
            statusBar()->showMessage(
                "WinKeyer control: the WinKeyer is not the active keyer — "
                "the radio's own keyer has no host-settable element timing "
                "(SDR ▸ Station setup… to switch)", 8000);
            return;
        }
        if (!wkPanel_) {
            auto* p = new ttc::WinKeyerPanel(wk, this);
            p->setAttribute(Qt::WA_DeleteOnClose);
            wkPanel_ = p;
        }
        wkPanel_->show();
        wkPanel_->raise();
        wkPanel_->activateWindow();
    });
}

// The CW window's "RADIO — CW" panel: the rig's own sidetone level and
// pitch, QSK delay and envelope, over CAT. Rig-side settings, so they
// apply with the WinKeyer doing the keying — which is why they live in
// the keyer window rather than a setup dialog.
void MainWindow::wireRigCwPanel() {
    if (!cwWin_ || !radio_) return;
    cwWin_->setRigCwAvailable(radio_->caps().catCwControls
                              && radio_->connected());

    connect(cwWin_, &CwWindow::rigSidetoneVolChanged, this,
            [this](int v) { radio_->setCwSidetoneVol(v); });
    connect(cwWin_, &CwWindow::rigQskDelayChanged, this,
            [this](int v) { radio_->setCwQskDelay(v); });
    connect(cwWin_, &CwWindow::rigAttackDecayChanged, this,
            [this](int v) { radio_->setCwAttackDecay(v); });
    connect(cwWin_, &CwWindow::rigSidetonePitchChanged, this, [this](int hz) {
        radio_->setCwSidetonePitch(hz);
        applyCwPitch(hz);                  // follow our own change at once
    });

    connect(radio_, &RadioController::cwSidetoneVolReported,
            cwWin_, &CwWindow::showRigSidetoneVol);
    connect(radio_, &RadioController::cwQskDelayReported,
            cwWin_, &CwWindow::showRigQskDelay);
    connect(radio_, &RadioController::cwAttackDecayReported,
            cwWin_, &CwWindow::showRigAttackDecay);
    connect(radio_, &RadioController::cwKeyerSpeedReported,
            cwWin_, &CwWindow::showRigKeyerSpeed);
    connect(radio_, &RadioController::cwKeyerWeightReported,
            cwWin_, &CwWindow::showRigKeyerWeight);
    connect(radio_, &RadioController::cwKeyerEnabledReported,
            cwWin_, &CwWindow::showRigKeyerEnabled);
    connect(radio_, &RadioController::cwSidetonePitchReported, this,
            [this](int hz) {
                cwWin_->showRigSidetonePitch(hz);
                applyCwPitch(hz);          // and follow the FRONT PANEL
            });
    radio_->queryCw();                     // fill the panel on first open
}

// The radio's sidetone pitch is the truth; cw/pitchHz is only our cache
// of it. Everything that assumed 550 now follows whatever the rig says —
// which is what makes the reader work for an operator who isn't Jon.
void MainWindow::applyCwPitch(int hz) {
    if (hz < 300 || hz > 1200) return;
    if (QSettings().value("cw/pitchHz", 550).toInt() == hz) return;
    QSettings().setValue("cw/pitchHz", hz);
    // The audio-path decoder mixes the sidetone down to DC, so its offset
    // IS the pitch — retune() is atomic and the capture thread applies it
    // at the next block. The SDR-path decoder is carrier-at-dial and the
    // skimmer hops its own channels, so neither cares. Zero-beat and the
    // Hz readout re-read the setting every time they run.
    if (audioDec_) audioDec_->retune(double(hz));
    if (audioSrc_) audioSrc_->setTargetPitch(hz);   // move the notch guard
    statusBar()->showMessage(
        QString("CW sidetone %1 Hz — reader and 0-beat follow").arg(hz), 4000);
}

void MainWindow::setupSkimUi(const QString& stationCall) {
    // "SKIM" dropdown: the CW skimmer — a bank of decoder channels parked on
    // the strongest signals in the band's CW segment, mining the decoded
    // text for callsigns. Found calls land on the panadapter as violet
    // spots, worked-before colored like everything else. The engine exists
    // even without an SDR build; it just never gets IQ.
    skim_ = new SkimmerEngine(                     // rate must match the SDR
        double(kSdrCaptureHz),
        std::clamp(QSettings().value("skim/channels", 24).toInt(), 4, 64),
        this);
    skim_->setCallValidator([this](const QString& call) {
        double la = 0.0, lo = 0.0;                 // decode artifacts have
        return cty_.lookup(call, la, lo);          // no country prefix
    });
    skim_->setKnownCalls(loadMasterScp());
    // The SKIM view's fast STFT taps the IQ stream in MainWindow's
    // iqHandler; it costs one branch per block until the window opens.
    skimStft_ = new SkimStft(double(kSdrCaptureHz), this);
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
    // Bayes brain A/B: matrix-verified, but defaults only flip after the
    // operator's on-air verdict (house rule) — so it's a plain toggle.
    auto* skimBayes = skimMenu->addAction("Bayes decoder (experimental)");
    skimBayes->setCheckable(true);
    skimBayes->setChecked(QSettings().value("skim/bayes", false).toBool());
    skimBayes->setToolTip(
        "Decode the skimmer channels with the Bayesian brain (soft keying\n"
        "+ whole-character inference) instead of the fldigi engine. Try\n"
        "it on the air and compare the finds.");
    connect(skimBayes, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("skim/bayes", on);
        skim_->setBayes(on);
    });
    if (skimBayes->isChecked()) skim_->setBayes(true);
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
                toolWinParent(this));
            adoptToolWindow(skimWin_);
            connect(skimWin_, &SkimmerWindow::tuneTo, this,
                    [this](qint64 hz, const QString& call) {
                        tuneAbsolute(uint64_t(hz));
                        if (!call.isEmpty()) {
                            if (cwWin_) cwWin_->setHisCall(call);
                            sendCqrLookup(call);
                        }
                    });
        }
        skimWin_->show();
        skimWin_->raise();
        skimWin_->activateWindow();
    });
    // Waterfall: the CW-Skimmer-style view — keying visible per station,
    // decoded calls riding the traces.
    auto* skimView = skimMenu->addAction("Waterfall view…");
    skimView->setToolTip(
        "High-resolution waterfall of the CW segment: every station's dits\n"
        "and dahs visible, decoded callsigns next to their traces. Click a\n"
        "trace to tune, wheel to zoom.");
    connect(skimView, &QAction::triggered, this, &MainWindow::openSkimView);
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
                                      qint64(centerHz_), loOffHz_);
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

void MainWindow::openSkimView() {
    if (!skimView_) {
        skimView_ = new SkimViewWindow(
            skimStft_, skim_,
            [this](const QString& call, qint64 hz) {
                if (!logbook_.ready()) return QChar('?');
                return logbook_.status(call, LogbookIndex::bandForHz(hz));
            },
            [this](qint64& dial, int& loOff) {
                dial = qint64(centerHz_);
                loOff = loOffHz_;
            },
            toolWinParent(this));
        adoptToolWindow(skimView_);
        connect(skimView_, &SkimViewWindow::tuneTo, this,
                [this](qint64 hz, const QString& call) {
                    tuneAbsolute(uint64_t(hz));
                    if (!call.isEmpty()) {
                        if (cwWin_) cwWin_->setHisCall(call);
                        sendCqrLookup(call);
                    }
                });
    }
    skimView_->show();
    skimView_->raise();
    skimView_->activateWindow();
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
            digiWin_ = new DigiWindow(fldigi_, toolWinParent(this));
            adoptToolWindow(digiWin_);
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
    rotor_.configure();
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
    connect(&rotor_, &RotorLink::azimuthChanged, this,
            [this, rotRefresh](double az) {
                pan_->setRotorAz(az);
                rotRefresh();
            });
    connect(&rotor_, &RotorLink::connectedChanged, this,
            [this, rotRefresh](bool on) {
                statusBar()->showMessage(
                    on ? (rotor_.direct() ? "rotor: DCU-3 answering"
                                          : "rotor: rotctld connected")
                       : "rotor: link lost",
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
