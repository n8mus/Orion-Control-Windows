// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <vector>

#include "dsp/ClientComp.h"
#include "dsp/ClientDeEss.h"
#include "dsp/ClientEq.h"
#include "dsp/ClientFinalLimiter.h"
#include "dsp/ClientGate.h"

namespace ttc {

// Voice-keyer EQ (phase 1 of the PC voice chain): the operator's recorded
// VK messages are shaped offline, at record time and whenever the curve
// changes, with the ClientEq parametric EQ ported from AetherSDR. Each
// slot keeps its untouched take (vkN.raw.wav) and the console renders the
// message that actually plays and transmits (vkN.wav) from it, so the
// curve can be re-tuned and re-auditioned without re-recording.
//
// The layout is fixed and voice-shaped rather than free-form — eight
// slots in the order a broadcast EQ runs them:
//   0 high-pass   1 low shelf   2..5 peaks   6 high shelf   7 low-pass
// The Orion still applies its own TX bandwidth (900-3900 Hz) and LF
// rolloff after this, so the EQ shapes tone; the radio sets the edges.
struct VoiceEqSettings {
    static constexpr int kBands = 8;
    bool enabled = false;                          // the EQ stage
    std::array<ClientEq::BandParams, kBands> bands{};

    // Dynamics, AetherSDR's TX chain order around the EQ:
    //   gate -> EQ -> de-esser -> compressor (+its limiter) -> final limiter
    // Each stage has its own on/off; values are the ported classes' own
    // parameters. A profile carries the whole chain.
    struct Gate {
        bool  on = false;
        float thresholdDb = -45.f;   // opens above this
        float ratio = 2.f;           // 2 = gentle expander .. 10 = hard gate
        float floorDb = -15.f;       // most it ever turns down
        float holdMs = 60.f;
        float releaseMs = 150.f;
        float returnDb = 3.f;        // hysteresis
        float lookaheadMs = 2.f;     // protects word onsets
    } gate;
    struct DeEss {
        bool  on = false;
        float freqHz = 4000.f;       // SSB's sibilance lives just under 3.9k
        float q = 1.5f;
        float thresholdDb = -30.f;
        float amountDb = -6.f;       // max cut in the band
        int   slopeStages = 2;       // x 12 dB/oct
    } deess;
    struct Comp {
        bool  on = false;
        float thresholdDb = -20.f;
        float ratio = 3.f;
        float attackMs = 10.f;
        float releaseMs = 150.f;
        float kneeDb = 6.f;
        float driveDb = 0.f;         // pushes more into the compressor
        int   phaseStages = 4;       // all-pass peak rotator, 0 = off
        bool  limiterOn = true;
        float ceilingDb = -1.f;
        float makeupDb = 6.f;        // LIVE mic only: VK renders measure it
    } comp;
    struct Limit {
        bool  on = true;             // the safety net — on by default
        float ceilingDb = -1.f;
        bool  dcBlock = true;
    } limit;

    // Slot types never change; a stored band of the wrong type is ignored.
    static ClientEq::FilterType slotType(int i);
    static QString slotName(int i);                // "High-pass", "Peak 1" …

    // Starting points to tune by ear on the MON, not measured answers.
    // DxPunch is fitted to the Heil DX target inside a 2.4 kHz TX filter;
    // EssbSmooth keeps flat lows and a broad high lift for 50-3900 Hz;
    // Shure444 is fitted to the Shure 444's response (the RE20/RE320 are
    // near-flat, so this lays the 444's colour over them).
    // HeilPr40 is fitted to the PR40's published on-axis response — on a
    // near-flat RE20/RE320 it lays the PR40's colour over the voice.
    enum class Preset { Flat = 0, SsbVoice = 1, WideSsb = 2, DxPunch = 3,
                        EssbSmooth = 4, Shure444 = 5, HeilPr40 = 6 };
    static QStringList presetNames();
    static VoiceEqSettings preset(Preset p);

