// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
#include <QDateTime>
#include <cmath>
#include <vector>
class QTimer;
class QSlider;
class QLabel;
class QToolButton;
class QLineEdit;
class QUdpSocket;
class QAction;
class QHBoxLayout;
#include "radio/TenTecOrion.h"
#include "net/RigctldServer.h"
#include "net/SpotClient.h"
#include "net/PotaClient.h"
#include "net/SolarClient.h"
#include "net/RotorClient.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"
#include "util/WatchList.h"
#include <QHash>
#include "cw/CwWindow.h"
namespace ttc { class CwDecoder; class SkimmerEngine; class FldigiClient;
                class AudioCwSource;
                class DigiWindow; class SkimServer; class SkimmerWindow; }
#include "ui/PanadapterWidget.h"
#include "ui/SMeterWidget.h"
#include "ui/ControlPanel.h"
#include "ui/FrequencyDisplay.h"
#include "ui/RoutingPanel.h"
#include "ui/AudioPanel.h"
#include "ui/TxBar.h"
#include "audio/ClipDeck.h"
#include "radio/TenTecOmni7.h"
#ifdef HAVE_SDRPLAY
#include "sdr/SdrPlaySource.h"
#include "dsp/SpectrumComputer.h"
#endif
#include "sdr/IqRecorder.h"
#include "dsp/PitchTrim.h"
#include "sdr/AutoGain.h"
#include <atomic>
class QThread;

