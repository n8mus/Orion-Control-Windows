// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

namespace ttc {

// Bottom transmit strip: power (with amplifier drive limit), tuner, mic gain,
// TX filter bandwidth and speech processor, plus four one-click
// TX profile buttons (KE9NS TXProfile idea: bundle the whole transmit-audio
// setup — left-click recalls, right-click saves the current settings into
// that slot). Slider drags are coalesced (~25/s trailing edge) like every
// other streamed control, so they can't flood the Orion's UART. (AF volume
// lives under each VFO readout in the top strip.)
class TxBar : public QWidget {
    Q_OBJECT
public:
    explicit TxBar(QWidget* parent = nullptr);

    bool ampMode() const;
    int  ampLimit() const;
    int  tuneLevel() const;                       // carrier watts for manual tune
    // Gray out every control that rides CAT commands the connected radio
    // can't take (Omni 8 in RADIO mode: the whole C1/C2 config group).
    void setCatTxControlsEnabled(bool on);

signals:
    void txPowerChanged(int pct);
    void ampModeChanged(bool on, int limitPct);   // toggle or limit spin
    void tuneToggled(bool on);                    // manual tune carrier on/off
    void moxToggled(bool on);                      // manual TX (voice) on/off
    void tuneLevelChanged(int watts);
    void tunerEnableToggled(bool on);
    void micGainChanged(int pct);
    void txFilterChanged(int hz);                 // TX BW slider (900-3900)
    void speechProcChanged(int level);            // PROC slider (0 = off)
    void profileRecalled(int slot);               // left-click a profile button
    void profileSaveRequested(int slot);          // right-click: store current
    // DVR intents — TxBar only renders state (showDvr* below); MainWindow
    // owns the clip deck, the files and PTT. Any click while the deck is
    // busy means "stop", which MainWindow resolves.
    void dvrRecordClicked();                      // off-air REC toggle
    void dvrPlayClicked(bool overAir);            // false = speakers, true = retransmit
    void vkClicked(int slot);                     // voice keyer: play (or stop)
    void vkRecordClicked(int slot);               // right-click: record the slot

public slots:
    void setAmpMode(bool on, int limitPct);       // restore from settings
    void setTuneLevel(int watts);                 // restore from settings
    void showTuneActive(bool on);                 // reflect/force the TUNE latch
    void showMoxActive(bool on);                  // reflect/force the MOX latch
    void showTxPower(int pct);
    void showMicGain(int pct);
    void showTxFilter(int hz);
    void showSpeechProc(int level);
    void showTuner(bool on);
    void showDvrIdle();                           // all DVR lights off
    void showDvrRecording(int slot);              // -1 = off-air deck, 0..3 = VK
    void showDvrPlaying(int slot);
    void setVkLoaded(int slot, bool loaded);      // dim + tooltip empty slots

private:
    QSlider* makeSlider(const QString& name, QLabel*& value,
                        QTimer*& tx, int& pend, void (TxBar::*sig)(int));
    void applyPowerCap();                          // slider max follows amp limit
    void markProfile(int slot);                    // check one button (-1 = none)

    QSlider* pwr_ = nullptr;   QLabel* pwrVal_ = nullptr;
    QSlider* mic_ = nullptr;   QLabel* micVal_ = nullptr;
    QSlider* txbw_ = nullptr;  QLabel* txbwVal_ = nullptr;
    QSlider* proc_ = nullptr;  QLabel* procVal_ = nullptr;
    QPushButton* profBtn_[4] = {};
    QPushButton* recBtn_ = nullptr;    // DVR off-air record
    QPushButton* playBtn_ = nullptr;   // DVR off-air playback
    QPushButton* vkBtn_[4] = {};       // voice keyer slots
    QPushButton* ampBtn_   = nullptr;
    QSpinBox*    ampLimit_ = nullptr;
    QPushButton* tuneBtn_  = nullptr;
    QPushButton* moxBtn_   = nullptr;
    QSpinBox*    tuneLvl_  = nullptr;
    QPushButton* tunerBtn_ = nullptr;
    QTimer* pwrTx_ = nullptr;  int pendPwr_ = -1;
    QTimer* micTx_ = nullptr;  int pendMic_ = -1;
    QTimer* txbwTx_ = nullptr; int pendTxbw_ = -1;
    QTimer* procTx_ = nullptr; int pendProc_ = -1;
};

} // namespace ttc
