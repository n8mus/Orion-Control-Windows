// SPDX-License-Identifier: GPL-2.0-or-later
// MainWindow operating features: manual tune carrier, digital/voice audio
// switching, TX profiles, band-stack memories, US 60 m channels and the
// DVR/voice-keyer. Split out of MainWindow.cpp (see MainWindowInternal.h).
#include "app/MainWindow.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"
#include "ui/SmithChartWidget.h"
#include "ui/SwrHistoryDialog.h"

#include <QVBoxLayout>

#include <QActionGroup>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimeEdit>
#include <QTimeZone>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ttc {

// Manual tune: the CAT set has no "tune button" command (*TTT needs the
// internal tuner enabled), so reproduce it: steady carrier at the set watts
// via FM mode + key, with the previous power/mode restored afterwards.
void MainWindow::startManualTune() {
    if (tuning_) return;
    tuning_ = true;
    preTunePwr_  = lastTxPwr_;
    preTuneMode_ = rigMode_;
    int level = txBar_->tuneLevel();
    if (txBar_->ampMode()) level = std::min(level, txBar_->ampLimit());
    radio_->setTxPower(level);
    if (rigMode_ != Mode::FM) radio_->setMode(Rx::Main, Mode::FM);
    radio_->setPtt(true);
    tuneTimeout_->start();
    statusBar()->showMessage(
        QString("TUNE: %1 W carrier — click again to stop (auto-stop 15 s)").arg(level));
}

// Over-the-air playback (voice keyer or retransmit). The clip goes in on the
// rig's rear line input, so ride the same source swap the DIG button does:
// line-in gain up / mic+proc parked, key, then start the audio only after the
// gain commands have had time to land (no clipped first syllable). dvrStopped
// unwinds it all — un-key first, then voice settings back.
void MainWindow::dvrPlayOverAir(const QString& wav, int slot) {
    if (radioDevUsed_.startsWith("udp:")) {
        // Remote: an Ethernet-keyed radio takes its TX audio ONLY from the
        // TRIP stream — no SignaLink anywhere in the path. Play the clip
        // into the TRIP_digital sink and point the TRIP capture at its
        // monitor for this playback (TripAudio re-reads tripSource at every
        // key, so this is live); dvrStopped restores the operator's source.
        QSettings s;
        dvrTripSrcSave_ = s.value("radio/tripSource").toString();
        s.setValue("radio/tripSource", "TRIP_digital.monitor");
        dvrAutoDig_ = false;                       // no line-in gain dance
        dvrTxPlayback_ = true;
        radio_->setPtt(true);                      // TRIP starts on the monitor
        txBar_->showDvrPlaying(slot);
        QTimer::singleShot(300, this, [this, wav] {
            if (!dvrTxPlayback_) return;           // aborted while arming
            dvr_->play(wav, QStringLiteral("TRIP_digital"));
        });                                        // finished() -> dvrStopped
        return;
    }
    dvrAutoDig_ = !digital_;                       // already in DIG (FT8)? leave it
    if (dvrAutoDig_) setDigitalMode(true);
    dvrTxPlayback_ = true;
    radio_->setPtt(true);
    txBar_->showDvrPlaying(slot);
    QTimer::singleShot(300, this, [this, wav] {
        if (!dvrTxPlayback_) return;               // aborted while arming
        dvr_->play(wav, radioSink_);               // a start failure lands in
    });                                            // finished() -> dvrStopped
}

// The clip deck went idle — a take or playback ended on its own or by a
// stop-click. Un-key if this playback held PTT (after a short drain so the
// sink empties: pw-play exits at its last buffer write, not the last sample),
// then hand the audio source back to the mic.
void MainWindow::dvrStopped() {
    if (dvrTxPlayback_) {
        dvrTxPlayback_ = false;
        // Drain before un-key: the player exits at its last buffer WRITE,
        // not the last sample. The serial/SignaLink path empties in ~250 ms;
        // the Ethernet path is longer (paplay server buffer -> TRIP_digital
        // -> parec -> TRIP pacing queue -> radio) and un-keying also flushes
        // TripAudio's queue — too short a drain chops the clip's tail.
        const int drainMs = radioDevUsed_.startsWith("udp:") ? 1200 : 250;
        QTimer::singleShot(drainMs, this, [this] {
            radio_->setPtt(false);                 // un-key BEFORE the mic is hot
            if (dvrAutoDig_) {
                dvrAutoDig_ = false;
                setDigitalMode(false);             // voice mic/proc come back
            }
            if (!dvrTripSrcSave_.isEmpty()) {      // remote clip: mic back on TRIP
                QSettings().setValue("radio/tripSource", dvrTripSrcSave_);
                dvrTripSrcSave_.clear();
            }
        });
    }
    // A fresh take gets peak-normalized on the way in: the radio's line out
    // and a mic both record tens of dB below full scale, which played back
    // as almost no TX drive. Normalizing the file (not the playback) means
    // what you audition on the speakers is exactly what goes over the air.
    if (!dvrJustRecorded_.isEmpty()) {
        ClipDeck::normalizeWav(dvrJustRecorded_);
        dvrJustRecorded_.clear();
    }
    txBar_->showDvrIdle();
    for (int i = 0; i < 4; ++i)                    // a VK record may have landed
        txBar_->setVkLoaded(i, QFileInfo::exists(vkPath(i)));
    statusBar()->showMessage("DVR: stopped");
}

QString MainWindow::dvrDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/dvr";
}

QString MainWindow::vkPath(int slot) const {
    return dvrDir() + QString("/vk%1.wav").arg(slot + 1);
}

void MainWindow::stopManualTune() {
    if (!tuning_) return;
    tuning_ = false;
    tuneTimeout_->stop();
    radio_->setPtt(false);                          // drop the carrier FIRST
    if (preTuneMode_ != Mode::FM) applyMode(preTuneMode_);
    radio_->setTxPower(preTunePwr_);
    txBar_->showTxPower(preTunePwr_);
    txBar_->showTuneActive(false);
    statusBar()->showMessage("TUNE: carrier off, power and mode restored");
}

// ---- SWR sweep (right-click TUNE) -----------------------------------------
// Steps the visible span at the TUNE watts, reading SWR at each stop from
// the external LP-100A wattmeter when the operator has one, otherwise from
// the radio's own @STF, and paints the curve on the pan (bright = newest run,
// one older run kept dim for antenna before/after). Operator-attended by
// design: started only from the TUNE context menu, refuses with the AMP
// enabled or on 60 m (channelized — no TX between channels), aborts on any
// pan click, ESC, the TUNE button or the carrier failsafe, and always
// restores mode, power and the dial. It keys ONE continuous low-power
// carrier and steps the dial while keyed — no T/R relay chatter. The view
// is held on the swept range with the same frame the band overview uses.

