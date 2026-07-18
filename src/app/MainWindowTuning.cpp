// SPDX-License-Identifier: GPL-2.0-or-later
// MainWindow tuning surface: click/zap/zero-beat tune paths, mode changes,
// passband + notch overlay math and the coalesced drag-to-filter streams.
// Split out of MainWindow.cpp so tuning work doesn't collide with UI work.
#include "app/MainWindow.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"
#include "cw/CwDecoder.h"
#include "net/FldigiClient.h"
#include "ui/DigiWindow.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ttc {

void MainWindow::onTuneRequested(int offsetHz, bool exact) {
    // fldigi click-to-carrier: radio in DIG with the DIGI window tracking,
    // and the click lands INSIDE the passband -> move fldigi's audio
    // cursor there and leave the radio alone (the panadapter acts as
    // fldigi's waterfall). Clicks outside the passband tune as usual;
    // Shift (exact) always tunes.
    if (!exact && digital_ && digiWin_ && digiWin_->isVisible()
        && digiWin_->trackClicks() && fldigi_ && fldigi_->connected()) {
        int lo = 0, hi = 0;
        edgesFromRig(rigMode_, rigBwHz_, rigPbtHz_, lo, hi);
        if (offsetHz >= lo && offsetHz <= hi) {
            const int audio = rigMode_ == Mode::LSB ? -offsetHz : offsetHz;
            if (audio > 0) {
                fldigi_->setCarrier(audio);
                statusBar()->showMessage(
                    QString("fldigi carrier -> %1 Hz").arg(audio), 4000);
                return;
            }
        }
    }
    // CW zap: a click is ~a pixel wide but the ear wants single-digit Hz.
    // Snap to the true carrier near the click; Shift (exact) bypasses.
    if (!exact && cwZap_
        && (rigMode_ == Mode::CWU || rigMode_ == Mode::CWL)) {
        const int snapped = snapToCwPeak(offsetHz, 150);
        if (snapped != offsetHz) {
            statusBar()->showMessage(
                QString("CW zap: snapped %1%2 Hz onto the carrier")
                    .arg(snapped > offsetHz ? "+" : "")
                    .arg(snapped - offsetHz), 4000);
            offsetHz = snapped;
        }
    }
    // Click-to-tune on the non-CW modes (SSB/AM/FM/digital): operators sit on
    // round kHz, but a pixel-click maps to an arbitrary Hz (click a 14.244
    // station, land on 14.244.340). Snap the absolute target to the nearest
    // 1 kHz so a click drops you right on frequency. Excludes the tight modes
    // (CW keeps its carrier zap above; SAM keeps a precise zero-beat) and any
    // exact tune — Shift+click for the odd-split station, and the wheel's
    // 100 Hz fine steps pass exact=true so they nudge off the round kHz.
    const bool tight = samActive_
                    || rigMode_ == Mode::CWU || rigMode_ == Mode::CWL;
    if (!exact && !tight) {
        const int64_t target = static_cast<int64_t>(centerHz_) + offsetHz;
        const int64_t snapped = ((target + 500) / 1000) * 1000;
        offsetHz = static_cast<int>(snapped - static_cast<int64_t>(centerHz_));
    }
    tuneAbsolute(centerHz_ + offsetHz);
}

// 0-BEAT (Z key): zap the strongest signal already inside the passband —
// PowerSDR's zero-beat button, fused with the same peak finder the click
// uses. One press runs up to three measure-tune-settle passes: the FFT is
// temporally averaged and the SDR LO moves with each correction, so the
// spectrum right after a retune is still smeared with pre-tune energy —
// a single measurement lands close, the follow-ups (on fresh frames ~700 ms
// apart) mop up the residual. Converges early when within a couple Hz;
// aborts if anything else retunes mid-sequence.
void MainWindow::zeroBeat() {
    if (rigMode_ != Mode::CWU && rigMode_ != Mode::CWL) {
        statusBar()->showMessage("0-beat (Z) works in CW modes", 3000);
        return;
    }
    // Pitch-first: an audible note near the target means the station is
    // already tuned — hand it straight to the servo (one small move) and
    // NEVER run the passband peak hunt, whose job is finding a signal you
    // don't have yet. Live-found: note at 530, zap's FFT hunt grabbed a
    // stronger neighbor, dial lurched to a 700 Hz note, servo rightly
    // refused the 150 Hz "correction" — stranded, when 20 Hz was the job.
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const int target = QSettings().value("cw/pitchHz", 550).toInt();
        if (lastPitchHz_ > 0.0 && now - lastPitchMs_ <= 1500
            && std::abs(lastPitchHz_ - target) <= PitchTrim::kMaxErrHz) {
            statusBar()->showMessage(
                QString("0-beat: note %1 Hz — trimming to %2…")
                    .arg(qRound(lastPitchHz_)).arg(target), 4000);
            zbPassesLeft_ = 0;             // no FFT passes on this path
            armPitchTrim();
            return;
        }
    }
    int lo = 0, hi = 0;
    edgesFromRig(rigMode_, rigBwHz_, rigPbtHz_, lo, hi);
    const int peak = snapToCwPeak((lo + hi) / 2, (hi - lo) / 2);
    tuneAbsolute(centerHz_ + peak);
    statusBar()->showMessage(
        QString("0-beat: %1%2 Hz onto the carrier, refining…")
            .arg(peak > 0 ? "+" : "").arg(peak), 4000);
    zbPassesLeft_ = 2;
    zbExpectHz_   = centerHz_;
    QTimer::singleShot(700, this, [this] { zeroBeatPass(); });
}

