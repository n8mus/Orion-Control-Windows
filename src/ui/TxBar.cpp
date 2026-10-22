// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/TxBar.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <algorithm>

namespace ttc {

TxBar::TxBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(40);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x10, 0x15, 0x1c));
    setPalette(pal);
    setStyleSheet(R"(
        QLabel { color: #8fa3b8; font-size: 10px; font-weight: bold; }
        QLabel[valueLabel="true"] { color: #c8d4e0; font-weight: normal; font-size: 11px; }
        QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                      border-radius: 3px; font-size: 11px; min-height: 24px;
                      padding: 0 10px; }
        QPushButton:hover { background: #26303e; }
        QPushButton:checked { background: #2b5c8a; color: #ffffff; border-color: #3f7cb4; }
        QPushButton#amp:checked { background: #8a5c2b; border-color: #b47c3f; }
        QPushButton#prof { padding: 0 7px; font-size: 10px; border-radius: 8px; }
        QPushButton#prof:checked { background: #1f7a45; border-color: #3ecf7a; }
        QPushButton#dvr { padding: 0 7px; font-size: 10px; border-radius: 8px; }
        QPushButton#dvr[dvrState="tx"]   { background: #8a2727; color: #ffffff;
                                           border-color: #e05d5d; }
        QPushButton#dvr[dvrState="rec"]  { background: #1f4f8a; color: #ffffff;
                                           border-color: #4f8fe0; }
        QPushButton#dvr[dvrState="play"] { background: #1f7a45; color: #ffffff;
                                           border-color: #3ecf7a; }
        QPushButton#dvr[vkEmpty="true"] { color: #5c6b7a; }
        QPushButton#live:checked { background: #8a5c2b; color: #ffffff;
                                   border-color: #e8b04a; }
        QSpinBox { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                   font-size: 11px; }
        QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }
        QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;
                                     background: #6aa5d8; }
    )");

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(8, 2, 8, 2);
    lay->setSpacing(6);
    // The bar is a row of SECTIONS (power, tune, mic/TX audio, profiles,
    // DVR/voice keyer, LIVE), each its own sub-row with the bar's spacing,
    // so a layout can lift one out whole (the Panel layout's sidebar) while
    // Classic draws exactly the row it always did.
    QHBoxLayout* cur = nullptr;
    const auto section = [&](Section id) {
        auto* sec = new QHBoxLayout;
        sec->setSpacing(6);
        lay->addLayout(sec);
        sections_[id] = sec;
        return sec;
    };

    cur = section(Power);
    cur->addWidget(new QLabel("PWR"));
    pwr_ = makeSlider("PWR", pwrVal_, pwrTx_, pendPwr_, &TxBar::txPowerChanged);
    pwr_->setFixedWidth(120);
    cur->addWidget(pwr_);
    cur->addWidget(pwrVal_);

    ampBtn_ = new QPushButton("AMP");
    ampBtn_->setObjectName("amp");
    ampBtn_->setCheckable(true);
    ampBtn_->setFocusPolicy(Qt::NoFocus);
    ampBtn_->setToolTip("Amplifier mode: cap drive at the limit and enforce it");
    ampLimit_ = new QSpinBox;
    ampLimit_->setRange(5, 100);
    ampLimit_->setValue(40);
    ampLimit_->setSuffix("W");
    ampLimit_->setEnabled(false);
    ampLimit_->setFocusPolicy(Qt::ClickFocus);
    cur->addWidget(ampBtn_);
    cur->addWidget(ampLimit_);
    connect(ampBtn_, &QPushButton::toggled, this, [this](bool on) {
        ampLimit_->setEnabled(on);
        applyPowerCap();
        emit ampModeChanged(on, ampLimit_->value());
    });
    connect(ampLimit_, &QSpinBox::valueChanged, this, [this](int lim) {
        applyPowerCap();
        emit ampModeChanged(ampBtn_->isChecked(), lim);
    });

    lay->addSpacing(8);
    cur = section(Tune);
    tuneBtn_ = new QPushButton("TUNE");
    tuneBtn_->setCheckable(true);                  // latches while the carrier is up
    tuneBtn_->setFocusPolicy(Qt::NoFocus);
    tuneBtn_->setToolTip("Steady carrier at the set watts for tuning the amp or an "
                         "external tuner (runs the internal tuner cycle if TUNER is on).\n"
                         "Right-click: SWR sweep of the visible span, plotted on the pan.");
    tuneBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tuneBtn_, &QPushButton::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                emit tuneContextRequested(tuneBtn_->mapToGlobal(pos));
            });
    tuneLvl_ = new QSpinBox;
    tuneLvl_->setRange(5, 100);
    tuneLvl_->setValue(20);
    tuneLvl_->setSuffix("W");
    tuneLvl_->setFocusPolicy(Qt::ClickFocus);
    tuneLvl_->setToolTip("Manual tune carrier power");
    tunerBtn_ = new QPushButton("TUNER");
    tunerBtn_->setCheckable(true);
    tunerBtn_->setFocusPolicy(Qt::NoFocus);
    tunerBtn_->setToolTip(
        "Enable the internal tuner — then press TUNE to run a match cycle\n"
        "(the radio makes its own carrier and drops it when matched).\n"
        "Leave off with an external tuner.");
    // MOX: manual transmit for voice — keys the rig in the current (SSB)
    // mode with no carrier, so the mic / TRIP audio goes out. Latches until
    // clicked again (the handler adds a runaway-carrier failsafe).
    moxBtn_ = new QPushButton("MOX");
    moxBtn_->setObjectName("mox");
    moxBtn_->setCheckable(true);
    moxBtn_->setFocusPolicy(Qt::NoFocus);
    moxBtn_->setToolTip("Manual transmit (voice): key the rig so the mic — or, "
                        "in remote mode, the TRIP audio stream — goes on the "
                        "air. Click again to un-key.\n"
                        "Space bar does the same (not while typing in a box).");
    cur->addWidget(tuneBtn_);
    cur->addWidget(tuneLvl_);
    cur->addWidget(tunerBtn_);
    cur->addWidget(moxBtn_);
    connect(tuneBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit tuneToggled(on); });
    connect(moxBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit moxToggled(on); });
    connect(tuneLvl_, &QSpinBox::valueChanged, this,
            [this](int w) { emit tuneLevelChanged(w); });
    connect(tunerBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit tunerEnableToggled(on); });

    lay->addSpacing(8);
    cur = section(Audio);
    cur->addWidget(new QLabel("MIC"));
    mic_ = makeSlider("MIC", micVal_, micTx_, pendMic_, &TxBar::micGainChanged);
    cur->addWidget(mic_);
    cur->addWidget(micVal_);

    // TX audio shaping: the Orion's whole CAT-visible surface is the TX
    // filter bandwidth (900-3900 Hz continuous, hi-cut in effect — the low
    // corner is fixed in firmware, no EQ/rolloff commands exist) and the
    // speech processor level.
    cur->addSpacing(8);
    cur->addWidget(new QLabel("TX BW"));
    txbw_ = makeSlider("TXBW", txbwVal_, txbwTx_, pendTxbw_, &TxBar::txFilterChanged);
    {
        const QSignalBlocker b(txbw_);             // range change must not command
        txbw_->setRange(900, 3900);                // the radio before its state is in
        txbw_->setSingleStep(50);
    }
    txbw_->setToolTip("SSB transmit filter bandwidth (Orion range 900-3900 Hz)");
    txbwVal_->setFixedWidth(32);
    cur->addWidget(txbw_);
    cur->addWidget(txbwVal_);

    cur->addWidget(new QLabel("PROC"));
    proc_ = makeSlider("PROC", procVal_, procTx_, pendProc_, &TxBar::speechProcChanged);
    {
        const QSignalBlocker b(proc_);
        proc_->setRange(0, 9);
    }
    proc_->setFixedWidth(56);
    proc_->setToolTip("Speech processor level (0 = off)");
    connect(proc_, &QSlider::valueChanged, this,   // after makeSlider's handler:
            [this](int v) { if (v == 0) procVal_->setText("OFF"); });
    cur->addWidget(proc_);
    cur->addWidget(procVal_);

    // TX profiles: one click mid-pileup, no popup. The lit button shows the
    // active profile; touching any TX slider clears it (settings have drifted).
    lay->addSpacing(10);
    cur = section(Profiles);
    static const char* kProfNames[4] = {"RAG", "DX", "CONT", "ESSB"};
    static const char* kProfTips[4]  = {"normal ragchew", "DX / pileup",
                                        "contest", "wide clean audio"};
    for (int i = 0; i < 4; ++i) {
        auto* b = new QPushButton(kProfNames[i]);
        b->setObjectName("prof");
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(QString("TX profile: %1\nclick = recall   right-click = "
                              "save current TX BW/PROC/MIC/PWR here").arg(kProfTips[i]));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        profBtn_[i] = b;
        cur->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] {
            markProfile(i);
            emit profileRecalled(i);
        });
        connect(b, &QPushButton::customContextMenuRequested, this, [this, i] {
            markProfile(i);                        // saved = current, so it's active
            emit profileSaveRequested(i);
        });
    }
    // Manual edits to anything a profile bundles un-light the profile button.
    for (QSlider* s : {pwr_, mic_, txbw_, proc_})
        connect(s, &QSlider::valueChanged, this, [this] { markProfile(-1); });

    // DVR (Record/Playback + VKx voice keyer idea): REC/PLAY work the off-air
    // deck, VK1-4 are canned voice messages. Left-click always TRANSMITS.
    // PLAY: right-click listens on the PC speakers. VK: right-click
    // RECORDS, Shift + either click listens. Lights: red = on the air,
    // blue = recording, green = speakers (operator's scheme, 2026-10-14). Buttons only emit intents —
    // MainWindow lights them back through showDvr*() once the deck confirms,
    // so a click can never show a state the audio engine isn't actually in.
    lay->addSpacing(10);
    cur = section(Voice);
    cur->addWidget(new QLabel("DVR"));
    recBtn_ = new QPushButton("REC");
    recBtn_->setObjectName("dvr");
    recBtn_->setFocusPolicy(Qt::NoFocus);
    recBtn_->setToolTip(
        "Off-air recorder\n"
        "click:  start recording the receiver audio (button goes blue)\n"
        "click again:  stop and save — the take is auto-leveled for retransmit\n"
        "Each take is its own timestamped file; PLAY uses the newest one.");
    cur->addWidget(recBtn_);
    connect(recBtn_, &QPushButton::clicked, this,
            [this] { emit dvrRecordClicked(); });
    playBtn_ = new QPushButton("PLAY");
    playBtn_->setObjectName("dvr");
    playBtn_->setFocusPolicy(Qt::NoFocus);
    playBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
    playBtn_->setToolTip(
        "Off-air playback\n"
        "left-click:  TRANSMIT the newest take (red) — rig switches to\n"
        "    line-in, keys up, plays it over the air, restores your voice setup\n"
        "right-click:  LISTEN to it on the PC speakers (green, nothing\n"
        "    transmitted)\n"
        "click while lit:  stop / abort");
    cur->addWidget(playBtn_);
    connect(playBtn_, &QPushButton::clicked, this,
            [this] { emit dvrPlayClicked(true); });
    connect(playBtn_, &QPushButton::customContextMenuRequested, this,
            [this] { emit dvrPlayClicked(false); });
    for (int i = 0; i < 4; ++i) {
        auto* b = new QPushButton(QString("VK%1").arg(i + 1));
        b->setObjectName("dvr");
        b->setFocusPolicy(Qt::NoFocus);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        vkBtn_[i] = b;
        cur->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] {
            if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
                emit vkPreviewClicked(i);
            else
                emit vkClicked(i);
        });
        connect(b, &QPushButton::customContextMenuRequested, this, [this, i] {
            if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
                emit vkPreviewClicked(i);
            else
                emit vkRecordClicked(i);
        });
    }
    // LIVE: the PC mic through the voice chain into the SignaLink, the
    // radio on its line input; the operator keys with his own PTT.
    lay->addSpacing(6);
    cur = section(Live);
    liveBtn_ = new QPushButton("LIVE");
    liveBtn_->setObjectName("live");
    liveBtn_->setCheckable(true);
    liveBtn_->setFocusPolicy(Qt::NoFocus);
    liveBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
    liveBtn_->setToolTip(
        "Live mic through the PC (amber while on)\n"
        "click:  your PC mic (Setup ▸ Voice keyer mic) goes through the\n"
        "    voice EQ & dynamics into the SignaLink; the radio switches\n"
        "    to its rear line input (DIG lights). Key with your own PTT.\n"
        "    Needs the SignaLink's PTT jumper OUT.\n"
        "click again:  front mic and your voice settings come back\n"
        "right-click:  the EQ & dynamics window");
    cur->addWidget(liveBtn_);
    connect(liveBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit liveMicToggled(on); });
    connect(liveBtn_, &QPushButton::customContextMenuRequested, this,
            [this] { emit liveMicMenuRequested(); });
    lay->addStretch(1);
}