// Step count follows the swept span instead of being fixed, so a narrow
// view sweep is not wastefully slow and a whole-band sweep on 10 m or 6 m
// is not uselessly blocky. Every step costs a keyed-carrier second, so the
// cap is a duty-cycle and patience limit, not a resolution one: 4 MHz of
// 6 m cannot be swept finely by keying a carrier at each stop, and asking
// for it would mean a minutes-long transmission.
static constexpr int64_t kSwrTargetHzPerStep = 5000;
static constexpr int kSwrMinSteps = 24;   // narrow spans still get detail
static constexpr int kSwrMaxSteps = 96;   // ~44 s of carrier — the ceiling

// Settle time after each dial move before a reading counts. The FIRST
// step needs far more than the rest, and the reason is not the dial:
// startManualTune() brings the carrier up COLD, so step 0 is a PA ramp
// plus ALC settling on top of the tune, while every later step is a dial
// nudge with the carrier already hot and steady. On top of that, a
// meter reading's timestamp is when its FRAME ARRIVED, not when the RF
// was sampled — both meters average internally, so a frame arriving just
// after arming can still carry an average blended with the pre-carrier
// state. Mid-sweep that is harmless (the neighbouring frequency reads
// about the same); at step 0 it blends real RF with NO RF, which is
// exactly the false first point the operator saw on a live 40 m sweep
// (2026-08-23). Cure is time, not arithmetic.
static constexpr int kSwrSettleMs      = 300;
static constexpr int kSwrFirstSettleMs = 1200;
static constexpr int kSwrTickMs        = 150;   // sweep state-machine period

// Never park the carrier exactly on a band edge: a whole-band sweep used
// to start at 7.000000 dead on, where the occupied bandwidth spills below
// the edge. Nudge both ends inside by this much — invisible on a curve
// hundreds of kHz wide.
static constexpr int64_t kSwrEdgeMarginHz = 1000;

// The external meter is believed only when it is enabled, chosen as the
// source, AND answering right now. Everything else falls through to @STF,
// which is what a station without an LP-100A always gets.
bool MainWindow::meterSwrReady() const {
    return txMeter_ && txMeter_->isAlive()
           && QSettings().value("swr/source", "meter").toString() != "radio";
}

// The operator's meter is configured and its port opened, but it has never
// delivered a frame — powered down, unplugged, or the shared DB9 cable is
// on the other wattmeter. This is the case a bare "reading the radio"
// hides, and the one that makes a radio curve look like a meter curve.
// Choosing the radio deliberately (swr/source=radio) is not a fault.
bool MainWindow::meterConfiguredButSilent() const {
    return txMeter_ && !txMeter_->isAlive()
           && QSettings().value("swr/source", "meter").toString() != "radio";
}

void MainWindow::showSwrMenu(const QPoint& globalPos) {
    auto* m = new QMenu(this);
    m->setAttribute(Qt::WA_DeleteOnClose);
    styleMenu(m);
    // Antenna picker — set BEFORE sweeping so the run gets tagged right;
    // the label always mirrors curAntenna_ so a glance at the menu
    // confirms what the next sweep will be filed under.
    QMenu* antMenu = m->addMenu(
        QString("Antenna: %1")
            .arg(curAntenna_.isEmpty() ? "(unlabeled)" : curAntenna_));
    styleMenu(antMenu);
    auto* antGroup = new QActionGroup(antMenu);
    antGroup->setExclusive(true);
    QAction* unlabeled = antMenu->addAction("(unlabeled)");
    unlabeled->setCheckable(true);
    unlabeled->setChecked(curAntenna_.isEmpty());
    antGroup->addAction(unlabeled);
    connect(unlabeled, &QAction::triggered, this, [this] {
        curAntenna_.clear();
        QSettings().setValue("swr/currentAntenna", curAntenna_);
        refreshSwrOverlay();
    });
    for (const QString& name : QSettings().value("swr/antennas").toStringList()) {
        QAction* a = antMenu->addAction(name);
        a->setCheckable(true);
        a->setChecked(curAntenna_ == name);
        antGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, name] {
            curAntenna_ = name;
            QSettings().setValue("swr/currentAntenna", curAntenna_);
            refreshSwrOverlay();
        });
    }
    antMenu->addSeparator();
    QAction* newAnt = antMenu->addAction("New antenna…");
    connect(newAnt, &QAction::triggered, this, [this] {
        const QString typed = QInputDialog::getText(
            this, "New antenna", "Antenna name (e.g. \"40m dipole\"):");
        const QString name = typed.trimmed();
        if (name.isEmpty()) return;
        QSettings s;
        QStringList known = s.value("swr/antennas").toStringList();
        // Nudge consistent naming rather than silently forking history —
        // a near-duplicate almost always means "I forgot how I spelled
        // it last time", not "this is a genuinely different antenna".
        for (const QString& k : known)
            if (k.compare(name, Qt::CaseInsensitive) == 0 && k != name) {
                statusBar()->showMessage(
                    QString("SWR: using existing antenna \"%1\" instead "
                            "(close match)").arg(k), 6000);
                curAntenna_ = k;
                s.setValue("swr/currentAntenna", curAntenna_);
                refreshSwrOverlay();
                return;
            }
        if (!known.contains(name)) {
            known << name;
            s.setValue("swr/antennas", known);
        }
        curAntenna_ = name;
        s.setValue("swr/currentAntenna", curAntenna_);
        refreshSwrOverlay();
    });
    QAction* sweep = m->addAction(
        swrSweeping_ ? QStringLiteral("Stop SWR sweep")
                     : QStringLiteral("Sweep SWR across the view"));
    connect(sweep, &QAction::triggered, this, [this] {
        if (swrSweeping_) stopSwrSweep(false);
        else startSwrSweep();
    });
    // Edge to edge, ignoring the 480 kHz view cap — the pan paints what
    // it can see, the Smith chart gets all of it. Only offered where it
    // adds range the view sweep doesn't have (10 m, 6 m, 80 m).
    if (!swrSweeping_ && curBand_ >= 0
        && int64_t(kBands[curBand_].hiHz) - int64_t(kBands[curBand_].loHz)
               > pan_->viewSpanHz()) {
        QAction* whole = m->addAction(
            QStringLiteral("Sweep SWR across the whole band"));
        connect(whole, &QAction::triggered, this,
                [this] { startSwrSweep(true); });
    }
    QAction* show = m->addAction(QStringLiteral("Show SWR plots"));
    show->setCheckable(true);
    show->setChecked(QSettings().value("swr/show", true).toBool());
    connect(show, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("swr/show", on);
        pan_->setShowSwr(on);
    });
    // Smith chart — only for runs with impedance, i.e. LP-100A sweeps.
    QAction* smith = m->addAction(QStringLiteral("Smith chart…"));
    const QString curKey = curBand_ >= 0
        ? swrKey(curAntenna_, QLatin1String(kBands[curBand_].label))
        : QString();
    bool hasZ = false;
    if (curBand_ >= 0)
        for (const auto& run : swrRuns_.value(curKey))
            for (const auto& pt : run.pts)
                if (pt.zValid) { hasZ = true; break; }
    smith->setEnabled(hasZ);
    smith->setToolTip(hasZ ? QString()
                           : QStringLiteral("Needs a sweep taken with the "
                                            "LP-100A (R+jX per point)"));
    connect(smith, &QAction::triggered, this, [this] {
        if (curBand_ < 0) return;
        showSmithChart(curAntenna_, QLatin1String(kBands[curBand_].label));
    });
    QAction* hist = m->addAction(QStringLiteral("SWR history…"));
    connect(hist, &QAction::triggered, this, &MainWindow::showSwrHistory);
    QAction* clear =
        m->addAction(QStringLiteral("Clear this antenna's runs on this band"));
    clear->setEnabled(curBand_ >= 0 && !swrRuns_.value(curKey).isEmpty());
    connect(clear, &QAction::triggered, this, [this, curKey] {
        if (curBand_ < 0) return;
        swrRuns_.remove(curKey);
        saveSwrRuns();
        refreshSwrOverlay();
        statusBar()->showMessage("SWR: runs cleared for this antenna+band");
    });
    m->popup(globalPos);
}

