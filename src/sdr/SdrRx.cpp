// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdr/SdrRx.h"

#include <aether_wdsp.h>
#include <rnnoise.h>
#ifdef HAVE_RADE
#include "sdr/RadeRx.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// WDSP 2.10 exports these (upstream/*.c, PORT) but AetherSDR's facade
// header does not declare them — it drives NR through its own path.
// Signatures copied from the upstream sources.
extern "C" {
void SetRXAEMNRRun(int channel, int run);
void SetRXAEMNRgainMethod(int channel, int method);
void SetRXAEMNRnpeMethod(int channel, int method);
void SetRXAEMNRaeRun(int channel, int run);
void SetRXAEMNRPosition(int channel, int position);
void SetRXAANRRun(int channel, int run);
void SetRXAANRPosition(int channel, int position);
void SetRXAANFRun(int channel, int run);
void SetRXAANFPosition(int channel, int position);
void SetRXASNBARun(int channel, int run);
void SetRXANNRRun(int channel, int setit);
void SetRXANNRPosition(int channel, int position);
void fexchange0(int channel, double* in, double* out, int* error);
struct _resample;
typedef struct _resample* RESAMPLE;
RESAMPLE create_resample(int run, int size, double* in, double* out, int in_rate,
                         int out_rate, double fc, int ncoef, double gain);
void destroy_resample(RESAMPLE a);
int xresample(RESAMPLE a);
}

namespace ttc {

namespace {
constexpr int kCh = 0;                             // WDSP channel + blanker id
constexpr int kDspSize = 1024;                     // 21 ms at 48 kHz
constexpr int kBytesPerMs = 96;                    // 48 kHz s16 mono
constexpr int kTargetBytes = 60 * kBytesPerMs;     // ~60 ms queued toward pacat
constexpr int kHardBytes = 160 * kBytesPerMs;
constexpr float kQuiet = 0.01f;                    // -40 dBFS block peak
constexpr size_t kMaxQueue = size_t(SdrRx::kDecimRate);   // 1 s at 125 kHz
constexpr int kRsIn = 1000;                        // 125 kHz samples per resample call

int rxaModeFor(Mode m) {
    switch (m) {
    case Mode::LSB: return 0;                      // RXA_LSB
    case Mode::USB: return 1;                      // RXA_USB
    case Mode::CWL: return 0;                      // carrier shifted to -pitch
    case Mode::CWU: return 1;                      // carrier shifted to +pitch
    case Mode::FM:  return 5;                      // RXA_FM
    case Mode::AM:  return 6;                      // RXA_AM
    }
    return 1;
}
}  // namespace

SdrRx::SdrRx() { setInputRate(1e6); }

SdrRx::~SdrRx() { stop(); }

// Windowed-sinc decimator, like SkimStft's but longer (12 taps per decim
// step, Blackman): this one is listened to, and the first alias lands
// ~70 dB down instead of ~50.
void SdrRx::setInputRate(double rate) {
    inputRate_ = rate;
    decim_ = std::max(1, int(std::lround(rate / kDecimRate)));
    const int n = 12 * decim_;
    taps_.resize(size_t(n));
    const double fc = 0.40 * kDecimRate;           // 50 kHz: WDSP filters after
    double sum = 0;
    for (int k = 0; k < n; ++k) {
        const double t = k - (n - 1) / 2.0;
        const double x = 2.0 * M_PI * fc / rate * t;
        const double sinc = std::abs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * k / (n - 1))
                       + 0.08 * std::cos(4 * M_PI * k / (n - 1));
        taps_[size_t(k)] = float(sinc * w);
        sum += taps_[size_t(k)];
    }
    for (float& t : taps_) t = float(t / sum);
    size_t sz = 1;
    while (sz < taps_.size() + 1) sz <<= 1;
    dline_.assign(sz, {0.f, 0.f});
    dpos_ = 0;
    phase_ = 0;
    retunePending_.store(true);
}

