// SPDX-License-Identifier: GPL-2.0-or-later
// MainWindow operating features: manual tune carrier, digital/voice audio
// switching, TX profiles, band-stack memories, US 60 m channels and the
// DVR/voice-keyer. Split out of MainWindow.cpp (see MainWindowInternal.h).
#include "app/MainWindow.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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

static constexpr int kSwrSteps = 48;

// The external meter is believed only when it is enabled, chosen as the
// source, AND answering right now. Everything else falls through to @STF,
// which is what a station without an LP-100A always gets.
bool MainWindow::meterSwrReady() const {
    return lpMeter_ && lpMeter_->isAlive()
           && QSettings().value("swr/source", "meter").toString() != "radio";
}

void MainWindow::showSwrMenu(const QPoint& globalPos) {
    auto* m = new QMenu(this);
    m->setAttribute(Qt::WA_DeleteOnClose);
    styleMenu(m);
    QAction* sweep = m->addAction(
        swrSweeping_ ? QStringLiteral("Stop SWR sweep")
                     : QStringLiteral("Sweep SWR across the view"));
    connect(sweep, &QAction::triggered, this, [this] {
        if (swrSweeping_) stopSwrSweep(false);
        else startSwrSweep();
    });
    QAction* show = m->addAction(QStringLiteral("Show SWR plots"));
    show->setCheckable(true);
    show->setChecked(QSettings().value("swr/show", true).toBool());
    connect(show, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("swr/show", on);
        pan_->setShowSwr(on);
    });
    QAction* clear = m->addAction(QStringLiteral("Clear this band's runs"));
    clear->setEnabled(curBand_ >= 0
                      && !swrRuns_.value(QLatin1String(
                             curBand_ >= 0 ? kBands[curBand_].label : ""))
                              .isEmpty());
    connect(clear, &QAction::triggered, this, [this] {
        if (curBand_ < 0) return;
        swrRuns_.remove(QLatin1String(kBands[curBand_].label));
        saveSwrRuns();
        refreshSwrOverlay();
        statusBar()->showMessage("SWR: runs cleared for this band");
    });
    m->popup(globalPos);
}