void TxBar::showLiveMic(bool on) {
    const QSignalBlocker b(liveBtn_);
    liveBtn_->setChecked(on);
}

void TxBar::markProfile(int slot) {
    for (int j = 0; j < 4; ++j) {
        const QSignalBlocker b(profBtn_[j]);
        profBtn_[j]->setChecked(j == slot);
    }
}

void TxBar::setCatTxControlsEnabled(bool on) {
    const QString tip = on ? QString()
                           : QStringLiteral("Not controllable on this radio over "
                                            "CAT (Omni: REMOTE mode only)");
    const std::initializer_list<QWidget*> ws = {
        pwr_, pwrVal_, ampBtn_, ampLimit_, tuneBtn_, tuneLvl_, tunerBtn_,
        mic_, micVal_, txbw_, txbwVal_, proc_, procVal_,
        profBtn_[0], profBtn_[1], profBtn_[2], profBtn_[3]};
    for (QWidget* w : ws) {
        w->setEnabled(on);
        if (!on) w->setToolTip(tip);
    }
}

QSlider* TxBar::makeSlider(const QString&, QLabel*& value, QTimer*& tx, int& pend,
                           void (TxBar::*sig)(int)) {
    auto* s = new QSlider(Qt::Horizontal);
    s->setRange(0, 100);
    s->setFixedWidth(90);
    value = new QLabel("--");
    value->setProperty("valueLabel", true);
    value->setFixedWidth(26);
    tx = new QTimer(this);
    tx->setSingleShot(true);
    tx->setInterval(40);
    connect(tx, &QTimer::timeout, this, [this, &pend, sig] {
        if (pend >= 0) { emit (this->*sig)(pend); pend = -1; }
    });
    connect(s, &QSlider::valueChanged, this, [this, value, &pend, tPtr = tx](int v) {
        value->setText(QString::number(v));
        pend = v;
        if (!tPtr->isActive()) tPtr->start();
    });
    return s;
}

