// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include "radio/RadioController.h"
#include "radio/SerialPort.h"

namespace ttc {

// Ten-Tec Orion 565/566 (firmware v3) driver. ASCII CAT over 57600 8N1.
// Command encoders below are the real protocol; see docs/cat-command-reference.md.
class TenTecOrion : public RadioController {
    Q_OBJECT
public:
    explicit TenTecOrion(QObject* parent = nullptr);

    bool open(const std::string& device) override;   // e.g. "/dev/ttyS0"
    void close();

    const CapabilityProfile& caps() const override { return caps_; }
    bool connected() const override { return serial_.isOpen(); }

    void setFrequencyHz(Rx rx, uint64_t hz) override;
    void setMode(Rx rx, Mode mode) override;
    void setPassband(Rx rx, int loEdgeHz, int hiEdgeHz) override;
    void setBandwidthHz(Rx rx, int bwHz) override;
    void setPbtHz(Rx rx, int pbtHz) override;

    // Control-surface knobs. AGC is the radio's own letter code:
    // 'F'ast 'M'edium 'S'low 'P'rogram 'O'ff. Attenuator steps 0..3 = off/6/12/18 dB.
    void setAgc(Rx rx, char agc) override;        // *R<M/S>A<F/M/S/P/O>
    // Programmable-AGC group (active in 'P' mode), all live-verified:
    // threshold in µV, decimals ok, radio quantizes (*RMAT150 -> 151.29);
    // hang in seconds, decimals ok, 0 = off (*RMAH2 -> 01.98), 0..~10 s;
    // decay integer (dB/s), minimum 5 — smaller values silently rejected.
    void setAgcThreshold(Rx rx, double uv);       // *R<M/S>AT<val>
    void queryAgcThreshold(Rx rx);                // ?R<M/S>AT -> @RMAT79.06
    void setAgcHang(Rx rx, double sec);           // *R<M/S>AH<val>
    void queryAgcHang(Rx rx);                     // ?R<M/S>AH -> @RMAH01.98
    void setAgcDecay(Rx rx, int rate);            // *R<M/S>AD<val>
    void queryAgcDecay(Rx rx);                    // ?R<M/S>AD -> @RMAD5
    void setRfGain(Rx rx, int gain);              // *R<M/S>G<0-100>
    void setAttenuator(Rx rx, int step);          // *R<M/S>T<0-3>
    void setPreamp(Rx rx, bool on);               // *R<M/S>E<0/1> (main RX only on a 565)
    // DSP helpers share one command group: *R<M/S>N<x><0-9>, 0 = off.
    // NOTE: v3 firmware moved NR from documented letter 'N' (dead, silently
    // ignored) to 'R' — live-probed with set/read-back on Jon's radio.
    void setNoiseReduction(Rx rx, int level);     // *R<M/S>NR<val>
    void setNoiseBlanker(Rx rx, int level);       // *R<M/S>NB<val>  (DSP blanker)
    void setAutoNotch(Rx rx, int level);          // *R<M/S>NA<val>
    void setHardwareNb(Rx rx, bool on);           // *R<M/S>NH<0/1> (undocumented)

    // Manual notch — UNDOCUMENTED commands, live-probe verified on the v3
    // firmware (see docs/cat-command-reference.md). The firmware silently
    // REJECTS out-of-range values, so these clamp before sending.
    void setNotchCenter(Rx rx, int hz);           // *R<M/S>NC<20-4000>
    void setNotchWidth(Rx rx, int hz);            // *R<M/S>NW<10-300>
    void setNotchEngaged(Rx rx, bool on);         // *R<M/S>NM<0/1>
    void setSaf(Rx rx, bool on);                  // *R<M/S>NS<0/1>

    // Transmitter / audio group.
    void setPtt(bool on);                         // *TK key / *TU unkey
    void setTxPower(int pct);                     // *TP<0-100> (percent of 100 W)
    void setMicGain(int pct);                     // *TM<0-100>
    void setSpeechProc(int level);                // *TS<0-9> (0 = off)
    void setTxFilter(int hz);                     // *TF<900-3900>, continuous (live-
                                                  // verified hard clamp; the Orion I
                                                  // has no TX EQ/rolloff over CAT)
    void setAuxInputGain(int pct);                // *TI<0-100> (rear line input)
    void setMonitor(int pct);                     // *TO<0-100> (TX audio monitor)
    void setAfVolume(Rx rx, int pct);             // *UM/*US (percent -> 0-255 byte)
    // Audio routing: what each output carries — left phone, right phone,
    // speaker; letters 'M' main / 'S' sub / 'B' both. One VFO per ear for
    // split work = *UCMS<spk>.
    void setAudioRouting(char left, char right, char speaker);   // *UC
    void setTunerEnabled(bool on);                // *TT<0/1> (internal tuner)
    void startTune();                             // *TTT (tune cycle)

