// SPDX-License-Identifier: GPL-3.0-or-later
// RTTY (2026-11-07): the MODE panel's RTTY pseudo-mode, the RTTY PNL deck
// under the pan, and the over — key, play, un-key when the last bit is on
// the air. The modem is RttyEngine (receive on the radio's audio through
// the CW reader's SignaLink capture, AFSK out through pacat); PTT, DIG and
// the radio's mode stay here, as for FreeDV.
#include "app/MainWindow.h"

#include <QBoxLayout>
#include <QDateTime>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

#include "app/MainWindowInternal.h"
#include "cw/AudioCwSource.h"
#include "log/QsoContext.h"
#include "rtty/RttyDeck.h"
#include "rtty/RttyEngine.h"
#include "util/LogbookIndex.h"

namespace ttc {

void MainWindow::setupRttyDeckUi() {
    auto* btn = new QToolButton(topStrip_);
    btn->setText("RTTY PNL");
    btn->setCheckable(true);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(QString(kSideBtnStyle));
    btn->setToolTip("RTTY panel: the waterfall hands its space to a RTTY deck —\n"
                    "reader with mark/space tuning, CALL/NAME/RST with LOG IT,\n"
                    "F1-F12 macros, a type-to-send pane. Opening it turns the\n"
                    "MODE panel's RTTY on. Press again for the waterfall.");
    topLay2_->addSpacing(8);
    topLay2_->addWidget(btn);
    connect(btn, &QToolButton::toggled, this, [this](bool on) { toggleRttyDeck(on); });
    rttyDeckBtn_ = btn;
}

bool MainWindow::rttyDeckVisible() const {
    return rttyDeck_ && rttyDeck_->isVisible();
}

// The modem and its ear. The SignaLink capture is the CW window's
// (AudioCwSource, built with it), shared through its raw tap.
void MainWindow::ensureRtty() {
    if (rtty_) return;
    ensureCwWindow();
    rtty_ = new RttyEngine(this);
    rtty_->setMyCall(QSettings().value("station/callsign", "").toString());
    if (audioSrc_)
        audioSrc_->setRawTap([this](const float* s, int n) { rtty_->feedAudio(s, n); });
    connect(rtty_, &RttyEngine::heard, this, [this](const QString& call, bool toMe) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (int i = rttyHeard_.size() - 1; i >= 0; --i)
            if (rttyHeard_[i].call == call) rttyHeard_.removeAt(i);
        rttyHeard_.push_back({call, qint64(centerHz_), now, toMe});
        if (pushSpots_) pushSpots_();
        if (toMe) statusBar()->showMessage(QString("RTTY: %1 is calling you").arg(call), 8000);
    });
    connect(rtty_, &RttyEngine::txDrained, this, [this] {
        if (!rttyTxOn_) return;
        radio_->setPtt(false);
        rttyTxOn_ = rttyHold_ = false;
        moxFailsafe_->stop();
        txBar_->showMoxActive(false);
        if (rttyDeck_) rttyDeck_->showTx(false);
        statusBar()->showMessage("RTTY: over sent, un-keyed", 3000);
    });
    connect(rtty_, &RttyEngine::txFailed, this, [this](const QString& why) {
        if (rttyTxOn_) radio_->setPtt(false);
        rttyTxOn_ = rttyHold_ = false;
        moxFailsafe_->stop();
        txBar_->showMoxActive(false);
        if (rttyDeck_) rttyDeck_->showTx(false);
        statusBar()->showMessage("RTTY TX failed: " + why, 8000);
    });
}