void MainWindow::zeroBeatPass() {
    if (zbPassesLeft_ <= 0) return;
    // User (or a spot click) moved the dial, or mode left CW: stand down.
    if (centerHz_ != zbExpectHz_
        || (rigMode_ != Mode::CWU && rigMode_ != Mode::CWL)) {
        zbPassesLeft_ = 0;
        return;
    }
    --zbPassesLeft_;
    // The carrier is near the dial now — a tight window so a louder
    // neighbor can't steal the refinement.
    const int err = snapToCwPeak(0, 75);
    if (std::abs(err) < 3) {                    // close enough to be inaudible
        zbPassesLeft_ = 0;
        statusBar()->showMessage("0-beat locked", 3000);
        armPitchTrim();
        return;
    }
    tuneAbsolute(centerHz_ + err);
    zbExpectHz_ = centerHz_;
    statusBar()->showMessage(
        QString("0-beat refine: %1%2 Hz%3").arg(err > 0 ? "+" : "").arg(err)
            .arg(zbPassesLeft_ > 0 ? "…" : " — done"), 3000);
    if (zbPassesLeft_ > 0)
        QTimer::singleShot(700, this, [this] { zeroBeatPass(); });
    else
        armPitchTrim();
}

// Final 0-beat stage — the closed loop. The FFT passes land the dial where
// the SDR says the carrier is, but the SDR and the radio disagree by a
// small reference offset, so the NOTE can still sit 10-20 Hz off the
// operator's pitch. The pitch meter measures the note in the radio's own
// audio frame (the only frame that matters); PitchTrim (dsp/PitchTrim.h)
// walks it onto cw/pitchHz in small quantized steps and stops on its own
// when the station stops. This replaces a one-throw-full-error version
// that rocked the dial in big swings off stale samples (live report:
// "I can see 400 then 650").
void MainWindow::armPitchTrim() {
    if (rigMode_ != Mode::CWU && rigMode_ != Mode::CWL) return;
    pitchTrim_.arm(QSettings().value("cw/pitchHz", 550).toInt(),
                   QDateTime::currentMSecsSinceEpoch());
    zbExpectHz_ = centerHz_;
    // Heartbeat: the pitch stream only flows while the CW-window capture
    // runs; these invalid ticks guarantee the no-signal timeout fires
    // even when it doesn't (capture off, station gone silently).
    const auto beat = [this](auto&& self) -> void {
        if (!pitchTrim_.active()) return;
        pitchTrimFeed(-1.0);
        QTimer::singleShot(1200, this, [this, self] { self(self); });
    };
    QTimer::singleShot(1200, this, [this, beat] { beat(beat); });
}

