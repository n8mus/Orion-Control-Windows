// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <complex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/ClientComp.h"
#include "dsp/ClientEq.h"
#include "dsp/ClientFinalLimiter.h"
#include "dsp/ClientGate.h"
#include "radio/RadioController.h"   // Mode

struct DenoiseState;
namespace ttc { class RadeRx; }

namespace ttc {

// SDR receive audio (phase 3): the RSP2's IQ demodulated by WDSP — Thetis's
// DSP library (Warren Pratt NR0V; AetherSDR's vendored 2.10, third_party/
// wdsp) — with its noise tools: NB / NB2 impulse blankers on the IQ, SNB,
// ANF, and NR (LMS) / NR2 (spectral) / NR3 (neural), plus RNNoise on the
// audio. It follows the Orion: tuning offset, mode, filter edges, CW pitch.
//
//   IQ worker thread:  processIq — mix the dial to 0 Hz, FIR-decimate to
//                      125 kHz, queue (cheap; the IQ fan-out's budget)
//   own DSP thread:    1000-sample blocks -> NB/NB2 -> WDSP RXA (bandpass,
//                      SNB, ANF, NR*, AGC, demod; 48 kHz out) -> RNNoise ->
//                      gate -> RX EQ -> compressor -> volume/BOOST ->
//                      final limiter -> MUTE / TX mute -> pacat on the PC's
//                      default output (AetherSDR's RX order: NR first, then
//                      gate, EQ, compressor, final output)
//
// Audio is held near kTargetMs of queue the way the LIVE mic is (the SDR
// and the sound card are different clocks): quiet 8 ms blocks are dropped
// when it grows. Muted (silence, stream kept) while the radio transmits —
// the RSP2 shares the antenna field and its audio would be the operator's
// own hash.
class SdrRx {
public:
    enum class NoiseBlanker { Off = 0, NB = 1, NB2 = 2 };   // never both (WDSP)
    enum class NoiseReduction { Off = 0, NR = 1, NR2 = 2, NR3 = 3 };
    enum class Agc { Fixed = 0, Long = 1, Slow = 2, Medium = 3, Fast = 4 };
    struct Settings {
        float volumeDb = -6.f;
        Agc agc = Agc::Medium;
        NoiseBlanker nb = NoiseBlanker::Off;
        NoiseReduction nr = NoiseReduction::Off;
        bool snb = false;                          // spectral noise blanker
        bool anf = false;                          // auto notch
        bool rnnoise = false;                      // AetherSDR-style, on audio
        // RX EQ: AetherSDR's ClientEq on the finished audio (after
        // RNNoise), the voice keyer's 8-slot layout (VoiceEqSettings).
        bool eqOn = false;
        std::array<ClientEq::BandParams, 8> eq{};
        // RX dynamics (AetherSDR's ClientGate / ClientComp, the same
        // classes as the voice chain). Levels are dBFS BEFORE the volume,
        // so turning the volume never moves a threshold.
        bool gateOn = false;                       // quiets the hiss between overs
        float gateThrDb = -35.f;
        float gateFloorDb = -20.f;                 // most it turns down
        bool compOn = false;                       // evens out fades / loud stations
        float compThrDb = -24.f;
        float compRatio = 2.f;                     // AetherSDR: 1.5-2:1 on RX
        float compMakeupDb = 4.f;
        bool mute = false;
        bool boost = false;                        // +kBoostDb into the limiter
        // FreeDV RADE: the demodulated USB audio is the modem; what plays
        // is RadeRx's decoded speech (then gate/EQ/comp/volume as usual).
        bool rade = false;
        // ...but play the raw sideband (freedv-gui's "Analog"): RADE still
        // decodes — sync, SNR, callsigns — you hear the SSB audio.
        bool radeAnalog = false;
    };
    static constexpr float kBoostDb = 6.f;
    static constexpr float kCeilingDb = -1.f;      // final limiter, always on

    // Meter snapshot (any thread; the stages publish per block).
    struct Levels {
        float inDb = -120.f;                       // into the gate, pre-volume
        bool gateOpen = true;
        float compGrDb = 0.f;                      // <= 0
        float limGrDb = 0.f;                       // final limiter, <= 0
        float outDb = -120.f;                      // to the speakers
    };
    Levels levels() const;

    // RADE status (any thread; meaningless unless Settings::rade).
    struct RadeStatus {
        bool available = false;                    // built with RADE
        bool sync = false;
        float snrDb = 0.f, freqOffsetHz = 0.f;
        std::string lastCall;                      // end-of-over callsign
        int calls = 0;                             // bumps per decoded EOO
    };
    RadeStatus radeStatus() const;
    bool radeDecoding() const;                     // running with Settings::rade

    SdrRx();
    ~SdrRx();
    SdrRx(const SdrRx&) = delete;
    SdrRx& operator=(const SdrRx&) = delete;