void MainWindow::startSwrSweep() {
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
    swrF0_ = std::max<int64_t>(viewC - span / 2, int64_t(kBands[curBand_].loHz));
    swrF1_ = std::min<int64_t>(viewC + span / 2, int64_t(kBands[curBand_].hiHz));
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
    swrStepCount_ = kSwrSteps;
    lastSwrMs_ = 0;
    swrUsedMeter_ = meterSwrReady();
    startManualTune();
    if (!tuning_) return;                          // carrier did not start
    // The sweep does NOT touch the meter's display mode — operator's call
    // (2026-08-01), and it matches the vendor's own practice: TelePost's
    // LP100_Plot, the same kind of automated sweep, only ever polls and
    // reads the meter as it finds it. Whether Peak Hold's 0.25-5 s hold
    // even reaches the serial numbers is unproven; if a sweep taken in
    // Peak Hold ever comes out visibly smeared, that is the evidence to
    // revisit with — LpMeter::seekMode() is still there and verified.
    tuneTimeout_->stop();                          // sweep failsafe: 75 s
    tuneTimeout_->setInterval(75000);              // (restored on stop)
    tuneTimeout_->start();
    if (!swrTick_) {
        swrTick_ = new QTimer(this);
        swrTick_->setInterval(150);
        connect(swrTick_, &QTimer::timeout, this, &MainWindow::swrTickStep);
    }
    swrSweeping_ = true;
    swrTick_->start();
    statusBar()->showMessage(
        QString("SWR sweep: %1-%2 MHz, %3 steps at %4 W, reading %5 "
                "— any click aborts")
            .arg(swrF0_ / 1e6, 0, 'f', 3).arg(swrF1_ / 1e6, 0, 'f', 3)
            .arg(swrStepCount_).arg(txBar_->tuneLevel())
            .arg(swrUsedMeter_ ? "the LP-100A" : "the radio"));
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
        swrStepArmedMs_ = now + 300;               // let the dial + PA settle
        swrStepTuned_ = true;
        return;
    }
    // Prefer the meter, but only per-step: if it dies halfway through a
    // sweep the remaining stops silently come from @STF rather than
    // stalling out. A point is taken only from a reading that postdates
    // this step's dial move, so we never record the previous frequency.
    PanadapterWidget::SwrRun::Pt pt;
    pt.hz = qint64(stepF);
    bool fresh = false;
    if (swrUsedMeter_ && lpMeter_ && lpMeter_->isAlive()) {
        const ttc::LpMeter::Reading& r = lpMeter_->last();
        // zValid means the meter actually saw the carrier; without it the
        // impedance fields are idle noise and the SWR is meaningless too.
        if (r.tsMs > swrStepArmedMs_ && r.zValid) {
            pt.swr    = r.swr;
            pt.rOhm   = r.rOhm;
            pt.xOhm   = r.xOhm;
            pt.zValid = true;
            fresh     = true;
        }
    }
    if (!fresh && lastSwrMs_ > swrStepArmedMs_) {         // radio's own @STF
        pt.swr = lastSwr_;
        fresh  = true;
    }
    if (!fresh && now < swrStepArmedMs_ + 1500) return;   // wait for a reading
    if (fresh)
        swrPts_.append(pt);
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
        run.ts = QDateTime::currentSecsSinceEpoch();
        run.pts = swrPts_;
        auto& list = swrRuns_[QLatin1String(kBands[curBand_].label)];
        list.prepend(run);
        while (list.size() > 2) list.removeLast();
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
        // the number you actually want when cutting an antenna. Reported by
        // linear interpolation between the two stops that straddle X = 0.
        for (int i = 1; i < run.pts.size(); ++i) {
            const auto &a = run.pts[i - 1], &b = run.pts[i];
            if (!a.zValid || !b.zValid) continue;
            if ((a.xOhm <= 0.0) == (b.xOhm <= 0.0)) continue;
            const double t = a.xOhm / (a.xOhm - b.xOhm);   // denominator != 0
            const double fRes = a.hz + t * double(b.hz - a.hz);
            msg += QString("; X=0 at %1 MHz, R≈%2 Ω")
                       .arg(fRes / 1e6, 0, 'f', 3)
                       .arg(a.rOhm + t * (b.rOhm - a.rOhm), 0, 'f', 1);
            break;
        }
        statusBar()->showMessage(msg, 15000);
    } else {
        statusBar()->showMessage(
            "SWR sweep stopped — carrier off, dial and settings restored",
            8000);
    }
    swrPts_.clear();
}

void MainWindow::refreshSwrOverlay() {
    if (curBand_ >= 0)
        pan_->setSwrRuns(swrRuns_.value(QLatin1String(kBands[curBand_].label)));
    else
        pan_->setSwrRuns({});
}

void MainWindow::saveSwrRuns() const {
    QJsonObject root;
    for (auto it = swrRuns_.cbegin(); it != swrRuns_.cend(); ++it) {
        QJsonArray runs;
        for (const auto& run : it.value()) {
            QJsonArray pts;
            // [Hz, SWR] as always, extended to [Hz, SWR, R, X] for points
            // measured with a vector meter. Old files (2-element points)
            // still load; old builds reading a new file take the first two
            // and ignore the rest, so the format degrades both ways.
            for (const auto& p : run.pts) {
                QJsonArray pa{double(p.hz), p.swr};
                if (p.zValid) { pa.append(p.rOhm); pa.append(p.xOhm); }
                pts.append(pa);
            }
            runs.append(QJsonObject{{"ts", double(run.ts)}, {"pts", pts}});
        }
        root[it.key()] = runs;
    }
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QFile f(dir + "/swr.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void MainWindow::loadSwrRuns() {
    QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + "/swr.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        QVector<PanadapterWidget::SwrRun> list;
        for (const QJsonValue& rv : it.value().toArray()) {
            PanadapterWidget::SwrRun run;
            const QJsonObject ro = rv.toObject();
            run.ts = qint64(ro.value("ts").toDouble());
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
            if (run.pts.size() >= 2) list.append(run);
        }
        if (!list.isEmpty()) swrRuns_[it.key()] = list;
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