// Every pitchMeasured() emission (~4/s, -1 when no tone) lands here; the
// servo decides, this applies. Sign: CWU higher note = carrier above the
// dial, CWL mirrored.
void MainWindow::pitchTrimFeed(double hz) {
    if (!pitchTrim_.active()) return;
    if ((rigMode_ != Mode::CWU && rigMode_ != Mode::CWL)
        || centerHz_ != zbExpectHz_) {         // operator moved on: stand down
        pitchTrim_.cancel();
        return;
    }
    const auto d =
        pitchTrim_.feed(hz, QDateTime::currentMSecsSinceEpoch());
    switch (d.what) {
    case PitchTrim::Decision::Move: {
        const int delta = rigMode_ == Mode::CWU ? d.deltaHz : -d.deltaHz;
        tuneAbsolute(centerHz_ + delta);
        zbExpectHz_ = centerHz_;
        statusBar()->showMessage(
            QString("0-beat trim %1%2 Hz…").arg(delta > 0 ? "+" : "")
                .arg(delta), 3000);
        break;
    }
    case PitchTrim::Decision::Done:
        statusBar()->showMessage(
            QString("0-beat: note on %1 Hz")
                .arg(QSettings().value("cw/pitchHz", 550).toInt()), 4000);
        break;
    case PitchTrim::Decision::Fail:
        if (qstrcmp(d.why, "no signal") == 0)
            statusBar()->showMessage("0-beat: signal gone — holding", 3000);
        break;                                 // other reasons: leave quietly
    case PitchTrim::Decision::None:
        break;
    }
}

// CW⇄CWR flip (X key): the aural zero-beat check — the BFO mirrors around
// the dial, so if the note's pitch doesn't change across the flip, the dial
// is exactly on the carrier. Works by ear with no spectrum at all; the
// verifier for signals too weak for the FFT peak to be trusted.
void MainWindow::flipCwSideband() {
    if (rigMode_ == Mode::CWU || rigMode_ == Mode::CWL) {
        const Mode other = rigMode_ == Mode::CWU ? Mode::CWL : Mode::CWU;
        applyMode(other);
        panel_->showMode(other);
        statusBar()->showMessage(
            QString("CW flip -> %1 (same pitch both ways = zero-beat)")
                .arg(other == Mode::CWU ? "CWU" : "CWL"), 4000);
    } else {
        statusBar()->showMessage("CW/CWR flip (X) works in CW modes", 3000);
    }
}

// Find the true carrier near offsetHz (relative to the dial): strongest bin
// within ±windowHz in the averaged display spectrum (averaging matters — it
// steadies the peak against keying and QSB, the same reason PowerSDR's
// zero-beat reads its averaged buffer), refined to sub-bin accuracy with a
// parabola through the three bins at the peak (~±5 Hz at 61 Hz bins). Both
// Ten-Tecs read the carrier on the dial in CW — dial onto the carrier means
// the note sits exactly on the sidetone/SPOT pitch, no offset arithmetic.
int MainWindow::snapToCwPeak(int offsetHz, int windowHz) const {
    const int n = static_cast<int>(lastSpectrum_.size());
    if (n < 8 || sdrSpanHz_ <= 0) return offsetHz;      // no SDR: tune as clicked
    const double binHz = double(sdrSpanHz_) / n;
    const auto binOf = [&](int off) {
        return n / 2 + static_cast<int>(std::lround((off - loOffHz_) / binHz));
    };
    const int i0 = std::clamp(binOf(offsetHz - windowHz), 1, n - 2);
    const int i1 = std::clamp(binOf(offsetHz + windowHz), i0, n - 2);
    int ip = i0;
    for (int i = i0; i <= i1; ++i)
        if (lastSpectrum_[i] > lastSpectrum_[ip]) ip = i;
    const double ym = lastSpectrum_[ip - 1], y0 = lastSpectrum_[ip],
                 yp = lastSpectrum_[ip + 1];
    const double den = ym - 2.0 * y0 + yp;
    const double d = std::abs(den) > 1e-9
        ? std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5) : 0.0;
    // Bin i sits at (i - n/2)*binHz from the SDR's LO, which is loOffHz_
    // above the dial (fixed 60 kHz classic, varying under CTUN) — same
    // mapping as binOf, inverted.
    return static_cast<int>(std::lround((ip - n / 2 + d) * binHz)) + loOffHz_;
}