void MainWindow::startSwrSweep(bool wholeBand) {
    if (swrSweeping_ || tuning_ || dvrTxPlayback_) {
        statusBar()->showMessage("SWR sweep: transmitter is busy", 5000);
        return;
    }
    if (txBar_->ampMode()) {
        statusBar()->showMessage(
            "SWR sweep refused: AMP mode is on — sweeping through an "
            "amplifier is a bad day. Turn AMP off first.", 8000);
        return;
    }
    if (curBand_ < 0 || is60m(curBand_)) {
        statusBar()->showMessage(
            "SWR sweep refused: dial is not in a sweepable ham band "
            "(60 m is channelized)", 8000);
        return;
    }
    const int64_t viewC = int64_t(centerHz_) + pan_->viewShiftHz();
    const int span = pan_->viewSpanHz();
    // Both ends stay kSwrEdgeMarginHz inside the band: the carrier must
    // never sit exactly on the edge (see the constant).
    const int64_t bandLo = int64_t(kBands[curBand_].loHz) + kSwrEdgeMarginHz;
    const int64_t bandHi = int64_t(kBands[curBand_].hiHz) - kSwrEdgeMarginHz;
    if (wholeBand) {
        // Band edge to edge. The dial has no view limit; the pan overlay
        // simply clips to whatever the frame can show, and the Smith
        // chart renders the full run.
        swrF0_ = bandLo;
        swrF1_ = bandHi;
    } else {
        swrF0_ = std::max<int64_t>(viewC - span / 2, bandLo);
        swrF1_ = std::min<int64_t>(viewC + span / 2, bandHi);
    }
    if (swrF1_ - swrF0_ < 20000) {
        statusBar()->showMessage("SWR sweep: visible in-band range is too "
                                 "narrow — zoom out first", 6000);
        return;
    }
    // Hold the view on the swept range (band overview already does; a
    // classic view would otherwise re-center on every step and slide).
    if (frameCenterHz_ == 0)
        applyFrame(viewC, span);
    swrPrevDial_ = centerHz_;
    swrPts_.clear();
    swrStepIdx_ = 0;
    swrStepTuned_ = false;
    swrStepCount_ = int(std::clamp<int64_t>(
        (swrF1_ - swrF0_) / kSwrTargetHzPerStep, kSwrMinSteps, kSwrMaxSteps));
    lastSwrMs_ = 0;
    swrUsedMeter_ = meterSwrReady();
    swrMeterPts_ = 0;
    startManualTune();
    if (!tuning_) return;                          // carrier did not start
    // The sweep does NOT touch the meter's display mode — operator's call
    // (2026-08-01), and it matches the vendor's own practice: TelePost's
    // LP100_Plot, the same kind of automated sweep, only ever polls and
    // reads the meter as it finds it. Whether Peak Hold's 0.25-5 s hold
    // even reaches the serial numbers is unproven; if a sweep taken in
    // Peak Hold ever comes out visibly smeared, that is the evidence to
    // revisit with — LpMeter::seekMode() is still there and verified.
    // Carrier failsafe. It has to scale with the step count now that the
    // count scales with span, or a legitimately long 6 m sweep would trip
    // its own safety net partway through and throw the run away. Budget
    // the typical per-step cost (one tick to tune, the settle rounded up
    // to whole ticks, one tick to read), double it for slow steps, and
    // never go below the old 75 s.
    // A step costs one tick to issue the tune, its settle rounded up to
    // whole ticks, then the tick that takes the reading.
    const auto stepMs = [](int settleMs) {
        return kSwrTickMs
               + ((settleMs + kSwrTickMs - 1) / kSwrTickMs) * kSwrTickMs;
    };
    const int sweepEstMs = stepMs(kSwrFirstSettleMs)
                           + (swrStepCount_ - 1) * stepMs(kSwrSettleMs);
    tuneTimeout_->stop();
    tuneTimeout_->setInterval(std::max(75000, sweepEstMs * 2));
    tuneTimeout_->start();                         // (restored on stop)
    if (!swrTick_) {
        swrTick_ = new QTimer(this);
        swrTick_->setInterval(kSwrTickMs);
        connect(swrTick_, &QTimer::timeout, this, &MainWindow::swrTickStep);
    }
    swrSweeping_ = true;
    swrTick_->start();
    // Step count and duration both vary with span now, so say them: the
    // operator is about to hold a carrier up and should know for how long.
    // Name the REASON when a configured meter isn't being used. Bare
    // "reading the radio" reads like a setting the operator chose, so a
    // dead meter looks identical to not owning one.
    const QString src =
        swrUsedMeter_ ? QStringLiteral("the wattmeter")
      : meterConfiguredButSilent()
            ? QString("the radio (%1 NOT ANSWERING on %2)")
                  .arg(meterName_, meterDevUsed_)
            : QStringLiteral("the radio");
    statusBar()->showMessage(
        QString("SWR sweep: %1-%2 MHz, %3 steps (%4 kHz each, ~%5 s) at "
                "%6 W, reading %7 — any click aborts")
            .arg(swrF0_ / 1e6, 0, 'f', 3).arg(swrF1_ / 1e6, 0, 'f', 3)
            .arg(swrStepCount_)
            .arg(double(swrF1_ - swrF0_) / 1000.0
                     / std::max(1, swrStepCount_ - 1), 0, 'f', 1)
            .arg(sweepEstMs / 1000)
            .arg(txBar_->tuneLevel())
            .arg(src));
}