void SdrRx::retune(double dialOffsetHz) {
    pendingOffset_.store(dialOffsetHz);
    retunePending_.store(true, std::memory_order_release);
}

void SdrRx::setDemod(Mode m, int loHz, int hiHz, int cwPitchHz) {
    {
        std::lock_guard<std::mutex> lk(setMux_);
        mode_ = m;
        bwLo_ = loHz;
        bwHi_ = hiHz;
        pitch_ = cwPitchHz;
        demodDirty_ = true;
    }
    // CW: shift the carrier to +pitch (CWU) / -pitch (CWL) so a sideband
    // demod lands it on the operator's sidetone pitch — the radios are
    // carrier-at-dial, and so is the dial offset handed to retune().
    const int shift = m == Mode::CWU ? cwPitchHz : m == Mode::CWL ? -cwPitchHz : 0;
    if (pitchShiftHz_.exchange(shift) != shift) retunePending_.store(true);
}

void SdrRx::setSettings(const Settings& s) {
    std::lock_guard<std::mutex> lk(setMux_);
    settings_ = s;
    settingsDirty_ = true;
}

void SdrRx::processIq(const std::complex<float>* d, size_t n) {
    if (!running_.load(std::memory_order_relaxed)) return;
    if (retunePending_.exchange(false, std::memory_order_acquire)) {
        // Mix the dial (minus the CW pitch shift) to 0 Hz.
        const double f = pendingOffset_.load() - pitchShiftHz_.load();
        const double inc = -2.0 * M_PI * f / inputRate_;
        loStep_ = {std::cos(inc), std::sin(inc)};
        lo_ = {1.0, 0.0};
    }
    const size_t mask = dline_.size() - 1;
    std::vector<std::complex<float>> out;
    out.reserve(n / size_t(decim_) + 1);
    for (size_t i = 0; i < n; ++i) {
        dline_[dpos_ & mask] = std::complex<float>(std::complex<double>(d[i]) * lo_);
        ++dpos_;
        lo_ *= loStep_;
        if (++renorm_ >= 4096) { renorm_ = 0; lo_ /= std::abs(lo_); }
        if (++phase_ < decim_) continue;
        phase_ = 0;
        std::complex<float> acc(0.f, 0.f);
        size_t idx = (dpos_ - 1) & mask;
        for (const float t : taps_) {
            acc += t * dline_[idx];
            idx = (idx - 1) & mask;
        }
        out.push_back(acc);
    }
    {
        std::lock_guard<std::mutex> lk(mux_);
        queue_.insert(queue_.end(), out.begin(), out.end());
        // A stalled DSP thread must not grow this forever: keep 1 s.
        if (queue_.size() > kMaxQueue)
            queue_.erase(queue_.begin(), queue_.begin() + long(queue_.size() - kMaxQueue));
    }
    cv_.notify_one();
}

bool SdrRx::start(const std::string& sinkNode) {
    if (running_.load()) return true;
    if (!openPlayer(sinkNode)) return false;
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(mux_);
        queue_.clear();
    }
    dropped_.store(0);
    running_.store(true);
    dsp_ = std::thread([this] { dspLoop(); });
    return true;
}

bool SdrRx::startTapOnly() {
    if (running_.load()) return true;
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(mux_);
        queue_.clear();
    }
    running_.store(true);
    dsp_ = std::thread([this] { dspLoop(); });
    return true;
}

void SdrRx::stop() {
    if (!running_.load() && !dsp_.joinable()) return;
    running_.store(false);
    stop_.store(true);
    cv_.notify_all();
    if (dsp_.joinable()) dsp_.join();
    closePlayer();
}