    // CW & keyer group (prg guide p24; whole group set+query live-verified
    // 2026-08-23 — an older note claiming ?C* is silently ignored was
    // wrong). These four are radio-side and apply however the rig is
    // keyed, so they matter with the WinKeyer doing the sending. NOT
    // exposed: *CK keyer on/off, *CS speed, *CW weighting — enabling the
    // internal keyer re-reads the key jack as a paddle and would fight
    // the WinKeyer wired into it.
    void setCwSidetoneVol(int pct);               // *CV<0-100>
    void setCwSidetonePitch(int hz);              // *CT<300-1200>
    void setCwQskDelay(int val);                  // *CQ<0-100>
    void setCwAttackDecay(int ms);                // *CD<3-10> ms
    void queryCw();                               // ?CV/?CT/?CQ/?CD + ?CS/?CW/?CK

    // VFO assignment: which VFO drives the main RX / sub RX / transmitter.
    // Letters 'A'/'B' ('N' = unassigned for sub/tx). RX on A with TX on B is
    // how the Orion does split. *KV[mainrx][subrx][tx], query ?KV -> @KV...
    void setVfoAssignment(char mainRx, char subRx, char tx);
    void queryVfoAssignment();
    // Antenna routing: per port (ANT1, ANT2, RX ANT), which receivers listen
    // there — 'M'ain, 'S'ub, 'B'oth, 'N'one. TX follows the main receiver's
    // port, and main must sit on ANT1 or ANT2 (RX ANT is receive-only).
    // *KA[ant1][ant2][rxant], query ?KA -> @KA...
    void setAntennaRouting(char ant1, char ant2, char rxAnt);
    void queryAntennaRouting();
    // VFO lock: *AL/*AU and *BL/*BU. Freezes the front-panel knob only —
    // CAT tuning still works (live-verified), so the console must also
    // refuse its own tune gestures while a lock is on.
    void setVfoLock(char vfo, bool on);           // vfo = 'A' or 'B'
    void queryVfoLock(char vfo);                  // ?AL/?BL -> @AL/@AU/@BL/@BU

    // Queries (responses arrive asynchronously via the signals below).
    void queryFrequency(Rx rx);
    void queryFilter(Rx rx);
    void queryMode(Rx rx);
    void querySMeter();                           // ?S -> @SRM<m>S<s> (or @STF.. in TX)
    void queryAgc(Rx rx);                         // ?R<M/S>A
    void queryRfGain(Rx rx);                      // ?R<M/S>G
    void queryAttenuator(Rx rx);                  // ?R<M/S>T
    void queryPreamp(Rx rx);                      // ?R<M/S>E
    void queryNotch(Rx rx);                       // ?R.NC / ?R.NW / ?R.NM (undocumented)
    void queryTxPower();                          // ?TP
    void queryTxAudio();                          // ?TM / ?TS / ?TF / ?TO / ?UM
    void queryTxFilter();                         // ?TF
    void querySpeechProc();                       // ?TS
    void queryMicGain();                          // ?TM
    void queryAuxInputGain();                     // ?TI (undocumented query)
    void queryTuner();                            // ?TT
    void queryAfVolume(Rx rx);                    // ?UM / ?US
    void queryAudioRouting();                     // ?UC
    // Speculative: NR/NB/AN level queries appear in no document, but neither
    // did the notch ones and the radio answered those. Unanswered queries are
    // harmless; if replies come back the *Reported signals below fire.
    void queryDspLevels(Rx rx);                   // ?R.NN / ?R.NB / ?R.NA

    // Report signals are inherited from RadioController.

private slots:
    void onLine(const QByteArray& line);

private:
    void send(const QByteArray& cmd);            // appends CR, writes
    static char rxLetter(Rx rx) { return rx == Rx::Main ? 'M' : 'S'; }

    SerialPort serial_;
    CapabilityProfile caps_;
};

} // namespace ttc