void MainWindow::toggleRttyDeck(bool on) {
    if (on && contestBtn_ && contestBtn_->isChecked()) contestBtn_->setChecked(false);
    if (on && cwDeckBtn_ && cwDeckBtn_->isChecked()) cwDeckBtn_->setChecked(false);
    if (on && !rttyMode_) {
        setRttyMode(true);                   // opens the deck itself
        if (!rttyMode_) {                    // refused: the button stays dark
            const QSignalBlocker b(rttyDeckBtn_);
            rttyDeckBtn_->setChecked(false);
            return;
        }
    }
    if (on) {
        if (!rttyDeck_) {
            ensureRtty();
            rttyDeck_ = new RttyDeck(rtty_, &logbook_, &cty_, &rotor_, qrz_, this);
            rttyDeck_->setContext(qsoContext());
            layout_->placeUnderPan(rttyDeck_);
            connect(rtty_, &RttyEngine::textDecoded, rttyDeck_, &RttyDeck::appendRead);
            connect(rttyDeck_, &RttyDeck::logRequested, this, [this] { logFromContext(); });
            connect(rttyDeck_, &RttyDeck::walkSpots, this, [this](int d) { walkContestSpot(d); });
            connect(rttyDeck_, &RttyDeck::rotorAimed, rose_, &CompassRoseWidget::pointBearing);
            connect(rttyDeck_, &RttyDeck::sendRequested, this,
                    [this](const QString& t, bool hold) { rttySend(t, hold); });
            connect(rttyDeck_, &RttyDeck::txHoldChanged, this, [this](bool hold) {
                if (hold) rttySend(QString(), true);
                else rttyEndOver();
            });
            connect(rttyDeck_, &RttyDeck::abortRequested, this, [this] { rttyAbort(); });
            connect(rttyDeck_, &RttyDeck::narrowRequested, this, [this](bool on) { rttySetNarrow(on); });
            connect(rttyDeck_, &RttyDeck::spotDxRequested, this,
                    [this](const QString& call, qint64 hz) {
                        statusBar()->showMessage(
                            spotClient_.spotDx(call, hz)
                                ? QString("spotted %1 at %2 kHz").arg(call).arg(hz / 1000.0, 0, 'f', 1)
                                : QStringLiteral("spot NOT sent — no logged-in cluster connection (SPOTS ▾)"),
                            6000);
                    });
            auto* feed = new QTimer(rttyDeck_);
            feed->setInterval(1000);
            connect(feed, &QTimer::timeout, rttyDeck_, [this] {
                if (rttyDeck_->isVisible()) rttyDeck_->setRig(qint64(centerHz_), QStringLiteral("RTTY"));
            });
            feed->start();
        }
        rttyDeck_->setRig(qint64(centerHz_), QStringLiteral("RTTY"));
        rttyDeck_->showNarrow(rttyNarrow_);
        savedSplit_ = pan_->displaySettings().split;
        DisplaySettings ds = pan_->displaySettings();
        ds.split = 1.0f;
        pan_->setDisplaySettings(ds);
        rttyDeck_->setVisible(true);
        pan_->installEventFilter(this);
    } else {
        if (rttyDeck_) rttyDeck_->setVisible(false);
        if (!contestDeckVisible() && !cwDeckVisible()) pan_->removeEventFilter(this);
        DisplaySettings ds = pan_->displaySettings();
        ds.split = savedSplit_ > 0.0f && savedSplit_ < 1.0f ? savedSplit_ : 0.42f;
        pan_->setDisplaySettings(ds);
    }
}

// The pseudo-mode. On: LSB (the RTTY convention on every band; USB/LSB
// flip it), the TX filter wide enough for the tones, DIG (line input),
// the modem listening on the radio's audio, the deck up. Off: everything
// back, SSB on the band's sideband unless a real mode was picked.
void MainWindow::setRttyMode(bool on, bool keepMode) {
    if (on == rttyMode_) { panel_->showRtty(on); return; }
    if (on) {
        if (winlinkActive_) {
            panel_->showRtty(false);
            statusBar()->showMessage("RTTY refused: a Winlink session owns the SignaLink", 8000);
            return;
        }
        if (wsjtx_ && wsjtxTransmitting()) {
            panel_->showRtty(false);
            statusBar()->showMessage("RTTY refused: WSJT-X is transmitting", 8000);
            return;
        }
        if (fdvMode_) setFdvMode(false, /*keepMode*/ true);
        if (samActive_) {
            samActive_ = false;
            panel_->showSam(false);
            panel_->setSamLabel("SAM");
        }
        rttySaved_ = {rigMode_, rigBwHz_, rigPbtHz_, txBwHz_, digital_};
        rttySb_ = Mode::LSB;
        applyMode(rttySb_);
        panel_->clearModeSelection();
        if (txBwHz_ < 2800) { radio_->setTxFilter(2800); txBar_->showTxFilter(2800); }
        if (liveMic_) setLiveMic(false);
        if (!digital_) setDigitalMode(true);
        rttyMode_ = true;
        ensureRtty();
        rtty_->setMyCall(QSettings().value("station/callsign", "").toString());
        rtty_->setRxEnabled(true);
        applyCwRxRouting();                  // the SignaLink capture runs
        if (rttyNarrow_) rttySetNarrow(true);
        panel_->showRtty(true);
        if (rttyDeckBtn_ && !rttyDeckBtn_->isChecked()) rttyDeckBtn_->setChecked(true);
        statusBar()->showMessage("RTTY: LSB, line input, reading the radio's audio. F-keys / Enter "
                                 "send an over; TX holds the key; MOX / space likewise.", 10000);
    } else {
        if (rttyTxOn_) rttyAbort();
        rttyMode_ = false;
        if (rtty_) rtty_->setRxEnabled(false);
        applyCwRxRouting();
        if (!rttySaved_.dig && digital_) setDigitalMode(false);
        if (rttySaved_.txBw != txBwHz_) {
            radio_->setTxFilter(rttySaved_.txBw);
            txBar_->showTxFilter(rttySaved_.txBw);
        }
        if (!keepMode) {
            Mode back = rttySaved_.mode;
            if (back == Mode::USB || back == Mode::LSB) back = fdvSideband(centerHz_);
            applyMode(back);
            panel_->showMode(back);
        }
        if (!keepMode || rttyNarrow_) {
            QTimer::singleShot(450, this, [this, bw = rttySaved_.bw, pbt = rttySaved_.pbt] {
                radio_->setBandwidthHz(Rx::Main, bw);
                radio_->setPbtHz(Rx::Main, pbt);
            });
        }
        panel_->showRtty(false);
        if (rttyDeckBtn_ && rttyDeckBtn_->isChecked()) rttyDeckBtn_->setChecked(false);
        statusBar()->showMessage("RTTY off — mode, filter, TX filter and input restored", 6000);
    }
}