void SdrRx::applyDemodDsp() {
    Mode m;
    int lo, hi, pitch;
    {
        std::lock_guard<std::mutex> lk(setMux_);
        if (!demodDirty_) return;
        demodDirty_ = false;
        m = mode_; lo = bwLo_; hi = bwHi_; pitch = pitch_;
    }
    SetRXAMode(kCh, rxaModeFor(m));
    // Passband in WDSP's baseband: the RF edges, moved with the CW pitch
    // shift. SSB keeps 100 Hz off the carrier (hum, DC) like a real
    // SSB filter's low cut.
    double flo = lo, fhi = hi;
    if (m == Mode::CWU) { flo += pitch; fhi += pitch; }
    if (m == Mode::CWL) { flo -= pitch; fhi -= pitch; }
    if (m == Mode::USB) flo = std::max(flo, 100.0);
    if (m == Mode::LSB) fhi = std::min(fhi, -100.0);
    if (fhi - flo < 50) fhi = flo + 50;
    RXASetPassband(kCh, flo, fhi);
}

void SdrRx::applySettingsDsp() {
    Settings s;
    {
        std::lock_guard<std::mutex> lk(setMux_);
        if (!settingsDirty_) return;
        settingsDirty_ = false;
        s = settings_;
    }
    SetRXAAGCMode(kCh, int(s.agc));
    // Blankers: one or none (they share a detector — WDSP's rule).
    SetEXTANBRun(kCh, s.nb == NoiseBlanker::NB ? 1 : 0);
    SetEXTNOBRun(kCh, s.nb == NoiseBlanker::NB2 ? 1 : 0);
    SetRXASNBARun(kCh, s.snb ? 1 : 0);
    SetRXAANFRun(kCh, s.anf ? 1 : 0);
    // Noise reduction: one at a time, as Thetis offers them.
    SetRXAANRRun(kCh, s.nr == NoiseReduction::NR ? 1 : 0);
    SetRXAEMNRRun(kCh, s.nr == NoiseReduction::NR2 ? 1 : 0);
    SetRXANNRRun(kCh, s.nr == NoiseReduction::NR3 ? 1 : 0);
    if (s.rnnoise && !rnn_) rnn_ = rnnoise_create(nullptr);
    eq_.setActiveBandCount(int(s.eq.size()));
    for (int i = 0; i < int(s.eq.size()); ++i) eq_.setBand(i, s.eq[size_t(i)]);
    if (s.eqOn && !eq_.isEnabled()) eq_.reset();   // no stale state on the way in
    eq_.setEnabled(s.eqOn);
    if (s.gateOn && !gate_.isEnabled()) gate_.reset();
    gate_.setEnabled(s.gateOn);
    gate_.setThresholdDb(s.gateThrDb);
    gate_.setFloorDb(s.gateFloorDb);
    gate_.setRatio(3.f);                           // a firm expander, not a slam
    gate_.setHoldMs(150.f);                        // rides through CW/word gaps
    gate_.setReleaseMs(300.f);
    gate_.setReturnDb(3.f);
    gate_.setLookaheadMs(2.f);
    if (s.compOn && !comp_.isEnabled()) comp_.reset();
    comp_.setEnabled(s.compOn);
    comp_.setThresholdDb(s.compThrDb);
    comp_.setRatio(s.compRatio);
    comp_.setAttackMs(10.f);
    comp_.setReleaseMs(250.f);
    comp_.setKneeDb(6.f);
    comp_.setMakeupDb(s.compMakeupDb);
    comp_.setLimiterEnabled(false);                // the final limiter is after it
    appliedSettings_ = s;
}