void MainWindow::tuneAbsolute(uint64_t f) {
    if (vfoLockA_) {                               // every console A-tune funnels
        statusBar()->showMessage("VFO A is LOCKED");  // here: one guard covers all
        return;
    }
    // A band recording's header freezes the capture LO — a retune would
    // silently corrupt the file's frequency mapping, so close it out.
    if (recIqAct_ && recIqAct_->isChecked()) recIqAct_->setChecked(false);
    f = std::clamp<uint64_t>(f, 100000, 60000000);
    const uint64_t prevDial = centerHz_;       // for CTUN's view bookkeeping
    radio_->setFrequencyHz(Rx::Main, f);
    rigctld_.cacheFrequency(f);
    centerHz_ = f;                              // recenter view on the new frequency
    pendingHz_ = f;
    sinceTune_.restart();                       // hold off dial-follow while it settles
    freqDisp_->setFrequency(f);
    pan_->setCenterHz(f);                       // keep grid labels on the dial
    syncBandRegister();                         // mirror the move into the stack
    // Mode follows the band plan — but ONLY for console-commanded tunes
    // (every one funnels through here: clicks, spot clicks, band buttons,
    // freq entry). Dial-follow from the radio's knob never consults this,
    // so the radio can always overrule. Digital sessions and SAM own their
    // mode, a deliberate X-key CWL flip counts as CW and is left alone,
    // and out-of-band/60 m frequencies change nothing. Escape hatch:
    // QSettings tune/modeByFreq=false.
    if (!digital_ && !samActive_
        && QSettings().value("tune/modeByFreq", true).toBool()) {
        bool known = false;
        const Mode pm = planModeForFreq(f, known);
        const bool cwAlready = pm == Mode::CWU
            && (rigMode_ == Mode::CWU || rigMode_ == Mode::CWL);
        if (known && pm != rigMode_ && !cwAlready)
            QTimer::singleShot(120, this, [this, pm] {  // trail the retune:
                applyMode(pm);                          // the Orion drops cmds
                panel_->showMode(pm);                   // while it's busy
            });
    }
    retuneSdrFor(f, prevDial);
    statusBar()->showMessage(QString("tune -> %1 MHz").arg(f / 1e6, 0, 'f', 6));
}

// How far the dial may drift from the capture LO before CTUN gives up and
// re-centers: the RSP2 grabs ±250 kHz around the LO; 190 kHz leaves room
// for the passband and keeps the view clear of the capture edges.
static constexpr int kCtunEdgeHz = 190000;

// One LO policy for every dial move (console tunes and radio-knob follow).
// Classic: the LO tracks the dial at +kLoOffsetHz, view centered on the
// dial. CTUN: the LO (and the screen) hold still while the dial floats,
// PowerSDR-style; the view shift eats the dial delta so the spectrum
// doesn't move, and the display re-centers when the marker nears the view
// edge or the dial nears the capture edge.
void MainWindow::retuneSdrFor(uint64_t dial, uint64_t prevDial) {
#ifdef HAVE_SDRPLAY
    if (ctun_ && sdrLoHz_ != 0) {
        const int64_t off = int64_t(sdrLoHz_) - int64_t(dial);   // LO - dial
        if (off >= -kCtunEdgeHz && off <= kCtunEdgeHz) {
            setLoOff(int(off));                    // capture untouched
            int shift = pan_->viewShiftHz()
                + int(int64_t(prevDial) - int64_t(dial));
            // PowerSDR edge rule: as the marker nears the display edge,
            // re-center so tuning is continuous.
            if (std::abs(shift) > pan_->viewSpanHz() * 42 / 100) shift = 0;
            pan_->setViewShiftHz(shift);
            return;
        }
    }
    sdrLoHz_ = dial + kLoOffsetHz;                 // classic re-center
    sdr_.setCenterFrequency(static_cast<double>(sdrLoHz_));
    pan_->setViewShiftHz(0);
    setLoOff(kLoOffsetHz);
#else
    (void)dial; (void)prevDial;
#endif
}

void MainWindow::setLoOff(int off) {
    if (off == loOffHz_) return;
    loOffHz_ = off;
    pan_->setDialOffsetHz(off);
    // The CW reader listens at the dial's offset from the LO — follow it.
    if (cwDec_) cwDec_->retune(-double(off));
}

void MainWindow::applyMode(Mode m) {
    radio_->setMode(Rx::Main, m);
    rigMode_ = m;
    // Prime the poll debounce with the TARGET: steady-state polls keep
    // pendingPolledMode_ equal to the old mode, so a stale report already
    // in flight would count as "seen twice" and revert this set on the
    // spot (live-observed: every console mode press bounced straight back).
    pendingPolledMode_ = m;
    rigctld_.cacheMode(m);
    refreshPassbandOverlay();
    refreshNotchOverlay();
    // The Orion recalls its per-mode stored filter on a mode change — fetch it
    // as soon as the firmware has settled instead of waiting for the slow poll.
    QTimer::singleShot(400, this, [this] { radio_->queryFilter(Rx::Main); });
}
// VFO B's on-screen representation: dial + sub-RX filter edges + TX state.
// Sideband placement uses the main mode — the radio is set to copy the mode
// to the sub, and v3 firmware can't set sub mode over CAT anyway.
void MainWindow::pushVfoB() {
    int lo = 0, hi = 0;
    edgesFromRig(rigMode_, subBwHz_, subPbtHz_, lo, hi);
    const char role = txVfo_ == 'B' ? 'T' : (rxVfo_ == 'B' ? 'R' : 'N');
    pan_->setVfoB(vfoBHz_, role, lo, hi);
}