namespace ttc {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onTuneRequested(int offsetHz, bool exact);
    void onPassbandChanged(int loHz, int hiHz);

private:
    // Second-deck tool builders (MainWindowTools.cpp) — one per feature,
    // called once from the constructor. New tools: add a function there
    // and a call in the constructor; buttons go on topLay2_.
    void setupLogUi();
    void setupCwUi();
    void setupSkimUi(const QString& stationCall);
    void setupDigiUi();
    void setupRotorUi();
    int  snapToCwPeak(int offsetHz, int windowHz) const; // CW zap peak finder
    void zeroBeat();                   // Z: zap strongest signal in the passband
    void zeroBeatPass();               // refinement passes (fresh FFT each time)
    void armPitchTrim();               // final stage: land the NOTE on cw/pitchHz
    void pitchTrimFeed(double hz);     // every pitch sample drives the servo
    void flipCwSideband();             // X: CW<->CWR aural zero-beat check
    void refreshPassbandOverlay();     // radio state -> mode-sided on-screen passband
    void refreshNotchOverlay();        // radio notch (audio Hz) -> RF-offset marker
    void syncNotchUi();                // notch/SAF state -> marker + sidebar buttons
    void sendPendingFilter();          // coalesced drag-to-filter serial writes
    void sendPendingNotch();           // coalesced drag-to-notch serial writes
    void tuneAbsolute(uint64_t hz);    // every tune path funnels through here
    void applyMode(Mode m);            // user mode change (button or band memory)
    void startManualTune();            // steady carrier for amp/external tuner
    void stopManualTune();
    void setDigitalMode(bool on);      // line-in for digital vs mic for voice
    void applyTxProfile(int slot);     // recall a stored TX-audio bundle
    void saveTxProfile(int slot);      // store current TX BW/PROC/MIC/PWR
    void pushVfoB();                   // VFO B dial+filter+TX state -> panadapter
    void saveBandMemory();             // stash freq/mode/filter in curBand_/curReg_
    void syncBandRegister();           // mirror any dial move into the band stack
    void recallStack(int band, int reg); // recall a band-stack register
    void recall60m(int chan);          // locked US 60 m channel (CH1..CH5)
    void dvrStopped();                 // clip deck went idle: lights off, PTT down
    void dvrPlayOverAir(const QString& wav, int slot); // line-in + PTT + play
    QString dvrDir() const;            // ~/.local/share/n8mus/tentec-console/dvr
    QString vkPath(int slot) const;    // voice keyer message file for a slot
    void saveMarkers();                // persist + repaint pinned freq markers
    void scheduleIqRecordingDialog();  // arm/cancel a timed IQ recording
    RadioController* radio_;                  // owned (QObject child); see makeRadio
    RigctldServer    rigctld_;
    SpotClient       spotClient_;                  // DX-cluster telnet feed
    PotaClient       potaClient_;                  // POTA activator API feed
    SolarClient      solarClient_;                 // NOAA space-weather poller
    QStringList      parkFilter_;                  // POTA park countries in area
    CtyLookup        cty_;                         // callsign -> country coords
    LogbookIndex     logbook_;                     // cqrlog worked-before data
    QHash<QString, QPair<double, double>> ctyMemo_; // per-call resolution cache
    PanadapterWidget* pan_ = nullptr;
    QWidget*     topStrip_ = nullptr;          // top bar (tool buttons' parent)
    QHBoxLayout* topLay2_  = nullptr;          // second menu deck (new buttons!)
    SMeterWidget*     smeter_ = nullptr;
    ControlPanel*     panel_  = nullptr;
    FrequencyDisplay* freqDisp_  = nullptr;   // VFO A (main RX, the panadapter dial)
    FrequencyDisplay* freqDispB_ = nullptr;   // VFO B (sub RX), display + direct set
    RoutingPanel*     routing_   = nullptr;   // VFO/antenna matrix + A/B transfers
    AudioPanel*       audioPanel_ = nullptr;  // output-routing popup
    // Per-VFO volume + mute, living under the frequency readouts.
    QSlider*     volSl_[2]   = {};            // [0]=A/main, [1]=B/sub
    QLabel*      volLbl_[2]  = {};
    QToolButton* muteBtn_[2] = {};
    QTimer*      volTx_[2]   = {};            // coalesced *UM/*US streams
    int  pendVol_[2] = {0, 0};
    bool muted_[2]   = {false, false};
    int  vol_[2]     = {50, 50};              // last known volumes (A, B)
    int  preMute_[2] = {50, 50};              // levels to restore on unmute
    uint64_t vfoBHz_ = 7000000;               // last known VFO B dial
    Mode     subMode_ = Mode::LSB;            // sub RX mode (rides with VFO B)
    int      subBwHz_ = 2500, subPbtHz_ = 0;  // sub RX filter (B's overlay width)
    char     txVfo_  = 'A';                   // from ?KV; 'B' = split active
    char     rxVfo_  = 'A';                   // main RX assignment (for B-engage)
    QTimer*  bfTx_ = nullptr;                 // coalesced *BF stream for B drags
    uint64_t pendBHz_ = 0;
    bool     bfDirty_ = false;
    QToolButton* bViewBtn_ = nullptr;         // show/hide B on the panadapter
    QToolButton* lockBtn_[2] = {};            // per-VFO tune locks (*AL/*BL)
    bool vfoLockA_ = false, vfoLockB_ = false;
    QTimer* sfTx_ = nullptr;                  // coalesced sub-filter drag stream
    int  pendSubBw_ = 0, pendSubPbt_ = 0;
    bool subFilterDirty_ = false;
    QElapsedTimer sinceSubFilterEdit_;        // suppress poll snap-back after a drag
    int anchorSubLoHz_ = 0, anchorSubHiHz_ = 0;   // B overlay edges at drag start
    int anchorSubBwHz_ = 2500, anchorSubPbtHz_ = 0; // sub radio state at drag start
    QTimer*  afTx_ = nullptr;                 // coalesced tune stream for A drags
    uint64_t pendAHz_ = 0;
    bool     afDirty_ = false;
    QElapsedTimer sinceVfoBEdit_;             // suppress poll snap-back after an edit
    TxBar*            txBar_ = nullptr;

    // DVR: off-air record/playback + voice keyer, audio over the SignaLink.
    ClipDeck* dvr_ = nullptr;
    bool dvrTxPlayback_ = false;       // this playback keyed the radio (drop PTT)
    bool dvrAutoDig_ = false;          // we engaged line-in for it (restore voice)
    QString dvrLastRx_;                // newest off-air take this session
    QString dvrJustRecorded_;          // take to peak-normalize when it lands
    QString radioSink_, radioSource_;  // SignaLink playback/capture node names
    QString micSource_;                // VK record source ("" = default mic)
    int curBand_ = -1;                         // index into kBands, -1 = none
    int curReg_  = 0;                          // active stack register (0..3 = A..D)
    int lastBandPress_ = -1;                   // band button last pressed (for cycling)
    QTimer* bandStamp_ = nullptr;              // debounced stack-register stamping
    QElapsedTimer sinceEnforce_;               // rate-limit amp drive-limit corrections