void SdrRx::dspLoop() {
    // Thetis-style RX channel: 96 kHz in, 48 kHz DSP and out, blocking
    // exchange on this thread (the channel runs its own DSP thread).
    OpenChannel(kCh, kInSize, kDspSize, kWdspInRate, kOutRate, kOutRate,
                0 /*RX*/, 1 /*on*/, 0.010, 0.025, 0.0, 0.010, 1 /*block*/);
    // Blankers on the IQ, pihpsdr's fixed parameter set (aether_wdsp.h).
    create_anbEXT(kCh, 0, kInSize, kWdspInRate, 0.0001, 0.0001, 0.0001, 0.05, 20.0);
    create_nobEXT(kCh, 0, 2 /*mean-hold*/, kInSize, kWdspInRate,
                  0.0001, 0.0001, 0.0001, 0.05, 20.0);
    std::vector<double> rsIn(size_t(kRsIn) * 2), rsOut(size_t(kRsIn) * 2 + 64);
    RESAMPLE rs = create_resample(1, kRsIn, rsIn.data(), rsOut.data(),
                                  int(kDecimRate), kWdspInRate, 0.0, 0, 1.0);
    std::deque<double> at96;                       // interleaved I/Q at 96 kHz
    gate_.prepare(kOutRate);
    eq_.prepare(kOutRate);
    comp_.prepare(kOutRate);
    limit_.prepare(kOutRate);
    limit_.setEnabled(true);                       // ears + headset: never clip
    limit_.setCeilingDb(kCeilingDb);
    limit_.setDcBlockEnabled(false);               // WDSP's output has none
    ready_.store(true);
    SetRXAEMNRgainMethod(kCh, 2);                  // Thetis NR2 defaults
    SetRXAEMNRnpeMethod(kCh, 0);
    SetRXAEMNRaeRun(kCh, 1);
    SetRXAEMNRPosition(kCh, 1);
    SetRXAANRPosition(kCh, 1);
    SetRXAANFPosition(kCh, 1);
    SetRXAAGCTop(kCh, 90.0);

    std::vector<double> in(size_t(kInSize) * 2), out(size_t(kOutSize) * 2);
    std::vector<float> audio(kOutSize);
    while (!stop_.load()) {
        if (at96.size() < size_t(kInSize) * 2) {
            {
                std::unique_lock<std::mutex> lk(mux_);
                cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
                    return queue_.size() >= size_t(kRsIn) || stop_.load();
                });
                if (stop_.load()) break;
                if (queue_.size() < size_t(kRsIn)) continue;
                // Conjugated on the way in: WDSP keeps the HPSDR IQ
                // handedness, the mirror of the RSP2 stream's — without
                // this, USB demodulated the lower sideband (test-found).
                for (int i = 0; i < kRsIn; ++i) {
                    rsIn[size_t(2 * i)] = queue_.front().real();
                    rsIn[size_t(2 * i + 1)] = -queue_.front().imag();
                    queue_.pop_front();
                }
            }
            const int got = xresample(rs);         // 125 kHz -> 96 kHz
            at96.insert(at96.end(), rsOut.begin(), rsOut.begin() + 2 * got);
            continue;
        }
        for (int i = 0; i < kInSize * 2; ++i) {
            in[size_t(i)] = at96.front();
            at96.pop_front();
        }
        applyDemodDsp();
        applySettingsDsp();
        const Settings& s = appliedSettings_;
        if (s.nb == NoiseBlanker::NB) xanbEXT(kCh, in.data(), in.data());
        if (s.nb == NoiseBlanker::NB2) xnobEXT(kCh, in.data(), in.data());
        int err = 0;
        fexchange0(kCh, in.data(), out.data(), &err);
        // WDSP's AGC drives its output to ~full scale (measured: 0 dBFS
        // peaks on real 40 m CW) — keep 6 dB of headroom under the volume.
        // The volume is applied at the end (emitAudio), after the dynamics.
        for (int i = 0; i < kOutSize; ++i) audio[size_t(i)] = float(out[size_t(2 * i)]) * 0.5f;
        if (s.rade) {
            radeBlock(audio.data(), kOutSize);
        } else if (s.rnnoise && rnn_) {
            // RNNoise works on 480-sample frames at 48 kHz, int16 scale.
            rnnIn_.insert(rnnIn_.end(), audio.begin(), audio.end());
            std::vector<float> done;
            while (rnnIn_.size() >= 480) {
                float f[480];
                for (int k = 0; k < 480; ++k) f[k] = rnnIn_[size_t(k)] * 32768.f;
                rnnoise_process_frame(rnn_, f, f);
                for (int k = 0; k < 480; ++k) done.push_back(f[k] / 32768.f);
                rnnIn_.erase(rnnIn_.begin(), rnnIn_.begin() + 480);
            }
            if (!done.empty()) emitAudio(done.data(), int(done.size()));
        } else {
            rnnIn_.clear();
            emitAudio(audio.data(), kOutSize);
        }
    }
    ready_.store(false);
    destroy_resample(rs);
    destroy_nobEXT(kCh);
    destroy_anbEXT(kCh);
    CloseChannel(kCh);
    if (rnn_) { rnnoise_destroy(rnn_); rnn_ = nullptr; }
