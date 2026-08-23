// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/AudioCwSource.h"
#include "cw/CwDecoder.h"
#include "dsp/Fft.h"

#include <algorithm>
#include <cmath>

#include <QProcess>
#include <QSettings>

#include <rnnoise.h>

namespace ttc {

namespace {
// The SignaLink's PipeWire source node. A different sound device can be
// set via cw/audioDev (any substring parec accepts as --device, or a
// full node name from `pactl list sources short`).
const char* kDefaultDev =
    "alsa_input.usb-BurrBrown_from_Texas_Instruments_USB_AUDIO_CODEC-00"
    ".analog-stereo";
} // namespace

AudioCwSource::AudioCwSource(CwDecoder* sink, QObject* parent)
    : QObject(parent), sink_(sink) {}

bool AudioCwSource::running() const {
    return proc_ && proc_->state() != QProcess::NotRunning;
}

void AudioCwSource::start() {
    if (running()) return;
    const QString dev =
        QSettings().value("cw/audioDev", kDefaultDev).toString();
    if (!proc_) {
        proc_ = new QProcess(this);
        connect(proc_, &QProcess::readyReadStandardOutput, this,
                &AudioCwSource::onReadable);
        connect(proc_, &QProcess::errorOccurred, this, [this] {
            emit statusChanged("RADIO audio: parec failed — is "
                               "pulseaudio-utils installed?");
        });
        connect(proc_, &QProcess::finished, this, [this](int code, auto) {
            if (code != 0)
                emit statusChanged(
                    QString("RADIO audio: capture ended (%1) — check the "
                            "device in cw/audioDev").arg(code));
        });
    }
    carry_.clear();
    nrFill_ = 0;
    steadyHz_ = notchHz_ = -1.0;
    steadyN_ = notchGoneN_ = 0;
    if (nrSt_) { rnnoise_destroy(nrSt_); nrSt_ = nullptr; }
    proc_->start("parec",
                 {"--device=" + dev, "--format=s16le", "--rate=48000",
                  "--channels=1", "--latency-msec=60"});
    emit statusChanged("RADIO audio: " + dev.section('.', 0, 0)
                       + " @ 48 kHz");
}

void AudioCwSource::setNr(bool on) {
    nrOn_ = on;
    nrFill_ = 0;
}

void AudioCwSource::stop() {
    if (!running()) return;
    proc_->kill();
    proc_->waitForFinished(500);
    carry_.clear();
    pitchBuf_.clear();
    pitchFill_ = 0;
    steadyHz_ = notchHz_ = -1.0;
    steadyN_ = notchGoneN_ = 0;
    emit pitchMeasured(-1.0);
}

// Strongest tone in the CW audio range, fldigi-style: Hann + 8192-pt FFT
// (5.86 Hz bins at 48 k), parabolic peak interpolation, gated on the peak
// standing ~12 dB over the band's median so silence reads "--" instead of
// jittering noise.
//
// Constant-carrier rejection (live-found 2026-07-28): the readout sat
// on "1000 Hz" all night — a dead-steady tone at +30 dB over the idle
// floor. Root cause turned out to be station wiring (the SignaLink was
// patched to the OTHER radio, idle — recurring gotcha, second time),
// but any parked birdie, broadcast het or tuner-upper does the same to
// the readout, so the hardening stays. Discriminator: real CW is KEYED,
// so its window-to-window level collapses between elements; an artifact
// holds frequency AND level dead-steady. A tone steady within
// kSteadyDb/kTrackHz for kSteadyWin consecutive windows (~8 s) is
// convicted and notched out of the peak search until it disappears for
// ~60 s. One notch slot — one parked carrier at a time is the realistic
// case; hum sits below the 200 Hz search floor anyway. Display-only:
// nothing in the decode chain reads the tracker.
void AudioCwSource::setTargetPitch(int hz) {
    targetHz_ = hz;
    // Moving the target onto an existing conviction must free it now, not
    // 60 s from now — the readout would be blind there in the meantime.
    if (notchHz_ > 0.0 && hz > 0
        && std::abs(notchHz_ - double(hz)) <= 30.0) {
        notchHz_ = -1.0;
        notchGoneN_ = 0;
    }
}

void AudioCwSource::measurePitch() {
    constexpr int kN = 8192;
    constexpr double kTrackHz = 12.0;   // same-tone frequency wobble
    constexpr double kSteadyDb = 4.0;   // "unkeyed" level tolerance/window
    constexpr int kSteadyWin = 32;      // ~8 s at 4 measurements/s
    constexpr double kNotchHz = 30.0;   // exclusion half-width (a strong
                                        // carrier's Hann skirt can pass the
                                        // gate ~25 Hz out — sim-found)
    constexpr int kNotchDropWin = 240;  // carrier gone ~60 s -> forget it
    static Fft fft(kN);
    static std::vector<std::complex<float>> frame(kN);
    for (int i = 0; i < kN; ++i) {
        const float w =
            0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (kN - 1));
        frame[size_t(i)] = {pitchBuf_[size_t(i)] * w, 0.0f};
    }
    fft.forward(frame);
    const double binHz = 48000.0 / kN;
    const int b0 = int(200.0 / binHz), b1 = int(1200.0 / binHz);
    int ipAll = b0;                     // raw strongest (carrier tracking)
    int ip = -1;                        // strongest outside the notch
    int ipNotch = -1;                   // strongest inside it (upkeep)
    std::vector<float> mags;
    mags.reserve(size_t(b1 - b0 + 1));
    for (int b = b0; b <= b1; ++b) {
        const float m = std::norm(frame[size_t(b)]);
        mags.push_back(m);
        if (m > std::norm(frame[size_t(ipAll)])) ipAll = b;
        const bool notched = notchHz_ > 0.0
            && std::abs(b * binHz - notchHz_) <= kNotchHz;
        if (notched) {
            if (ipNotch < 0 || m > std::norm(frame[size_t(ipNotch)]))
                ipNotch = b;
        } else if (ip < 0 || m > std::norm(frame[size_t(ip)])) {
            ip = b;
        }
    }
    std::nth_element(mags.begin(), mags.begin() + mags.size() / 2,
                     mags.end());
    const float med = mags[mags.size() / 2];