// One over. Empty text with hold = MOX / the TX button: key up and diddle.
void MainWindow::rttySend(const QString& text, bool hold) {
    if (!rttyMode_) { statusBar()->showMessage("RTTY is off (MODE panel)", 4000); return; }
    if (winlinkActive_ || wsjtxTransmitting()) {
        statusBar()->showMessage("RTTY TX refused: Winlink / WSJT-X owns the transmitter", 6000);
        return;
    }
    if (radioSink_.isEmpty()) {
        statusBar()->showMessage("RTTY TX: radio sound device (SignaLink) not found", 6000);
        return;
    }
    ensureRtty();
    if (!rttyTxOn_) {
        if (!rtty_->start(radioSink_)) return;    // txFailed said why
        if (tuneTimeout_->isActive()) { txBar_->showTuneActive(false); stopManualTune(); }
        radio_->setPtt(true);
        rttyTxOn_ = true;
        moxFailsafe_->start();
        txBar_->showMoxActive(true);
        if (rttyDeck_) rttyDeck_->showTx(true);
    }
    if (hold) rttyHold_ = true;
    if (!text.isEmpty()) rtty_->send(text);
    if (!rttyHold_) rttyEndOver();
}

void MainWindow::rttyEndOver() {
    if (!rttyTxOn_ || !rtty_) return;
    rttyHold_ = false;
    rtty_->end();                            // txDrained drops PTT
    if (rttyDeck_) rttyDeck_->setStatus("TX · finishing the over…");
}

void MainWindow::rttyAbort() {
    if (rtty_) rtty_->abort();
    if (rttyTxOn_) radio_->setPtt(false);
    rttyTxOn_ = rttyHold_ = false;
    moxFailsafe_->stop();
    txBar_->showMoxActive(false);
    if (rttyDeck_) rttyDeck_->showTx(false);
}

// NARROW: 500 Hz on the tone pair. The console's passband model puts a
// filter's audio centre at bw/2 + PBT (MainWindowInternal.h), so PBT =
// (mark + shift/2) - 250; the nominal low-cut it cannot read may leave
// this a couple hundred Hz off — the PBT slider trims it, the deck's
// bars say when it is right.
void MainWindow::rttySetNarrow(bool on) {
    rttyNarrow_ = on;
    if (!rttyMode_ || !rtty_) return;
    if (on) {
        const auto& s = rtty_->settings();
        const int centre = s.markHz + s.shiftHz / 2;
        radio_->setBandwidthHz(Rx::Main, 500);
        radio_->setPbtHz(Rx::Main, std::min(8000, centre - 250));
    } else {
        radio_->setBandwidthHz(Rx::Main, rttySaved_.bw);
        radio_->setPbtHz(Rx::Main, rttySaved_.pbt);
    }
    if (rttyDeck_) rttyDeck_->showNarrow(on);
}

}  // namespace ttc