void MainWindow::refreshPassbandOverlay() {
    // SSB bandwidth grows away from the carrier (edgesFromRig): tell the
    // panadapter which edge a pure-BW drag pins so the preview matches.
    pan_->setBwAnchor(rigMode_ == Mode::USB ? -1
                      : rigMode_ == Mode::LSB ? +1 : 0);
    // Don't snap the overlay out from under the user right after they dragged it.
    if (sinceFilterEdit_.isValid() && sinceFilterEdit_.elapsed() < 2000) return;
    int lo = 0, hi = 0;
    edgesFromRig(rigMode_, rigBwHz_, rigPbtHz_, lo, hi);
    pan_->setPassband(lo, hi);
}

void MainWindow::onPassbandChanged(int loHz, int hiHz) {
    // Delta from the drag anchor, applied to the radio's real anchored state.
    // The PBT delta is the exact inverse of edgesFromRig, so the (bw, pbt)
    // sent reproduces the drawn edges: in SSB the nominal placement rides
    // the zero-beat edge (USB lo = pbt, LSB hi = -pbt), elsewhere the center.
    // In AM the drawn width is TWICE the radio's bandwidth number (both
    // sidebands), so the width delta halves on the way back to the radio.
    int dWidth = (hiHz - loHz) - (anchorHiHz_ - anchorLoHz_);
    if (rigMode_ == Mode::AM) dWidth /= 2;
    int dPbt;
    switch (rigMode_) {
        case Mode::USB: dPbt = loHz - anchorLoHz_;    break;
        case Mode::LSB: dPbt = anchorHiHz_ - hiHz;    break;
        default:
            dPbt = pbtRfSign(rigMode_)
                   * (((hiHz + loHz) - (anchorHiHz_ + anchorLoHz_)) / 2);
            break;
    }
    pendBwHz_  = std::clamp(anchorBwHz_ + dWidth, 100, bwMaxFor(rigMode_));
    pendPbtHz_ = std::clamp(anchorPbtHz_ + dPbt, -8000, 8000);
    filterDirty_ = true;
    sinceFilterEdit_.restart();
    if (!filterTx_->isActive()) filterTx_->start();  // coalesce to ~25 writes/sec
    statusBar()->showMessage(QString("filter -> bw %1 Hz  pbt %2 Hz")
                                 .arg(pendBwHz_).arg(pendPbtHz_));
}

// The notch is an audio-domain DSP filter: an RF signal at offset d from the
// carrier demodulates to audio |d| regardless of PBT (PBT moves the passband
// filter, not the BFO). So marker RF offset = sideband sign x audio center.
void MainWindow::refreshNotchOverlay() {
    pan_->setNotch(notchOn_, pbtRfSign(rigMode_) * notchCenter_, notchWidth_, safOn_);
}

// One place derives every notch/SAF display from the state trio: the marker
// draws whenever the engine runs (green when peaking), the NOTCH button only
// lights when the engine is in reject flavor.
void MainWindow::syncNotchUi() {
    refreshNotchOverlay();
    panel_->showNotch(notchOn_ && !safOn_, notchCenter_, notchWidth_);
    panel_->showSaf(safOn_);
}

void MainWindow::sendPendingNotch() {
    if (!notchDirty_) return;
    notchDirty_ = false;
    radio_->setNotchCenter(Rx::Main, pendNotchHz_);  // -> *RMNC (clamped in driver)
    statusBar()->showMessage(QString("notch -> %1 Hz x %2 Hz")
                                 .arg(pendNotchHz_).arg(notchWidth_));
}

void MainWindow::sendPendingFilter() {
    if (!filterDirty_) return;
    filterDirty_ = false;
    radio_->setBandwidthHz(Rx::Main, pendBwHz_);     // -> *RMF
    radio_->setPbtHz(Rx::Main, pendPbtHz_);          // -> *RMP
    rigBwHz_  = pendBwHz_;                          // optimistic; poll will confirm
    rigPbtHz_ = pendPbtHz_;
    rigctld_.cacheBandwidth(pendBwHz_);
    panel_->showPbt(pendPbtHz_);
}


} // namespace ttc