    // Manual tune carrier (TUNE with the internal tuner disabled).
    bool tuning_      = false;
    bool tunerOn_     = false;                 // internal tuner enable, from polling
    int  lastTxPwr_   = 50;                    // radio's PWR, for restore after tune
    Mode preTuneMode_ = Mode::USB;
    int  preTunePwr_  = 50;
    QTimer* tuneTimeout_ = nullptr;            // safety: carrier auto-drops

    // SAM pseudo-mode (ECSS): USB zero-beaten on an AM carrier, fine
    // wheel steps while active; previous mode/filter restored on exit.
    bool samActive_   = false;
    Mode samEngine_   = Mode::USB;            // click the lit button to flip U/L
    Mode preSamMode_  = Mode::AM;
    int  preSamBw_    = 6000;

    // Digital/voice audio switching. Voice settings are learned
    // from the radio when entering digital, and persisted (defaults 51/2).
    bool digital_       = false;
    int  lastMicGain_   = 51;                  // learned VOICE values (never 0 —
    int  lastSpeechProc_ = 2;                  // see the poisoning guards)
    int  micNow_        = 0;                   // radio's mic as last reported
    int  txBwHz_        = 2850;                // TX filter, from polling (?TF)
    uint64_t centerHz_ = 7150000;              // open on 40 m where the Orion lives
    bool awaitingFreq_ = false;                // one ?AF in flight at a time
    QElapsedTimer freqQueryAge_;
    uint64_t pendingHz_ = 0;                   // change must be seen twice (glitch filter)
    int pollTick_ = 0;                         // schedules mode/filter queries between ?AF
    QElapsedTimer sinceTune_;                  // suppress dial-follow right after a click
    QElapsedTimer sinceFilterEdit_;            // suppress overlay refresh right after a drag

    // Radio filter state (from polling) and mode-aware passband mapping.
    Mode rigMode_  = Mode::USB;
    int  rigBwHz_  = 2400;
    int  rigPbtHz_ = 0;

    // Delta-anchored drag: on edge grab, snapshot the radio's real polled state;
    // drags send anchor + delta, so no assumed nominal placement can cause jumps.
    int anchorLoHz_ = 0, anchorHiHz_ = 0;      // overlay edges at drag start
    int anchorBwHz_ = 2400, anchorPbtHz_ = 0;  // radio state at drag start

    // Manual notch state (audio Hz, from polling; see refreshNotchOverlay).
    // safOn_ = the shared DSP engine is in SAF (peak) flavor, not reject.
    bool notchOn_      = false;
    bool safOn_        = false;
    int  notchCenter_  = 550;
    int  notchWidth_   = 30;
    QElapsedTimer sinceNotchEdit_;             // suppress poll snap-back after a drag