#ifdef HAVE_RADE
    delete rade_.exchange(nullptr);
#endif
    radeOut_.clear();
    radePrimed_ = false;
}

// pacat fed through our own pipe: one page of pipe (~43 ms) plus what we
// choose to queue, measured with FIONREAD on a kept read handle — the LIVE
// mic's backlog control, without QProcess (this runs on the DSP thread).
bool SdrRx::openPlayer(const std::string& sinkNode) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[1], F_SETPIPE_SZ, 8192);
    std::vector<std::string> args{"pacat", "--playback", "--raw", "--format=s16le",
                                  "--rate=48000", "--channels=1", "--latency-msec=40"};
    if (!sinkNode.empty()) args.push_back("--device=" + sinkNode);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, "pacat", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { ::close(fds[0]); ::close(fds[1]); return false; }
    playerPid_ = pid;
    pipeProbe_ = fds[0];                           // kept only for FIONREAD
    pipeW_ = fds[1];
    ::fcntl(pipeW_, F_SETFL, O_NONBLOCK);
    ::signal(SIGPIPE, SIG_IGN);
    return true;
}

void SdrRx::closePlayer() {
    if (pipeW_ >= 0) ::close(pipeW_);
    if (pipeProbe_ >= 0) ::close(pipeProbe_);
    pipeW_ = pipeProbe_ = -1;
    if (playerPid_ > 0) {
        ::kill(playerPid_, SIGTERM);
        ::waitpid(playerPid_, nullptr, 0);
    }
    playerPid_ = -1;
}

double SdrRx::backlogMs() const {
    int n = 0;
    if (pipeProbe_ >= 0) ::ioctl(pipeProbe_, FIONREAD, &n);
    return double(n) / kBytesPerMs;
}

void SdrRx::emitAudio(float* x, int n) {
    const Settings& s = appliedSettings_;
    float pk = 0.f;                                // own meter: the gate's only
    for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(x[i]));   // runs when on
    inPeakDb_.store(20.f * std::log10(std::max(pk, 1e-6f)));
    if (asrTap_) asrTap_(x, n, muted_.load());
    if (s.gateOn) gate_.process(x, n, 1);
    if (eq_.isEnabled()) eq_.process(x, n, 1);
    if (s.compOn) comp_.process(x, n, 1);
    const float g = std::pow(10.f, (s.volumeDb + (s.boost ? kBoostDb : 0.f)) / 20.f);
    for (int i = 0; i < n; ++i) x[i] *= g;
    limit_.process(x, n, 1);
    if (s.mute) std::fill(x, x + n, 0.f);
    writeAudio(x, n);
}

// RADE: the block of modem audio in, one block of speech out — always
// exactly n samples, so the player sees a steady stream. The decoder
// hands speech over in 120 ms bursts (one modem frame = 12 vocoder
// frames), so a FIFO primed to ~130 ms smooths them; silence while
// unsynced or underrun (then it primes again).
void SdrRx::radeBlock(const float* modem, int n) {
    std::vector<float> blk(size_t(n), 0.f);
    {
        float pk = 0.f;
        for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(modem[i]));
        modemPeakDb_.store(20.f * std::log10(std::max(pk, 1e-6f)));
    }
