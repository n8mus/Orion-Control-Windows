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
        QTimer::singleShot(250, this, [this] {
            radio_->setPtt(false);                 // un-key BEFORE the mic is hot
            if (dvrAutoDig_) {
                dvrAutoDig_ = false;
                setDigitalMode(false);             // voice mic/proc come back
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
        txBar_->showSpeechProc(0);
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