bool TxBar::ampMode() const { return ampBtn_->isChecked(); }
int  TxBar::ampLimit() const { return ampLimit_->value(); }
int  TxBar::tuneLevel() const { return tuneLvl_->value(); }

void TxBar::setTuneLevel(int watts) {
    QSignalBlocker block(tuneLvl_);
    tuneLvl_->setValue(std::clamp(watts, 5, 100));
}

void TxBar::showTuneActive(bool on) {
    QSignalBlocker block(tuneBtn_);
    tuneBtn_->setChecked(on);
}

void TxBar::toggleMox() { moxBtn_->toggle(); }

void TxBar::showMoxActive(bool on) {
    QSignalBlocker block(moxBtn_);
    moxBtn_->setChecked(on);
}

void TxBar::applyPowerCap() {
    const int cap = ampBtn_->isChecked() ? ampLimit_->value() : 100;
    pwr_->setMaximum(cap);                // also drags an over-cap value down,
}                                         // which emits and corrects the radio

void TxBar::setAmpMode(bool on, int limitPct) {
    QSignalBlocker b1(ampBtn_), b2(ampLimit_);
    ampBtn_->setChecked(on);
    ampLimit_->setValue(std::clamp(limitPct, 5, 100));
    ampLimit_->setEnabled(on);
    applyPowerCap();
}