    // Coalesced drag-to-filter: keep only the latest values, send ~25x/sec.
    QTimer* filterTx_ = nullptr;
    int  pendBwHz_ = 0, pendPbtHz_ = 0;
    bool filterDirty_ = false;
    QTimer* notchTx_ = nullptr;                // same pattern for notch drags
    int  pendNotchHz_ = 0;
    bool notchDirty_ = false;
    // SDR-source S-meter calibration: dbS9 = passband power + gain
    // compensation + sdrCalDb_. The offset is set once by "Calibrate SDR to
    // radio" (matching the radio's reading while its RF gain is full up) and
    // survives every SDRplay gain change — the gRdB/LNA compensation is exact.
    double sdrCalDb_      = 0.0;
    double lastRadioDbS9_ = NAN;               // radio's latest RX reading
    double lastSdrMeasDb_ = NAN;               // SDR measurement, pre-offset
    // CW zap: click in CW snaps the dial onto the true carrier peak found in
    // the averaged spectrum (both Ten-Tecs read the carrier on the dial in
    // CW, so this alone puts the note on the sidetone pitch).
    std::vector<float> lastSpectrum_;          // latest averaged FFT frame
    int  sdrSpanHz_ = 0;                       // capture span of that frame
    bool cwZap_     = true;                    // DISPLAY "CW zap" checkbox
    int      zbPassesLeft_ = 0;                // 0-beat refinement passes pending
    uint64_t zbExpectHz_   = 0;                // abort if anything else retunes
    // LOG panel: one-click QSO -> cqrlog via the fork's always-on UDP ADIF
    // bridge (127.0.0.1:2334). Call/park prefill from the last clicked spot.
    QLineEdit* logCall_ = nullptr;
    QLineEdit* logRstS_ = nullptr;
    QLineEdit* logRstR_ = nullptr;
    QLineEdit* logPark_ = nullptr;
    QUdpSocket* logUdp_ = nullptr;
    QDateTime qsoStartUtc_;                    // when the call landed in the field
    AutoGain autoLna_;                         // RF AGC policy (skimmer)
    std::atomic<float> iqPeak_{0.0f};          // wideband |peak| since last read
    // TX monitor (Option A): gain slam while transmitting; see the
    // txTick timer in MainWindow.cpp. Ant-B coupler switch lands here
    // when the RF sampler hardware arrives (Option C).
    QAction* txMonAct_ = nullptr;
    bool     txMonOn_ = false;
    bool     radioTx_ = false;
    int      txSaveGr_ = 45, txSaveLna_ = 3;
    qint64   lastTxMs_ = 0;
    qint64   txPredictMs_ = 0;             // console is about to key (CW win)
    // 0-beat final stage: damped stepped servo on the SignaLink pitch
    // measurement (the radio's own audio frame). Policy in dsp/PitchTrim.h;
    // fed from the pitchMeasured stream in setupCwUi. The latest reading
    // is kept so zeroBeat() can go pitch-first: an audible note near the
    // target skips the FFT peak hunt entirely.
    PitchTrim pitchTrim_;
    double   lastPitchHz_ = -1.0;
    qint64   lastPitchMs_ = 0;
    bool     panicked_ = false;            // event-callback slam may be live
    std::atomic<bool> txMonEnabled_{true}; // checkbox mirror (SDR thread reads)
    int      txMonHangMs_ = 1000;          // QSK hang before RX gain returns
    unsigned lastOverloads_ = 0;
    IqRecorder* iqRec_ = nullptr;              // band recorder (.tciq)
    QAction* recIqAct_ = nullptr;              // SDR-menu toggle
    QAction* schedIqAct_ = nullptr;            // SDR-menu schedule/cancel entry
    QTimer*  schedStartTmr_ = nullptr;         // armed: fire = tune + record
    uint64_t schedHz_ = 0;                     // scheduled dial frequency
    int      schedSecs_ = 0;                   // scheduled recording length
    QVector<FreqMarker> markers_;              // pinned panadapter frequencies
    QThread* replayThread_ = nullptr;          // TTC_IQFILE feeder
    std::atomic<bool> replayStop_{false};
    CwWindow* cwWin_ = nullptr;                // WinKeyer CW sender (lazy)
    CwDecoder* cwDec_ = nullptr;               // SDR-fed CW reader
    CwDecoder* audioDec_ = nullptr;            // radio-audio CW reader (lazy)
    AudioCwSource* audioSrc_ = nullptr;        // SignaLink capture for it
    bool rxWanted_ = false;                    // CW window wants decode
    bool rxRadio_ = false;                     // source: radio audio vs SDR
    SkimmerEngine* skim_ = nullptr;            // CW skimmer decoder bank
    QAction* skimEnable_ = nullptr;            // SKIM menu on/off toggle
    QLabel*  skimStatus_ = nullptr;            // live channel readout (menu)
    SkimServer* skimSrv_ = nullptr;            // local RBN telnet (:7300)
    SkimmerWindow* skimWin_ = nullptr;         // band map (lazy)
    FldigiClient* fldigi_ = nullptr;           // XML-RPC link (lazy)
    DigiWindow*   digiWin_ = nullptr;          // fldigi companion (lazy)
    RotorClient rotor_;                        // rotctld link (:4533)
    double  lastRoseBearing_ = -1.0;           // rose target for TURN
    QString lastRoseLabel_;
    WatchList watch_;                          // DX-watch alert patterns
    QHash<QString, qint64> watchNotified_;     // call -> epoch (renotify gap)
#ifdef HAVE_SDRPLAY
    SdrPlaySource    sdr_;
    SpectrumComputer spectrum_{8192};   // 61 Hz/bin at 500 kHz capture — survives deep zoom
#endif
};

} // namespace ttc