void MainWindow::swrTickStep() {
    if (!swrSweeping_) return;
    if (!tuning_) {                                // failsafe or TUNE click
        stopSwrSweep(false);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int64_t stepF = swrF0_
        + (swrF1_ - swrF0_) * swrStepIdx_ / std::max(1, swrStepCount_ - 1);
    if (!swrStepTuned_) {
        swrQuietTune_ = true;                      // no plan-mode re-moding
        tuneAbsolute(uint64_t(stepF));
        swrQuietTune_ = false;
        // Step 0 pays for the cold key-up (and flushes the meter's
        // averaging window of its pre-carrier state); later steps only
        // have to settle a dial move. See kSwrFirstSettleMs.
        swrStepArmedMs_ = now + (swrStepIdx_ == 0 ? kSwrFirstSettleMs
                                                  : kSwrSettleMs);
        swrStepTuned_ = true;
        return;
    }
    // A point is taken only from a reading that postdates this step's dial
    // move, so we never record the previous frequency. One rule keeps the
    // curve honest: NEVER mix rulers inside a run. The meter and the
    // radio's @STF disagree systematically (different calibrations), and a
    // single borrowed @STF value splices a visible kink into an otherwise
    // clean meter curve — seen live. So: while the meter is delivering,
    // a step that misses its reading is SKIPPED (the curve bridges one
    // step, invisibly) rather than patched from the radio. The radio still
    // provides the WHOLE curve when the meter sees no RF at all (coupler
    // in the other rig's line — swrMeterPts_ == 0 keeps that path open)
    // or was never selected.
    PanadapterWidget::SwrRun::Pt pt;
    pt.hz = qint64(stepF);
    bool fresh = false;
    const bool meterLive = swrUsedMeter_ && txMeter_ && txMeter_->isAlive();
    if (meterLive) {
        const ttc::TxMeter::Reading& r = txMeter_->last();
        // valid means the meter actually saw the carrier; without it the
        // fields are idle noise. R/X are recorded only from a vector meter
        // (zValid) — a PowerMaster run yields an SWR-only curve, and the
        // Smith chart simply stays unavailable for it.
        if (r.tsMs > swrStepArmedMs_ && r.valid) {
            pt.swr = r.swr;
            if (r.zValid) {
                pt.rOhm   = r.rOhm;
                pt.xOhm   = r.xOhm;
                pt.zValid = true;
            }
            fresh = true;
            swrMeterPts_++;
        }
    }
    if (!fresh && (!meterLive || swrMeterPts_ == 0)
        && lastSwrMs_ > swrStepArmedMs_) {                // radio's own @STF
        pt.swr = lastSwr_;
        fresh  = true;
    }
    if (!fresh && now < swrStepArmedMs_ + 1500) return;   // wait for a reading
    if (fresh)
        swrPts_.append(pt);                               // timeout: skip point
    swrStepIdx_++;
    swrStepTuned_ = false;
    if (swrStepIdx_ >= swrStepCount_)
        stopSwrSweep(true);
}

void MainWindow::stopSwrSweep(bool completed) {
    if (!swrSweeping_) return;
    swrSweeping_ = false;
    if (swrTick_) swrTick_->stop();
    stopManualTune();                              // unkey, restore mode+power
    tuneTimeout_->setInterval(15000);              // plain TUNE failsafe back
    swrQuietTune_ = true;                          // restore tune keeps the
    tuneAbsolute(swrPrevDial_);                    // operator's restored mode
    swrQuietTune_ = false;
    if (completed && swrPts_.size() >= 8 && curBand_ >= 0) {
        PanadapterWidget::SwrRun run;
        run.ts   = QDateTime::currentSecsSinceEpoch();
        run.ant  = curAntenna_;
        run.band = QLatin1String(kBands[curBand_].label);
        run.pts  = swrPts_;
        // Storage keeps every sweep forever (browse/delete via the SWR
        // history dialog); only the live overlay/Smith chart trim to a
        // couple of runs for display.
        swrRuns_[swrKey(run.ant, run.band)].prepend(run);
        saveSwrRuns();
        QSettings().setValue("swr/show", true);
        pan_->setShowSwr(true);
        refreshSwrOverlay();
        double minS = 99.0;
        qint64 minF = 0;
        for (const auto& p : run.pts)
            if (p.swr < minS) { minS = p.swr; minF = p.hz; }
        QString msg = QString("SWR sweep done: best %1 at %2 MHz (%3 points)")
                          .arg(minS, 0, 'f', 2).arg(minF / 1e6, 0, 'f', 3)
                          .arg(swrPts_.size());
        // With a vector meter we also know where the reactance changes sign
        // — the true resonance, which is NOT always the SWR minimum and is
        // the number you actually want when cutting an antenna. The meter
        // reports |X| without its sign (live-verified: the operator's 40 m
        // run dips to ~4 Ω and rises both sides, never negative), so the
        // sign is inferred from the sweep shape the same way the Smith
        // chart and the vendor's own Plot program do it.
        // Only for a run that is vector end to end: a mid-sweep fallback to
        // the radio leaves |X| holes that would fool the dip detector.
        const bool allZ = std::all_of(
            run.pts.cbegin(), run.pts.cend(),
            [](const PanadapterWidget::SwrRun::Pt& pt) { return pt.zValid; });
        if (allZ) {
            QVector<double> absX, rr;
            for (const auto& pt : run.pts) {
                absX.append(std::fabs(pt.xOhm));
                rr.append(pt.rOhm);
            }
            const QVector<int> sg = SmithChartWidget::inferXSigns(absX, rr);
            for (int i = 1; i < run.pts.size(); ++i) {
                const auto &a = run.pts[i - 1], &b = run.pts[i];
                const double xa = sg[i - 1] * std::fabs(a.xOhm);
                const double xb = sg[i] * std::fabs(b.xOhm);
                if ((xa <= 0.0) == (xb <= 0.0) || xa == xb) continue;
                const double t = xa / (xa - xb);
                const double fRes = a.hz + t * double(b.hz - a.hz);
                msg += QString("; X=0 near %1 MHz, R≈%2 Ω")
                           .arg(fRes / 1e6, 0, 'f', 3)
                           .arg(a.rOhm + t * (b.rOhm - a.rOhm), 0, 'f', 1);
                break;
            }
        }
        // The trap this catches is real (it caught the operator): meter
        // alive and streaming, but its COUPLER is in the other radio's
        // antenna line, so it honestly reports 0 W all sweep and every
        // point silently falls back to the radio's coarse @STF. Say so —
        // a curve that quietly isn't what the operator asked for is worse
        // than an error.
        if (swrUsedMeter_ && swrMeterPts_ == 0)
            msg += " — WATTMETER SAW NO RF (coupler in this radio's "
                   "line?); curve is the radio's own reading";
        else if (swrUsedMeter_ && swrMeterPts_ < swrPts_.size())
            msg += QString(" (%1 of %2 points from the wattmeter)")
                       .arg(swrMeterPts_).arg(swrPts_.size());
        // The sibling trap, and the one that actually bit: the meter is
        // enabled in Setup but never answered, so the run never even
        // wanted it. Saying nothing here let a radio curve be filed under
        // the antenna as if the meter had measured it.
        else if (meterConfiguredButSilent())
            msg += QString(" — %1 NOT ANSWERING on %2; curve is the radio's "
                           "own reading, not the meter's")
                       .arg(meterName_, meterDevUsed_);
        statusBar()->showMessage(msg, 15000);
    } else {
        statusBar()->showMessage(
            "SWR sweep stopped — carrier off, dial and settings restored",
            8000);
    }
    swrPts_.clear();
}

// Non-modal so the operator can sweep again and compare; reopening just
// refreshes it with the given antenna+band's runs. Only the newest two
// runs are shown (SmithChartWidget itself only ever draws runs[0]/[1] —
// "newest" + "one dim backdrop" — so trimming here just keeps the list
// we hand it small and cheap; storage underneath is unbounded).
void MainWindow::showSmithChart(const QString& ant, const QString& band) {
    if (band.isEmpty()) return;
    const QVector<PanadapterWidget::SwrRun> runs =
        swrRuns_.value(swrKey(ant, band)).mid(0, 2);
    if (smithDlg_) smithDlg_->close();     // WA_DeleteOnClose reaps it
    auto* dlg = new QDialog(this);
    smithDlg_ = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QString("Smith chart — %1 / %2 m")
                             .arg(ant.isEmpty() ? "(unlabeled)" : ant, band));
    dlg->setStyleSheet("QDialog { background: #141b24; }");
    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(4, 4, 4, 4);
    if (!runs.isEmpty()) {
        // Condition context (weather, recent antenna work) for the newest
        // run — this is where a past sweep gets reviewed in detail, so an
        // odd-looking curve's explanation should be visible right here.
        auto* sub = new QLabel(dlg);
        sub->setStyleSheet("QLabel { color: #8a9bb0; }");
        sub->setWordWrap(true);
        QString subTxt = QDateTime::fromSecsSinceEpoch(runs.first().ts)
                              .toString("yyyy-MM-dd HH:mm");
        if (!runs.first().notes.isEmpty())
            subTxt += " — " + runs.first().notes;
        sub->setText(subTxt);
        lay->addWidget(sub);
    }
    auto* chart = new SmithChartWidget(dlg);
    chart->setRuns(runs, band);
    lay->addWidget(chart);
    dlg->resize(600, 660);
    dlg->show();
}