#ifdef HAVE_RADE
    if (!rade_.load()) rade_.store(new RadeRx);
    std::vector<float> sp;
    // Transmitting (muted): the RSP2 hears our own signal at full strength
    // — don't decode it (live-found: our end of over decoded as "heard
    // N8EM" and went to the Reporter). The decoder resyncs after.
    if (!muted_.load()) rade_.load()->process(modem, n, sp);
    radeOut_.insert(radeOut_.end(), sp.begin(), sp.end());
    if (radeOut_.size() > 48000) radeOut_.erase(radeOut_.begin(), radeOut_.end() - 12000);
    if (!radePrimed_ && radeOut_.size() >= 6240) radePrimed_ = true;
    if (appliedSettings_.radeAnalog) {             // hear the sideband itself
        std::copy(modem, modem + n, blk.begin());
        radeOut_.clear();
        radePrimed_ = false;
    } else if (radePrimed_) {
        if (radeOut_.size() >= size_t(n)) {
            std::copy(radeOut_.begin(), radeOut_.begin() + n, blk.begin());
            radeOut_.erase(radeOut_.begin(), radeOut_.begin() + n);
        } else {
            radePrimed_ = false;                   // underrun: re-prime
        }
    }
#else
    (void)modem;
#endif
    emitAudio(blk.data(), n);
}

bool SdrRx::radeDecoding() const {
    if (!running_.load()) return false;
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(setMux_));
    return settings_.rade;
}

SdrRx::RadeStatus SdrRx::radeStatus() const {
    RadeStatus st;
#ifdef HAVE_RADE
    st.available = true;
    // rade_ is created and deleted on the DSP thread; its reads are atomic
    // or locked, and stop() joins before the delete.
    if (const RadeRx* r = rade_.load()) {
        st.sync = r->sync();
        st.snrDb = r->snrDb();
        st.freqOffsetHz = r->freqOffsetHz();
        st.lastCall = r->lastCallsign();
        st.calls = r->callsignCount();
    }
#endif
    return st;
}

SdrRx::Levels SdrRx::levels() const {
    Levels l;
    const bool on = running_.load();
    l.inDb = on ? inPeakDb_.load() : -120.f;
    l.gateOpen = gate_.isEnabled() ? gate_.gateOpen() : true;
    l.compGrDb = comp_.isEnabled() ? comp_.gainReductionDb() : 0.f;
    l.limGrDb = limit_.gainReductionDb();
    l.outDb = on ? outPeakDb_.load() : -120.f;
    return l;
}

void SdrRx::writeAudio(const float* x, int n) {
    if (tap_ && n > 0) tap_(x, n);
    if (pipeW_ < 0 || n <= 0) return;
    float pk = 0.f;
    for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(x[i]));
    const bool muted = muted_.load();
    outPeakDb_.store(muted ? -120.f : 20.f * std::log10(std::max(pk, 1e-6f)));
    int queued = 0;
    if (pipeProbe_ >= 0) ::ioctl(pipeProbe_, FIONREAD, &queued);
    if (queued > kHardBytes || (queued > kTargetBytes && pk < kQuiet)) {
        dropped_.fetch_add(1);                     // clock drift: drop a quiet block
        return;
    }
    std::vector<int16_t> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        pcm[size_t(i)] = muted ? int16_t(0)
            : int16_t(std::clamp(int(std::lround(x[i] * 32767.f)), -32768, 32767));
    const ssize_t w = ::write(pipeW_, pcm.data(), pcm.size() * 2);
    if (w < 0) dropped_.fetch_add(1);              // pipe full: drop
}

} // namespace ttc