    void setInputRate(double rate);                // before start (replays)
    bool start(const std::string& sinkNode);       // "" = default output
    // Tests: every output block also goes here; startTapOnly() runs the
    // chain with no player at all (nothing reaches the speakers).
    void setAudioTap(std::function<void(const float*, int)> tap) { tap_ = std::move(tap); }
    bool startTapOnly();
    // Copy Assist's feed: the audio before the gate/EQ/compressor (raw
    // demod, or RADE's decoded speech) plus "we are transmitting". Set it
    // before start(); called on the DSP thread.
    void setAsrTap(std::function<void(const float*, int, bool muted)> f) { asrTap_ = std::move(f); }
    void stop();
    bool running() const { return running_.load(); }
    // The WDSP channel is open (the first open loads the NR3 models and
    // plans FFTs — noticeably slow); IQ queued before then is kept (1 s).
    bool ready() const { return ready_.load(); }

    // Dial position relative to the SDR LO (Hz, negative = below), as the
    // CW decoder takes it. Thread-safe; applied at the next IQ block.
    void retune(double dialOffsetHz);
    // Mode and filter edges as RF offsets from the dial (edgesFromRig),
    // CW sidetone pitch. Thread-safe.
    void setDemod(Mode m, int loHz, int hiHz, int cwPitchHz);
    void setSettings(const Settings& s);           // thread-safe
    void setMuted(bool m) { muted_.store(m); }

    void processIq(const std::complex<float>* d, size_t n);   // IQ worker

    // For the RX menu's readouts.
    double backlogMs() const;
    float outputPeakDb() const { return outPeakDb_.load(); }
    float modemPeakDb() const { return modemPeakDb_.load(); }   // WDSP out, before RADE
    float inputPeakDb() const { return inPeakDb_.load(); }      // into gate/EQ (RADE speech in FDV)
    int droppedBlocks() const { return dropped_.load(); }

    static constexpr double kDecimRate = 125000.0; // after our FIR decimator
    // WDSP needs integer rate ratios in -> DSP -> out (channel.c sizes its
    // buffers with integer division — 125k into 48k overran our output by
    // 116 samples, ASan-found), and NR3 a DSP rate that is a multiple of
    // 16 kHz: WDSP's own rational resampler takes 125 kHz to 96 kHz first.
    static constexpr int kWdspInRate = 96000;
    // Block sizes must nest too (create_iobuffs assumes the DSP-side block
    // is a whole number of input blocks; 768 vs 2048 overran its input ring,
    // ASan-found) — powers of two, as Thetis runs it.
    static constexpr int kInSize = 1024;           // 10.7 ms at 96 kHz
    static constexpr int kOutRate = 48000;
    static constexpr int kOutSize = 512;           // kInSize / (96k / 48k)

private:
    void dspLoop();
    void applySettingsDsp();                       // DSP thread only
    void applyDemodDsp();
    bool openPlayer(const std::string& sinkNode);
    void closePlayer();
    void writeAudio(const float* x, int n);

    // ---- IQ side (IQ worker thread) ----
    double inputRate_ = 1e6;
    int decim_ = 8;
    std::vector<float> taps_;
    std::vector<std::complex<float>> dline_;
    size_t dpos_ = 0;
    int phase_ = 0;
    std::complex<double> lo_{1.0, 0.0}, loStep_{1.0, 0.0};
    int renorm_ = 0;
    std::atomic<double> pendingOffset_{0.0};
    std::atomic<bool> retunePending_{true};
    std::atomic<int> pitchShiftHz_{0};             // CW: carrier -> +/- pitch

    // ---- hand-off ----
    mutable std::mutex mux_;
    std::condition_variable cv_;
    std::deque<std::complex<float>> queue_;        // decimated, 125 kHz

    // ---- DSP thread ----
    std::thread dsp_;
    std::atomic<bool> running_{false}, stop_{false}, ready_{false};
    std::mutex setMux_;
    Settings settings_, appliedSettings_;
    bool settingsDirty_ = true;
    Mode mode_ = Mode::USB;
    int bwLo_ = 100, bwHi_ = 2800, pitch_ = 600;
    bool demodDirty_ = true;
    std::atomic<bool> muted_{false};
    ClientGate gate_;                              // RX dynamics, all at 48 kHz
    ClientEq eq_;
    ClientComp comp_;
    ClientFinalLimiter limit_;
    void emitAudio(float* x, int n);               // dynamics -> writeAudio
    DenoiseState* rnn_ = nullptr;
    std::atomic<RadeRx*> rade_{nullptr};           // DSP thread makes it; UI reads status
    std::deque<float> radeOut_;                    // decoded speech, 48 kHz
    bool radePrimed_ = false;
    void radeBlock(const float* modem, int n);     // DSP thread
    std::vector<float> rnnIn_;                     // 480-sample frames

    // ---- output pipe (pacat) ----
    int pipeW_ = -1, pipeProbe_ = -1;
    int playerPid_ = -1;
    std::atomic<float> outPeakDb_{-120.f}, inPeakDb_{-120.f}, modemPeakDb_{-120.f};
    std::atomic<int> dropped_{0};
    std::function<void(const float*, int)> tap_;
    std::function<void(const float*, int, bool)> asrTap_;
};

} // namespace ttc
