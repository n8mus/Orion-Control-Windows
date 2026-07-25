// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <cstdint>
#include <string>

namespace ttc {

enum class Rx { Main, Sub };

enum class Mode { USB, LSB, CWU, CWL, AM, FM };

// What a given radio can actually do. Drives UI gating and how the
// panadapter passband-drag gesture is quantized.
struct CapabilityProfile {
    std::string name;
    int   bandwidthMinHz   = 100;
    int   bandwidthMaxHz   = 6000;
    int   bandwidthStepHz  = 1;      // Orion: 1 (continuous). Omni VII: preset ladder.
    int   pbtRangeHz       = 8000;   // ± range
    bool  continuousFilter = true;   // false => snap width to a preset table
    bool  dualReceiver     = true;
    bool  needsHwHandshake = false;  // Omni VII serial requires RTS/CTS
    bool  catTxControls    = true;   // power/mic/proc/txbw/tune over CAT.
                                     // Omni VII: only in REMOTE (Ethernet).
};

// Radio-agnostic control surface + report signals. Drivers override what
// their radio supports; the default implementations are no-ops so a driver
// with a smaller CAT surface (Omni VII in RADIO mode) simply ignores the
// rest. MainWindow and the rigctld server talk only to this class.
class RadioController : public QObject {
    Q_OBJECT
public:
    explicit RadioController(QObject* parent = nullptr) : QObject(parent) {}

    virtual const CapabilityProfile& caps() const = 0;
    virtual bool connected() const = 0;
    virtual bool open(const std::string& device) = 0;

    // Tuning / mode / receive filter.
    virtual void setFrequencyHz(Rx, uint64_t) {}
    virtual void setMode(Rx, Mode) {}
    virtual void setPassband(Rx, int /*loEdgeHz*/, int /*hiEdgeHz*/) {}
    virtual void setBandwidthHz(Rx, int) {}
    virtual void setPbtHz(Rx, int) {}
    virtual void queryFrequency(Rx) {}
    virtual void queryFilter(Rx) {}
    virtual void queryMode(Rx) {}

    // Receiver front end / DSP.
    virtual void setAgc(Rx, char /*F M S P O*/) {}
    virtual void setAgcThreshold(Rx, double /*uV*/) {}
    virtual void setAgcHang(Rx, double /*sec*/) {}
    virtual void setAgcDecay(Rx, int) {}
    virtual void setRfGain(Rx, int /*0-100*/) {}
    virtual void setAttenuator(Rx, int /*0-3*/) {}
    virtual void setPreamp(Rx, bool) {}
    virtual void setNoiseReduction(Rx, int) {}
    virtual void setNoiseBlanker(Rx, int) {}
    virtual void setAutoNotch(Rx, int) {}
    virtual void setHardwareNb(Rx, bool) {}
    virtual void setNotchCenter(Rx, int) {}
    virtual void setNotchWidth(Rx, int) {}
    virtual void setNotchEngaged(Rx, bool) {}
    virtual void setSaf(Rx, bool) {}
    virtual void queryAgc(Rx) {}
    virtual void queryAgcThreshold(Rx) {}
    virtual void queryAgcHang(Rx) {}
    virtual void queryAgcDecay(Rx) {}
    virtual void queryRfGain(Rx) {}
    virtual void queryAttenuator(Rx) {}
    virtual void queryPreamp(Rx) {}
    virtual void queryNotch(Rx) {}
    virtual void queryDspLevels(Rx) {}

    // Transmitter / TX audio.
    virtual void setPtt(bool) {}
    virtual void setTxPower(int /*pct*/) {}
    virtual void setMicGain(int) {}
    virtual void setSpeechProc(int /*0-9*/) {}
    virtual void setTxFilter(int /*hz*/) {}
    virtual void setAuxInputGain(int) {}
    virtual void setMonitor(int) {}
    virtual void setTunerEnabled(bool) {}
    virtual void startTune() {}
    virtual void queryTxPower() {}
    virtual void queryTxAudio() {}
    virtual void queryTxFilter() {}
    virtual void querySpeechProc() {}
    virtual void queryMicGain() {}
    virtual void queryAuxInputGain() {}
    virtual void queryTuner() {}
    // Omni VII extras (REMOTE mode only): TX EQ in dB, LF rolloff in Hz.
    virtual void setTxEq(int /*-20..20 dB*/) {}
    virtual void setTxRolloff(int /*70-300 Hz*/) {}

    // Audio outputs.
    virtual void setAfVolume(Rx, int /*pct*/) {}
    virtual void setAudioRouting(char, char, char) {}
    virtual void queryAfVolume(Rx) {}
    virtual void queryAudioRouting() {}

    // VFO/antenna plumbing and meters.
    virtual void setVfoAssignment(char /*mainRx*/, char /*subRx*/, char /*tx*/) {}
    virtual void setAntennaRouting(char, char, char) {}
    virtual void setVfoLock(char /*A|B*/, bool) {}
    virtual void queryVfoAssignment() {}
    virtual void queryAntennaRouting() {}
    virtual void queryVfoLock(char) {}
    virtual void querySMeter() {}

signals:
    void frequencyReported(Rx rx, uint64_t hz);
    void bandwidthReported(Rx rx, int bwHz);
    void pbtReported(Rx rx, int pbtHz);
    void modeReported(Rx rx, Mode mode);
    void sMeterReported(int mainRaw, int subRaw); // Orion raw-unit scale
    void txMeterReported(double fwdWatts, double refWatts, double swr);
    void agcReported(Rx rx, char agc);
    void agcThresholdReported(Rx rx, double uv);
    void agcHangReported(Rx rx, double sec);
    void agcDecayReported(Rx rx, int rate);
    void rfGainReported(Rx rx, int gain);
    void attenReported(Rx rx, int step);
    void preampReported(Rx rx, bool on);
    void notchCenterReported(Rx rx, int hz);
    void notchWidthReported(Rx rx, int hz);
    void notchEngagedReported(Rx rx, bool on);
    void safReported(Rx rx, bool on);
    void nrReported(Rx rx, int level);
    void nbReported(Rx rx, int level);
    void autoNotchReported(Rx rx, int level);
    void hardwareNbReported(Rx rx, bool on);
    void txPowerReported(int pct);
    void micGainReported(int pct);
    void speechProcReported(int level);
    void txFilterReported(int hz);
    void auxInputGainReported(int pct);
    void monitorReported(int pct);
    void afVolumeReported(Rx rx, int pct);
    void audioRoutingReported(char left, char right, char speaker);
    void tunerReported(bool on);
    void vfoAssignmentReported(char mainRx, char subRx, char tx);
    void antennaRoutingReported(char ant1, char ant2, char rxAnt);
    void vfoLockReported(char vfo, bool locked);
    void txEqReported(int db);
    void txRolloffReported(int hz);
};

} // namespace ttc