void MainWindow::refreshSwrOverlay() {
    if (curBand_ >= 0)
        pan_->setSwrRuns(swrRuns_.value(swrKey(curAntenna_,
                              QLatin1String(kBands[curBand_].label))).mid(0, 2));
    else
        pan_->setSwrRuns({});
}

// Non-modal, single instance (like smithDlg_) — reopening just refreshes
// it. The dialog owns no data; it reacts to user actions with signals
// and this is where they land against the real store.
void MainWindow::showSwrHistory() {
    if (!swrHistDlg_) {
        auto* dlg = new SwrHistoryDialog(this);
        swrHistDlg_ = dlg;
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &SwrHistoryDialog::openSmith, this,
                [this](const QString& ant, const QString& band) {
                    showSmithChart(ant, band);
                });
        connect(dlg, &SwrHistoryDialog::deleteRun, this,
                [this, dlg](const QString& ant, const QString& band, qint64 ts) {
                    auto& list = swrRuns_[swrKey(ant, band)];
                    list.erase(std::remove_if(list.begin(), list.end(),
                                   [ts](const auto& r) { return r.ts == ts; }),
                               list.end());
                    if (list.isEmpty()) swrRuns_.remove(swrKey(ant, band));
                    saveSwrRuns();
                    refreshSwrOverlay();
                    dlg->setRuns(allSwrRuns());
                    statusBar()->showMessage("SWR: sweep deleted");
                });
        connect(dlg, &SwrHistoryDialog::clearBucket, this,
                [this, dlg](const QString& ant, const QString& band) {
                    swrRuns_.remove(swrKey(ant, band));
                    saveSwrRuns();
                    refreshSwrOverlay();
                    dlg->setRuns(allSwrRuns());
                    statusBar()->showMessage(
                        "SWR: history cleared for this antenna+band");
                });
        connect(dlg, &SwrHistoryDialog::notesEdited, this,
                [this, dlg](const QString& ant, const QString& band,
                            qint64 ts, const QString& notes) {
                    auto& list = swrRuns_[swrKey(ant, band)];
                    for (auto& r : list)
                        if (r.ts == ts) { r.notes = notes; break; }
                    saveSwrRuns();
                    dlg->setRuns(allSwrRuns());
                });
    }
    static_cast<SwrHistoryDialog*>(swrHistDlg_.data())->setRuns(allSwrRuns());
    swrHistDlg_->show();
    swrHistDlg_->raise();
    swrHistDlg_->activateWindow();
}

QVector<PanadapterWidget::SwrRun> MainWindow::allSwrRuns() const {
    QVector<PanadapterWidget::SwrRun> out;
    for (const auto& list : swrRuns_) out += list;
    return out;
}

// Composite key: antenna + band, so two antennas covering the same band
// keep separate histories instead of overwriting each other's runs.
// Unit separator (0x1F) never appears in typed antenna names and splits
// trivially — used only as the in-memory QHash key, never persisted.
QString MainWindow::swrKey(const QString& ant, const QString& band) {
    return ant + QChar(0x1F) + band;
}

