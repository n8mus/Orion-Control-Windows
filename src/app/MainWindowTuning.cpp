// SPDX-License-Identifier: GPL-2.0-or-later
// MainWindow tuning surface: click/zap/zero-beat tune paths, mode changes,
// passband + notch overlay math and the coalesced drag-to-filter streams.
// Split out of MainWindow.cpp so tuning work doesn't collide with UI work.
#include "app/MainWindow.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"
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
    zbPitchTries_ = 3;                 // arm the pitch-trim final stage
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
        QTimer::singleShot(900, this, [this] { zeroBeatPitchTrim(); });
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
        QTimer::singleShot(900, this, [this] { zeroBeatPitchTrim(); });
}

// Final 0-beat stage — the closed loop. The FFT passes land the dial where
// the SDR says the carrier is, but the SDR and the radio disagree by a
// small reference offset, so the NOTE can still sit 10-20 Hz off the
// operator's pitch. The pitch meter measures the note in the radio's own
// audio frame (the only frame that matters), and one signed nudge lands
// it ON cw/pitchHz — the readout goes green as the zap finishes. Runs
// only with a fresh reading (CW window capture live + station keying);
// corrections outside 3..100 Hz are refused (too small to matter / too
// likely a different station).
void MainWindow::zeroBeatPitchTrim() {
    if (zbPitchTries_ <= 0) return;
    if (rigMode_ != Mode::CWU && rigMode_ != Mode::CWL) return;
    if (centerHz_ != zbExpectHz_) return;      // operator moved on
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastPitchHz_ < 0.0 || now - lastPitchMs_ > 1500) {
        if (--zbPitchTries_ > 0)               // wait for the next keying
            QTimer::singleShot(700, this, [this] { zeroBeatPitchTrim(); });
        return;
    }
    const int target = QSettings().value("cw/pitchHz", 550).toInt();
    const double err = lastPitchHz_ - target;
    if (std::abs(err) < 3.0) {
        zbPitchTries_ = 0;
        statusBar()->showMessage(
            QString("0-beat: note on %1 Hz").arg(target), 4000);
        return;
    }
    if (std::abs(err) > 100.0) {
        zbPitchTries_ = 0;
        return;                                // not our station — leave it
    }
    // CWU: higher note = carrier above the dial; CWL mirrored.
    const double delta = rigMode_ == Mode::CWU ? err : -err;
    tuneAbsolute(centerHz_ + qint64(std::lround(delta)));
    zbExpectHz_ = centerHz_;
    statusBar()->showMessage(
        QString("0-beat pitch trim %1%2 Hz").arg(delta > 0 ? "+" : "")
            .arg(qRound(delta)), 4000);
    if (--zbPitchTries_ > 0)                   // one confirming look
        QTimer::singleShot(900, this, [this] { zeroBeatPitchTrim(); });
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
        return n / 2 + static_cast<int>(std::lround((off - kLoOffsetHz) / binHz));
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
    // Bin i sits at (i - n/2)*binHz from the SDR's LO, which is kLoOffsetHz
    // above the dial — same mapping as binOf, inverted.
    return static_cast<int>(std::lround((ip - n / 2 + d) * binHz)) + kLoOffsetHz;
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
    radio_->setFrequencyHz(Rx::Main, f);
    rigctld_.cacheFrequency(f);
    centerHz_ = f;                              // recenter view on the new frequency
    pendingHz_ = f;
    sinceTune_.restart();                       // hold off dial-follow while it settles
    freqDisp_->setFrequency(f);
    pan_->setCenterHz(f);                       // keep grid labels on the dial
    syncBandRegister();                         // mirror the move into the stack
#ifdef HAVE_SDRPLAY
    sdr_.setCenterFrequency(static_cast<double>(f + kLoOffsetHz));
#endif
    statusBar()->showMessage(QString("tune -> %1 MHz").arg(f / 1e6, 0, 'f', 6));
}

void MainWindow::applyMode(Mode m) {
    radio_->setMode(Rx::Main, m);
    rigMode_ = m;
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