    // Carrier bookkeeping runs on the RAW peak, readout gate or not.
    const float pkAll = std::norm(frame[size_t(ipAll)]);
    const bool rawTone =
        pkAll >= med * 16.0f && ipAll > b0 && ipAll < b1;
    if (rawTone) {
        const double hzAll = ipAll * binHz;
        const double dbAll = 10.0 * std::log10(double(pkAll) + 1e-30);
        if (steadyHz_ >= 0.0 && std::abs(hzAll - steadyHz_) <= kTrackHz
            && std::abs(dbAll - steadyDb_) <= kSteadyDb)
            ++steadyN_;
        else
            steadyN_ = 1;
        steadyHz_ = hzAll;
        steadyDb_ = dbAll;
        // ... but never at the sidetone pitch. SPOT is indistinguishable
        // from a birdie, and convicting the target frequency is
        // self-defeating AND self-sustaining: the readout then skips the
        // very station being zero-beat, whose own energy keeps the
        // conviction "alive" below.
        const bool protectedHz = targetHz_ > 0
            && std::abs(hzAll - double(targetHz_)) <= kNotchHz;
        if (steadyN_ >= kSteadyWin && !protectedHz) notchHz_ = hzAll;
    } else {
        steadyN_ = 0;
        steadyHz_ = -1.0;
    }
    // Notch upkeep probes the notch REGION's own energy: while a real
    // station is being copied the raw peak sits on the station, but the
    // carrier underneath is still there and must stay convicted. Only a
    // genuinely quiet notch region for ~60 s clears it.
    if (notchHz_ > 0.0) {
        const bool alive = ipNotch >= 0
            && std::norm(frame[size_t(ipNotch)]) >= med * 16.0f;
        if (alive) {
            notchHz_ = ipNotch * binHz;   // follow slow drift
            notchGoneN_ = 0;
        } else if (++notchGoneN_ >= kNotchDropWin) {
            notchHz_ = -1.0;
        }
    }

    if (ip < 0) { emit pitchMeasured(-1.0); return; }
    const float pk = std::norm(frame[size_t(ip)]);
    if (pk < med * 16.0f || ip <= b0 || ip >= b1) {   // ~12 dB gate
        emit pitchMeasured(-1.0);
        return;
    }
    const double ym = std::abs(frame[size_t(ip - 1)]),
                 y0 = std::abs(frame[size_t(ip)]),
                 yp = std::abs(frame[size_t(ip + 1)]);
    const double den = ym - 2.0 * y0 + yp;
    const double d = std::abs(den) > 1e-12
        ? std::clamp(0.5 * (ym - yp) / den, -0.5, 0.5) : 0.0;
    emit pitchMeasured((ip + d) * binHz);
}

void AudioCwSource::onReadable() {
    QByteArray data = carry_ + proc_->readAllStandardOutput();
    const int usable = data.size() & ~1;   // whole int16 samples only
    carry_ = data.mid(usable);
    const int n = usable / 2;
    if (n <= 0 || !sink_) return;
    const auto* s = reinterpret_cast<const int16_t*>(data.constData());
    constexpr float kNorm = 1.0f / 32768.0f;
    // Pitch meter: rolling raw window, one measurement per quarter second.
    constexpr int kPitchN = 8192;
    for (int i = 0; i < n; ++i) {
        pitchBuf_.push_back(s[i] * kNorm);
        if (int(pitchBuf_.size()) > kPitchN)
            pitchBuf_.erase(pitchBuf_.begin(),
                            pitchBuf_.end() - kPitchN);
        if (++pitchFill_ >= 12000 && int(pitchBuf_.size()) == kPitchN) {
            pitchFill_ = 0;
            measurePitch();
        }
    }
    if (!nrOn_) {
        buf_.resize(size_t(n));
        for (int i = 0; i < n; ++i)
            buf_[size_t(i)] = {s[i] * kNorm, 0.0f};
        sink_->processIq(buf_.data(), buf_.size());
        return;
    }
    // RNNoise path: 480-sample frames, s16-range floats in and out.
    if (!nrSt_) nrSt_ = rnnoise_create(nullptr);
    const int fs = rnnoise_get_frame_size();
    if (int(nrIn_.size()) != fs) nrIn_.assign(size_t(fs), 0.0f);
    buf_.clear();
    static thread_local std::vector<float> fout;
    fout.resize(size_t(fs));
    for (int i = 0; i < n; ++i) {
        nrIn_[size_t(nrFill_)] = float(s[i]);
        if (++nrFill_ < fs) continue;
        nrFill_ = 0;
        rnnoise_process_frame(nrSt_, fout.data(), nrIn_.data());
        for (int k = 0; k < fs; ++k)
            buf_.push_back({fout[size_t(k)] * kNorm, 0.0f});
    }
    if (!buf_.empty()) sink_->processIq(buf_.data(), buf_.size());
}

} // namespace ttc