// v2 file schema: {"version":2, "antennas":[...], "runs":[{"ant","band",
// "ts","notes","pts"}, ...]}. Unlike the old point-array format (which
// degrades both ways on purpose), this is a hard bump: an old build
// finds no band-label keys at the root of a v2 file and just loads
// nothing (not a crash) — acceptable since there is no real downgrade
// path, and silently misreading a run with no antenna concept at all
// would corrupt grouping rather than just losing a field.
void MainWindow::saveSwrRuns() const {
    QJsonObject root;
    root["version"] = 2;
    root["antennas"] =
        QJsonArray::fromStringList(QSettings().value("swr/antennas").toStringList());
    QJsonArray runs;
    for (auto it = swrRuns_.cbegin(); it != swrRuns_.cend(); ++it) {
        for (const auto& run : it.value()) {
            QJsonArray pts;
            // [Hz, SWR] as always, extended to [Hz, SWR, R, X] for points
            // measured with a vector meter.
            for (const auto& p : run.pts) {
                QJsonArray pa{double(p.hz), p.swr};
                if (p.zValid) { pa.append(p.rOhm); pa.append(p.xOhm); }
                pts.append(pa);
            }
            runs.append(QJsonObject{{"ant", run.ant}, {"band", run.band},
                                     {"ts", double(run.ts)},
                                     {"notes", run.notes}, {"pts", pts}});
        }
    }
    root["runs"] = runs;
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QFile f(dir + "/swr.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

static PanadapterWidget::SwrRun swrRunFromJson(const QJsonObject& ro) {
    PanadapterWidget::SwrRun run;
    run.ts    = qint64(ro.value("ts").toDouble());
    run.ant   = ro.value("ant").toString();
    run.notes = ro.value("notes").toString();
    for (const QJsonValue& pv : ro.value("pts").toArray()) {
        const QJsonArray pa = pv.toArray();
        PanadapterWidget::SwrRun::Pt pt;
        pt.hz  = qint64(pa.at(0).toDouble());
        pt.swr = pa.at(1).toDouble();
        if (pa.size() >= 4) {          // vector-meter run
            pt.rOhm   = pa.at(2).toDouble();
            pt.xOhm   = pa.at(3).toDouble();
            pt.zValid = true;
        }
        run.pts.append(pt);
    }
    return run;
}

void MainWindow::loadSwrRuns() {
    QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + "/swr.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (!root.contains("version")) {
        // Old band-only format: {"<band>": [{"ts","pts"}, ...]}. Every run
        // migrates into the unlabeled ("") antenna bucket for that band —
        // real historical sweeps are preserved, nothing dropped. Re-saved
        // in v2 immediately below so this path runs exactly once.
        for (auto it = root.begin(); it != root.end(); ++it) {
            QVector<PanadapterWidget::SwrRun> list;
            for (const QJsonValue& rv : it.value().toArray()) {
                PanadapterWidget::SwrRun run = swrRunFromJson(rv.toObject());
                run.ant = QString();
                run.band = it.key();
                if (run.pts.size() >= 2) list.append(run);
            }
            if (!list.isEmpty()) swrRuns_[swrKey(QString(), it.key())] = list;
        }
        if (!swrRuns_.isEmpty()) saveSwrRuns();
        return;
    }
    // swr.json's "antennas" array is a synced-on-save copy for the file's
    // self-description; QSettings stays authoritative — merge in anything
    // the file knows that QSettings doesn't (e.g. a config wipe).
    {
        QSettings s;
        QStringList known = s.value("swr/antennas").toStringList();
        for (const QJsonValue& v : root.value("antennas").toArray()) {
            const QString name = v.toString();
            if (!name.isEmpty() && !known.contains(name)) known << name;
        }
        s.setValue("swr/antennas", known);
    }
    for (const QJsonValue& rv : root.value("runs").toArray()) {
        const QJsonObject ro = rv.toObject();
        PanadapterWidget::SwrRun run = swrRunFromJson(ro);
        run.band = ro.value("band").toString();
        if (run.pts.size() >= 2)
            swrRuns_[swrKey(run.ant, run.band)].append(run);
    }
}

// Digital/voice audio switch. The Orion has no MIC/LINE/BOTH CAT
// command, so the radio's input source is set to BOTH once (front panel) and
// we swap between front-mic and rear line-input purely by their gains:
//   digital: mic 0, speech proc off, aux/line 100
//   voice:   aux/line 0, mic + speech proc restored to the learned values
// Voice settings are snapshotted (and persisted) the moment we go digital, so
// whatever you actually run for SSB is what comes back.
void MainWindow::setDigitalMode(bool on) {
    if (on == digital_) return;
    if (on) {
        if (lastMicGain_ > 0) {                     // remember the voice setup
            QSettings s;                            // (never persist the zeros
            s.setValue("audio/voiceMic", lastMicGain_);       // of a digital
            s.setValue("audio/voiceSpeech", lastSpeechProc_); // state)
        }
        digital_ = true;                            // (gates the "learn" slots)
        radio_->setMicGain(0);
        radio_->setSpeechProc(0);
        radio_->setAuxInputGain(100);
        txBar_->showMicGain(0);        // readout matches the radio (was stuck
        txBar_->showSpeechProc(0);     // on the remembered voice value)
        statusBar()->showMessage("DIGITAL: line-in 100, mic/speech off");
    } else {
        digital_ = false;
        if (lastMicGain_ <= 0) {
            // The learned value was poisoned (zeros captured from an old
            // digital session): fall back to the persisted voice setup,
            // then to sane defaults.
            QSettings s;
            const int vm = s.value("audio/voiceMic", 51).toInt();
            lastMicGain_ = vm > 0 ? std::min(vm, 100) : 51;   // stored 0 = poisoned too
            lastSpeechProc_ = std::clamp(s.value("audio/voiceSpeech", 2).toInt(), 0, 9);
        }
        radio_->setAuxInputGain(0);
        radio_->setMicGain(lastMicGain_);
        radio_->setSpeechProc(lastSpeechProc_);
        txBar_->showMicGain(lastMicGain_);
        txBar_->showSpeechProc(lastSpeechProc_);
        statusBar()->showMessage(QString("VOICE: mic %1, speech %2, line-in off")
                                     .arg(lastMicGain_).arg(lastSpeechProc_));
    }
    panel_->showDigital(on);
}

// TX profiles (KE9NS TXProfile idea, sized to the Orion's CAT surface): a
// bundle of TX filter BW, speech-proc level, mic gain and power, recalled in
// one click from the TX bar. Slots ship with sensible defaults and are
// overwritten in place by right-clicking the button (saveTxProfile).
struct TxProf { int bw, proc, mic, pwr; };
static const TxProf kTxProfDefault[4] = {
    {3000, 0, 50, 100},   // RAG  — natural ragchew audio, no processing
    {2400, 5, 55, 100},   // DX   — mid-focused punch, moderate compression
    {2100, 7, 55, 100},   // CONT — narrow and dense for contest runs
    {3900, 0, 45, 100},   // ESSB — the Orion's widest, processor off
};
static const char* kTxProfName[4] = {"RAGCHEW", "DX", "CONTEST", "ESSB"};

void MainWindow::applyTxProfile(int slot) {
    slot = std::clamp(slot, 0, 3);
    QSettings st;
    const QString k = QString("txprof/%1/").arg(slot);
    const TxProf& d = kTxProfDefault[slot];
    const int bw   = std::clamp(st.value(k + "bw",   d.bw).toInt(), 900, 3900);
    const int proc = std::clamp(st.value(k + "proc", d.proc).toInt(), 0, 9);
    const int mic  = std::clamp(st.value(k + "mic",  d.mic).toInt(), 0, 100);
    int pwr        = std::clamp(st.value(k + "pwr",  d.pwr).toInt(), 0, 100);
    if (txBar_->ampMode()) pwr = std::min(pwr, txBar_->ampLimit());
    txBwHz_ = bw;
    lastSpeechProc_ = proc;
    lastMicGain_ = mic;
    radio_->setTxFilter(bw);
    radio_->setTxPower(pwr);
    txBar_->showTxFilter(bw);
    txBar_->showTxPower(pwr);
    if (digital_) {
        // Mic and processor are parked at 0 as the line-in switch; the
        // profile's values land when voice comes back (setDigitalMode).
        statusBar()->showMessage(QString("TX profile %1: bw %2 Hz, pwr %3 "
                                         "(mic %4 / proc %5 queued for voice)")
                                     .arg(kTxProfName[slot]).arg(bw).arg(pwr)
                                     .arg(mic).arg(proc));
        return;
    }
    radio_->setSpeechProc(proc);
    radio_->setMicGain(mic);
    txBar_->showSpeechProc(proc);
    txBar_->showMicGain(mic);
    statusBar()->showMessage(QString("TX profile %1: bw %2 Hz  proc %3  mic %4  pwr %5")
                                 .arg(kTxProfName[slot]).arg(bw).arg(proc)
                                 .arg(mic).arg(pwr));
}

void MainWindow::saveTxProfile(int slot) {
    slot = std::clamp(slot, 0, 3);
    QSettings st;
    const QString k = QString("txprof/%1/").arg(slot);
    st.setValue(k + "bw",   txBwHz_);
    st.setValue(k + "proc", lastSpeechProc_);      // voice values even in digital
    st.setValue(k + "mic",  lastMicGain_);
    st.setValue(k + "pwr",  lastTxPwr_);
    statusBar()->showMessage(QString("TX profile %1 saved: bw %2 Hz  proc %3  mic %4  pwr %5")
                                 .arg(kTxProfName[slot]).arg(txBwHz_)
                                 .arg(lastSpeechProc_).arg(lastMicGain_).arg(lastTxPwr_));
}

void MainWindow::saveBandMemory() {
    if (curBand_ < 0 || curBand_ >= kBandCount) return;
    if (is60m(curBand_)) return;                 // 60 m channels are locked
    // Only stamp the register if the current frequency actually belongs to this
    // band — otherwise a client (WSJT-X/cqrlog) that moved the VFO elsewhere
    // would corrupt the outgoing register with an unrelated frequency.
    if (centerHz_ < kBands[curBand_].loHz || centerHz_ > kBands[curBand_].hiHz) return;
    QSettings s;
    const QString key = QString("band/%1/%2/")
                            .arg(kBands[curBand_].label).arg(kStackNames[curReg_]);
    s.setValue(key + "freq", QVariant::fromValue<qulonglong>(centerHz_));
    s.setValue(key + "mode", static_cast<int>(rigMode_));
    s.setValue(key + "bw",   rigBwHz_);
    s.setValue(key + "pbt",  rigPbtHz_);
    s.setValue(QString("band/%1/reg").arg(kBands[curBand_].label), curReg_);
}

// Every frequency change funnels here (dial-follow and tuneAbsolute). Keeps
// curBand_/curReg_ honest and schedules a debounced stamp so the active stack
// register always holds "where I last was on this band" — the same semantics
// as the Orion's own (unreadable-over-CAT) band stack, kept in lockstep.
void MainWindow::syncBandRegister() {
    const int idx = bandIndexOf(centerHz_);
    const bool crossed = (idx != curBand_);
    if (crossed) {
        curBand_ = idx;
        panel_->showBand(idx);
        refreshSwrOverlay();                    // this band's stored curves
        maybeRunVoacap();                       // forecast follows the band
        lastBandPress_ = -1;                    // band moved under the buttons:
        if (idx >= 0 && !is60m(idx)) {          // next press recalls, not cycles
            QSettings s;
            curReg_ = std::clamp(
                s.value(QString("band/%1/reg").arg(kBands[idx].label), 0).toInt(),
                0, kStackCount - 1);
            // If we landed exactly on a stored register (the radio's band
            // button recalls the same spot we stamped), adopt its letter.
            for (int r = 0; r < kStackCount; ++r) {
                const QString key = QString("band/%1/%2/")
                                        .arg(kBands[idx].label).arg(kStackNames[r]);
                const uint64_t f = s.value(key + "freq",
                    QVariant::fromValue<qulonglong>(kBands[idx].stack[r].hz))
                    .toULongLong();
                if (f == centerHz_) { curReg_ = r; break; }
            }
        }
    }
    if (is60m(curBand_)) {
        // Locked channels: label whichever channel the dial sits on ("--"
        // when between channels inside the band) and never stamp anything.
        int ch = -1;
        for (int i = 0; i < kChan60Count; ++i)
            if (kUs60mChans[i].dialHz == centerHz_) { ch = i; break; }
        if (ch >= 0) curReg_ = ch;
        panel_->showBandStackText(
            ch >= 0 ? QString("STACK %1").arg(kUs60mChans[ch].name) : "STACK --");
        return;
    }
    panel_->showBandStack(curBand_ >= 0 ? curReg_ : -1);
    if (curBand_ >= 0 && bandStamp_) bandStamp_->start();
}

void MainWindow::recallStack(int band, int reg) {
    // FREQUENCY ONLY. The Orion's real band stack is unreadable over CAT,
    // and the shadow copy we kept here applied its stored mode/filter on
    // every recall — which fought the radio and the operator whenever the
    // stored mode wasn't what they wanted next (poisoned stamps put LSB on
    // CW registers; legitimate phone stamps ambushed CW sessions the same
    // way). Operator's verdict: the radio owns mode. A band button hops to
    // the last frequency used on that band and touches nothing else.
    const StackDef& seed = kBands[band].stack[reg];
    QSettings s;
    const QString key = QString("band/%1/%2/")
                            .arg(kBands[band].label).arg(kStackNames[reg]);
    const uint64_t f =
        s.value(key + "freq", QVariant::fromValue<qulonglong>(seed.hz)).toULongLong();
    tuneAbsolute(f);                            // syncs curBand_ (may guess a register)
    curReg_ = reg;                              // explicit recall wins over the guess
    panel_->showBandStack(reg);
    s.setValue(QString("band/%1/reg").arg(kBands[band].label), reg);
    statusBar()->showMessage(QString("band %1m stack %2  ->  %3 MHz")
                                 .arg(kBands[band].label).arg(kStackNames[reg])
                                 .arg(f / 1e6, 0, 'f', 4));
}

// US 60 m channel recall: everything comes from the hard-coded kUs60mChans
// table — no QSettings override, no radio read-back adoption; the channels
// are locked. Sequenced like recallStack (the Orion drops commands during a
// mode switch), with the channel's transmit profile trailing once the DSP
// traffic has settled. CH3 flips to line-in for FT8; voice channels restore
// the mic with the channel's processor level.
void MainWindow::recall60m(int chan) {
    chan = std::clamp(chan, 0, kChan60Count - 1);
    const Chan60& c = kUs60mChans[chan];
    tuneAbsolute(c.dialHz);
    curReg_ = chan;                             // channel index rides curReg_
    QTimer::singleShot(120, this, [this, m = c.mode] {
        applyMode(m);
        panel_->showMode(m);
    });
    QTimer::singleShot(450, this, [this, bw = c.bwHz] {
        radio_->setBandwidthHz(Rx::Main, bw);
        radio_->setPbtHz(Rx::Main, 0);
    });
    QTimer::singleShot(650, this, [this, chan] {
        const Chan60& c = kUs60mChans[chan];
        int pwr = c.txPwrPct;
        if (txBar_->ampMode()) pwr = std::min(pwr, txBar_->ampLimit());
        radio_->setTxFilter(c.txBwHz);
        radio_->setTxPower(pwr);
        txBwHz_ = c.txBwHz;
        lastTxPwr_ = pwr;
        txBar_->showTxFilter(c.txBwHz);
        txBar_->showTxPower(pwr);
        if (c.digital) {
            setDigitalMode(true);               // FT8: line-in, mic/proc parked
        } else {
            lastSpeechProc_ = c.procLvl;
            if (digital_) {
                setDigitalMode(false);          // restores mic + our proc level
            } else {
                radio_->setSpeechProc(c.procLvl);
                txBar_->showSpeechProc(c.procLvl);
            }
        }
    });
    rigBwHz_  = c.bwHz;                         // optimistic UI; poll confirms
    rigPbtHz_ = 0;
    rigctld_.cacheBandwidth(c.bwHz);
    panel_->showPbt(0);
    refreshPassbandOverlay();
    panel_->showBandStackText(QString("STACK %1").arg(c.name));
    QSettings().setValue("band/60/chan", chan);
    statusBar()->showMessage(QString("60m %1  %2 MHz  ->  pwr %3, TX bw %4 (locked)")
                                 .arg(c.name).arg(c.dialHz / 1e6, 0, 'f', 4)
                                 .arg(c.txPwrPct).arg(c.txBwHz));
}

void MainWindow::saveMarkers() {
    QStringList out;
    for (const auto& m : markers_)
        out << QString("%1|%2").arg(m.hz).arg(m.label);
    QSettings().setValue("panadapter/markers", out);
    pan_->setMarkers(markers_);
}

// Scheduled IQ recording (SDR Console's recording scheduler): arm a start
// time, duration and dial, walk away — the capture lands in the usual iq/
// directory for skimreplay. RX only, nothing here can key the radio. One
// schedule at a time; invoking again while armed cancels it. The use case
// that asked for it: W1AW code practice runs at fixed UTC slots.
void MainWindow::scheduleIqRecordingDialog() {
    if (schedStartTmr_ && schedStartTmr_->isActive()) {    // armed -> cancel
        schedStartTmr_->stop();
        schedIqAct_->setText("Schedule IQ recording…");
        statusBar()->showMessage("scheduled IQ recording canceled");
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle("Schedule IQ recording");
    auto* form = new QFormLayout(&dlg);
    auto* when = new QTimeEdit(&dlg);
    when->setDisplayFormat("HH:mm");
    when->setTime(QDateTime::currentDateTimeUtc().time().addSecs(300));
    form->addRow("Start (UTC):", when);
    auto* mins = new QSpinBox(&dlg);
    mins->setRange(1, 180);
    mins->setValue(10);
    mins->setSuffix(" min");
    form->addRow("Duration:", mins);
    auto* dial = new QDoubleSpinBox(&dlg);
    dial->setRange(100.0, 60000.0);
    dial->setDecimals(1);
    dial->setSuffix(" kHz");
    dial->setValue(centerHz_ / 1000.0);
    form->addRow("Dial:", dial);
    auto* note = new QLabel(&dlg);
    const auto updNote = [note, mins] {
        note->setText(QString("≈ %1 MB on disk (2 MB/s)")
                          .arg(mins->value() * 120));
    };
    updNote();
    connect(mins, &QSpinBox::valueChanged, &dlg, updNote);
    form->addRow(note);
    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;

    schedHz_   = static_cast<uint64_t>(dial->value() * 1000.0 + 0.5);
    schedSecs_ = mins->value() * 60;
    QDateTime at(QDate::currentDate(), when->time(), QTimeZone::utc());
    if (at <= QDateTime::currentDateTimeUtc()) at = at.addDays(1);
    if (!schedStartTmr_) {
        schedStartTmr_ = new QTimer(this);
        schedStartTmr_->setSingleShot(true);
        connect(schedStartTmr_, &QTimer::timeout, this, [this] {
            schedIqAct_->setText("Schedule IQ recording…");
            tuneAbsolute(schedHz_);
            // Let the SDR retune settle before opening the file — and the
            // tune must come FIRST, because tuning auto-stops a recording.
            QTimer::singleShot(1500, this, [this] {
                recIqAct_->setChecked(true);
                QTimer::singleShot(schedSecs_ * 1000, this, [this] {
                    recIqAct_->setChecked(false);  // no-op if already stopped
                });
            });
        });
    }
    schedStartTmr_->start(
        int(QDateTime::currentDateTimeUtc().msecsTo(at)));
    schedIqAct_->setText(QString("Cancel scheduled IQ recording (%1 UTC · "
                                 "%2 kHz · %3 min)")
                             .arg(when->time().toString("HH:mm"))
                             .arg(schedHz_ / 1000.0, 0, 'f', 1)
                             .arg(mins->value()));
    statusBar()->showMessage(
        QString("IQ recording armed for %1 UTC at %2 kHz")
            .arg(when->time().toString("HH:mm"))
            .arg(schedHz_ / 1000.0, 0, 'f', 1), 8000);
}

} // namespace ttc