    // The operator's own named profiles (vk/eqProfiles, one JSON object
    // keyed by name — any characters allowed in a name). Built-in presets
    // are separate and can't be overwritten or deleted.
    // `store` is the QSettings key holding the set: the voice keyer's
    // (default) or the SDR receive EQ's (kRxProfileStore) — separate sets,
    // so a receive curve can never land in the transmit list or vice versa.
    static constexpr const char* kTxProfileStore = "vk/eqProfiles";
    static constexpr const char* kRxProfileStore = "sdrrx/eqProfiles";
    static QStringList userProfileNames(const char* store = kTxProfileStore);  // sorted, case-blind
    static bool loadUserProfile(const QString& name, VoiceEqSettings& out,
                                const char* store = kTxProfileStore);
    static void saveUserProfile(const QString& name, const VoiceEqSettings& s,
                                const char* store = kTxProfileStore);
    static void deleteUserProfile(const QString& name, const char* store = kTxProfileStore);

    QJsonObject toJson() const;
    static VoiceEqSettings fromJson(const QJsonObject& o);
    static VoiceEqSettings load();                 // QSettings vk/eq (default SSB)
    void save() const;

    float responseDb(float hz) const;              // whole curve, for the plot
};

// Reference mic/speech target curves drawn behind the EQ plot as a shape
// to steer toward (display only — never applied). Data from AetherSDR's
// src/gui/ClientEqCurveWidget.cpp (GPL-3, KK7GWY and contributors), which
// digitised them from their sources: AT&T 1959 speech-transmission
// optimum, Heil's DX/contest target, and the Astatic D-104, Shure 444 and
// Heil HC-5 responses. Index 0 = none.
struct VoiceEqTarget {
    struct Pt { float hz, db; };
    QString name;
    std::vector<Pt> pts;
    float dbAt(float hz) const;                    // log-frequency interpolation
};
const std::vector<VoiceEqTarget>& voiceEqTargets();

// The chain itself — gate -> EQ -> de-esser -> compressor -> final limiter —
// shared by the offline VK renders and the live mic so both sound
// identical for one profile. configure() may be called again at any time
// (live edits from the EQ window); the ported stages pick new parameters
// up at the next block. Not thread-safe: one owner thread.
class VoiceChain {
public:
    void prepare(int rate, const VoiceEqSettings& s, float makeupDb);
    void configure(const VoiceEqSettings& s, float makeupDb);
    void process(float* mono, int n);              // in place, any n
    void reset();                                  // flush envelopes

    // Last block's meters (dB, <= 0), for stats and live readouts.
    float gateGrDb() const { return s_.gate.on ? gate_.gainReductionDb() : 0.f; }
    float deessGrDb() const { return s_.deess.on ? deess_.gainReductionDb() : 0.f; }
    float compGrDb() const { return s_.comp.on ? comp_.gainReductionDb() : 0.f; }
    bool  limitActive() const { return s_.limit.on && limit_.active(); }
    bool  gateOn() const { return s_.gate.on; }
    bool  gateOpen() const { return !s_.gate.on || gate_.gateOpen(); }

private:
    VoiceEqSettings s_;
    int   rate_ = 48000;
    bool  eqDirty_ = true;
    ClientGate gate_;
    ClientEq eq_;
    ClientDeEss deess_;
    ClientComp comp_;
    ClientFinalLimiter limit_;
};

// What a render did, for the operator: the compressor's work shows up as
// talk power — average (RMS) level gained at the same peak, i.e. the drop
// in crest factor — because every message is re-peaked to the same level.
struct VoiceRenderStats {
    double talkPowerDb = 0;      // RMS gain vs the unprocessed take, same peak
    double gateMaxDb = 0;        // deepest gate reduction (<= 0)
    double deessMaxDb = 0;       // deepest de-esser reduction (<= 0)
    double compAvgDb = 0;        // compressor GR, averaged over speech
    double compMaxDb = 0;        // compressor GR, deepest
    double limitPct = 0;         // share of speech blocks the final limiter touched
};

// Render inWav (any 16-bit PCM WAV) through the chain to outWav, then peak-
// normalize the result to targetPeak so processing never changes TX drive
// (cuts would starve the ALC, boosts would clip). With every stage off the
// output is the input, normalized. The output is written to a temp file
// and renamed into place, so a message is never left half-written.
bool renderVoiceEq(const QString& inWav, const QString& outWav,
                   const VoiceEqSettings& s, double targetPeak = 0.7,
                   QString* error = nullptr, VoiceRenderStats* stats = nullptr);

} // namespace ttc