static void showVal(QSlider* s, QLabel* l, int v) {
    QSignalBlocker block(s);
    s->setValue(v);
    l->setText(QString::number(v));
}

void TxBar::showTxPower(int pct)  { showVal(pwr_, pwrVal_, pct); }
void TxBar::showMicGain(int pct)  { showVal(mic_, micVal_, pct); }
void TxBar::showTxFilter(int hz)  { showVal(txbw_, txbwVal_, hz); }

void TxBar::showSpeechProc(int level) {
    showVal(proc_, procVal_, level);
    if (level == 0) procVal_->setText("OFF");
}

void TxBar::showTuner(bool on) {
    QSignalBlocker block(tunerBtn_);
    tunerBtn_->setChecked(on);
}

// Qt only re-reads [property] stylesheet selectors on repolish.
static void setDvrLight(QPushButton* b, const char* stateName) {
    b->setProperty("dvrState", stateName);
    b->style()->unpolish(b);
    b->style()->polish(b);
}

void TxBar::showDvrIdle() {
    setDvrLight(recBtn_, "");
    setDvrLight(playBtn_, "");
    for (auto* b : vkBtn_) setDvrLight(b, "");
}

void TxBar::showDvrRecording(int slot) {
    showDvrIdle();
    setDvrLight(slot < 0 ? recBtn_ : vkBtn_[slot], "rec");
}

void TxBar::showDvrPlaying(int slot, bool onAir) {
    showDvrIdle();
    setDvrLight(slot < 0 ? playBtn_ : vkBtn_[slot], onAir ? "tx" : "play");
}

void TxBar::setVkLoaded(int slot, bool loaded) {
    QPushButton* b = vkBtn_[slot];
    b->setProperty("vkEmpty", !loaded);
    b->style()->unpolish(b);
    b->style()->polish(b);
    b->setToolTip(loaded
        ? QString("Voice keyer message %1\n"
                  "left-click:  TRANSMIT the message (red) — rig switches to\n"
                  "    line-in, keys up, plays it, restores your mic/speech\n"
                  "right-click:  RE-RECORD it from the mic (blue while\n"
                  "    recording, click to stop; replaces the old message\n"
                  "    only if the new take has real audio in it)\n"
                  "Shift + left or right click:  LISTEN on the PC speakers\n"
                  "    (green, nothing transmitted)\n"
                  "click while lit:  stop (and un-key)").arg(slot + 1)
        : QString("Voice keyer slot %1 — EMPTY\n"
                  "right-click:  record a message from the mic (button goes\n"
                  "    blue), click it again to stop and save\n"
                  "left-click:  transmits the message (once one is recorded)\n"
                  "Shift+click:  listens to it on the PC speakers")
              .arg(slot + 1));
}

} // namespace ttc
