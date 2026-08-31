// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/ClipDeck.h"

#include <QFile>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <csignal>

#if defined(Q_OS_WIN) && defined(HAVE_QTMULTIMEDIA)
#include "audio/AudioIo.h"
#define TTC_CLIPDECK_QTMM 1
#endif

namespace ttc {

#ifdef TTC_CLIPDECK_QTMM
namespace {
// The 44-byte canonical PCM header; sizes are patched on finalize.
QByteArray wavHeader(int rate, int channels, int bits) {
    QByteArray h;
    h.reserve(44);
    const auto u16 = [&](quint16 v) { h.append(char(v)); h.append(char(v >> 8)); };
    const auto u32 = [&](quint32 v) { u16(quint16(v)); u16(quint16(v >> 16)); };
    h.append("RIFF"); u32(36);                       // patched later
    h.append("WAVEfmt "); u32(16);
    u16(1); u16(quint16(channels)); u32(quint32(rate));
    u32(quint32(rate * channels * bits / 8));
    u16(quint16(channels * bits / 8)); u16(quint16(bits));
    h.append("data"); u32(0);                        // patched later
    return h;
}

// Parse a PCM WAV into s16 mono (downmixing stereo); false when the file
// isn't 16-bit PCM. Mirrors normalizeWav's chunk walker.
bool loadWavS16Mono(const QString& path, QByteArray& pcm, int& rate) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = f.readAll();
    if (b.size() < 44 || !b.startsWith("RIFF") || b.mid(8, 4) != "WAVE")
        return false;
    const auto u16 = [&](qsizetype o) {
        return quint16(quint8(b[o])) | quint16(quint8(b[o + 1])) << 8;
    };
    const auto u32 = [&](qsizetype o) {
        return quint32(u16(o)) | quint32(u16(o + 2)) << 16;
    };
    int channels = 0, bits = 0;
    qsizetype dataOff = -1, dataLen = 0;
    for (qsizetype o = 12; o + 8 <= b.size();) {
        const QByteArray id = b.mid(o, 4);
        const qsizetype len = u32(o + 4);
        if (id == "fmt " && len >= 16) {
            if (u16(o + 8) != 1) return false;       // PCM only
            channels = u16(o + 10);
            rate = int(u32(o + 12));
            bits = u16(o + 22);
        }
        if (id == "data") {
            dataOff = o + 8;
            dataLen = std::min<qsizetype>(qsizetype(len), b.size() - dataOff);
            break;
        }
        o += 8 + len + (len & 1);
    }
    if (dataOff < 0 || bits != 16 || channels < 1) return false;
    const auto* s = reinterpret_cast<const int16_t*>(b.constData() + dataOff);
    const qsizetype frames = dataLen / (2 * channels);
    pcm.resize(frames * 2);
    auto* d = reinterpret_cast<int16_t*>(pcm.data());
    for (qsizetype i = 0; i < frames; ++i) {
        int acc = 0;
        for (int c = 0; c < channels; ++c) acc += s[i * channels + c];
        d[i] = int16_t(acc / channels);
    }
    return true;
}
} // namespace
#endif // TTC_CLIPDECK_QTMM

ClipDeck::ClipDeck(QObject* parent) : QObject(parent) {
    proc_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&proc_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus st) {
        state_ = State::Idle;
        emit finished();
        // A stop() lands as SIGINT (clean exit) or, if pw-cat ignored it, a
        // signal death (CrashExit) — neither is a failure worth reporting.
        // failed() goes out after finished() so its message isn't overwritten
        // by generic idle-state handling.
        if (!stopping_ && st == QProcess::NormalExit && code != 0)
            emit failed(QString::fromUtf8(proc_.readAll()).trimmed());
    });
    connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {        // no finished() from QProcess
            state_ = State::Idle;
            emit finished();
            emit failed("parecord/paplay not found (pulseaudio-utils missing?)");
        }
    });
}

ClipDeck::~ClipDeck() {
    if (state_ != State::Idle) {                   // finalize an in-flight WAV
        stop();
        proc_.waitForFinished(1000);
    }
}

#ifdef TTC_CLIPDECK_QTMM

// Windows deck: Qt Multimedia endpoints, WAV framing done here. Recording
// streams capture chunks straight into the file behind a placeholder
// header (patched on stop — the parecord-on-SIGINT contract, kept). File
// playback is pumped in paced slices against the sink's free space, then
// the pump watches for drain to declare finished().
bool ClipDeck::record(const QString& wavPath, const QString& targetNode) {
    if (state_ != State::Idle) return false;
    if (!cap_) {
        cap_ = new AudioCapture(this);
        connect(cap_, &AudioCapture::chunk, this, [this](const QByteArray& c) {
            if (state_ == State::Recording && wav_.isOpen()) {
                wav_.write(c);
                dataBytes_ += c.size();
            }
        });
        connect(cap_, &AudioCapture::errorText, this, [this](const QString& e) {
            if (state_ != State::Recording) return;
            stop();
            emit failed(e);
        });
    }
    wav_.setFileName(wavPath);
    if (!wav_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(QStringLiteral("cannot write %1").arg(wavPath));
        return false;
    }
    wav_.write(wavHeader(48000, 1, 16));
    dataBytes_ = 0;
    if (!cap_->start(targetNode, 48000)) {
        wav_.close();
        wav_.remove();
        return false;                    // errorText already reported why
    }
    state_ = State::Recording;
    return true;
}

bool ClipDeck::play(const QString& wavPath, const QString& targetNode) {
    if (state_ != State::Idle) return false;
    playPcm_.clear();
    playPos_ = 0;
    playRate_ = 48000;
    if (!loadWavS16Mono(wavPath, playPcm_, playRate_) || playPcm_.isEmpty()) {
        emit failed(QStringLiteral("not a 16-bit PCM WAV: %1").arg(wavPath));
        return false;
    }
    if (!out_) out_ = new AudioPlayback(this);
    if (!out_->start(targetNode, playRate_)) return false;
    if (!pump_) {
        pump_ = new QTimer(this);
        pump_->setInterval(50);
        connect(pump_, &QTimer::timeout, this, [this] {
            if (state_ != State::Playing || !out_) return;
            if (playPos_ < playPcm_.size()) {
                const qint64 room = out_->bytesFree();
                if (room > 0) {
                    const qsizetype n =
                        std::min<qsizetype>(room, playPcm_.size() - playPos_);
                    out_->write(playPcm_.mid(playPos_, n));
                    playPos_ += n;
                }
                return;
            }
            if (out_->drained()) {       // whole file through the device
                pump_->stop();
                out_->stop();
                state_ = State::Idle;
                emit finished();
            }
        });
    }
    state_ = State::Playing;
    pump_->start();
    return true;
}

void ClipDeck::stop() {
    if (state_ == State::Idle) return;
    stopping_ = true;
    if (state_ == State::Recording) {
        if (cap_) cap_->stop();
        if (wav_.isOpen()) {             // patch RIFF/data sizes (the
            const auto patch32 = [&](qint64 off, quint32 v) {   // "finalize
                wav_.seek(off);                                  // on stop"
                char b[4] = {char(v), char(v >> 8), char(v >> 16),
                             char(v >> 24)};
                wav_.write(b, 4);
            };
            patch32(4, quint32(36 + dataBytes_));
            patch32(40, quint32(dataBytes_));
            wav_.close();
        }
    } else {                             // Playing
        if (pump_) pump_->stop();
        if (out_) out_->stop();
        playPcm_.clear();
    }
    state_ = State::Idle;
    emit finished();
}

#else // Linux ------------------------------------------------------------

// Pulse-layer tools (parecord/paplay -> pipewire-pulse), not the native
// pw-cat pair: live-found that the native client path can wedge silently
// after a pipewire restart (stream RUNNING, sink monitor carrying audio,
// speakers silent) while the pulse layer — the road every desktop app
// takes — keeps working. s16le forced so normalizeWav's 16-bit PCM
// expectation holds; parecord finalizes a valid WAV on SIGINT (verified).
bool ClipDeck::record(const QString& wavPath, const QString& targetNode) {
    if (state_ != State::Idle) return false;
    QStringList args{"--format=s16le", "--rate=48000", "--channels=1"};
    if (!targetNode.isEmpty()) args << "-d" << targetNode;
    args << wavPath;
    return launch("parecord", args, State::Recording);
}

bool ClipDeck::play(const QString& wavPath, const QString& targetNode) {
    if (state_ != State::Idle) return false;
    QStringList args;
    if (!targetNode.isEmpty()) args << "-d" << targetNode;
    args << wavPath;
    return launch("paplay", args, State::Playing);
}

void ClipDeck::stop() {
    if (state_ == State::Idle) return;
    stopping_ = true;
    if (proc_.processId() > 0)
        ::kill(static_cast<pid_t>(proc_.processId()), SIGINT);
}

#endif // TTC_CLIPDECK_QTMM

bool ClipDeck::launch(const QString& exe, const QStringList& args, State s) {
    stopping_ = false;
    proc_.start(exe, args);
    if (!proc_.waitForStarted(2000)) return false; // errorOccurred already fired
    state_ = s;
    return true;
}

#ifdef TTC_CLIPDECK_QTMM

// Windows: "node" is a device description from Qt Multimedia. Same
// contracts — substring match, and the excluding variant prefers a USB
// device so a voice-keyer take records the operator, not the radio codec.
QString ClipDeck::findSink(const QString& match) {
    for (const QString& d : AudioPlayback::outputDescriptions())
        if (d.contains(match, Qt::CaseInsensitive)) return d;
    return {};
}

QString ClipDeck::findSource(const QString& match) {
    for (const QString& d : AudioCapture::inputDescriptions())
        if (d.contains(match, Qt::CaseInsensitive)) return d;
    return {};
}

QString ClipDeck::findSourceExcluding(const QString& avoid) {
    QStringList candidates;
    for (const QString& d : AudioCapture::inputDescriptions()) {
        if (!avoid.isEmpty() && d.contains(avoid, Qt::CaseInsensitive))
            continue;
        candidates << d;
    }
    for (const QString& c : candidates)
        if (c.contains("microphone", Qt::CaseInsensitive)
            || c.contains("usb", Qt::CaseInsensitive))
            return c;
    return candidates.isEmpty() ? QString() : candidates.first();
}

#else // Linux ------------------------------------------------------------

static QString findNode(const char* kind, const QString& match) {
    QProcess p;
    p.start("pactl", {"list", "short", kind});
    if (!p.waitForFinished(3000)) return {};
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    for (const QString& line : out.split('\n')) {
        const QStringList f = line.split('\t');
        if (f.size() >= 2 && f[1].contains(match, Qt::CaseInsensitive)
            && !f[1].endsWith(".monitor"))
            return f[1];
    }
    return {};
}

QString ClipDeck::findSink(const QString& match)   { return findNode("sinks", match); }
QString ClipDeck::findSource(const QString& match) { return findNode("sources", match); }

QString ClipDeck::findSourceExcluding(const QString& avoid) {
    QProcess p;
    p.start("pactl", {"list", "short", "sources"});
    if (!p.waitForFinished(3000)) return {};
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    QStringList candidates;
    for (const QString& line : out.split('\n')) {
        const QStringList f = line.split('\t');
        if (f.size() < 2 || f[1].endsWith(".monitor")) continue;
        if (!avoid.isEmpty() && f[1].contains(avoid, Qt::CaseInsensitive)) continue;
        candidates << f[1];
    }
    for (const QString& c : candidates)            // a USB mic beats a probably-
        if (c.contains("usb", Qt::CaseInsensitive)) return c;   // empty line-in
    return candidates.isEmpty() ? QString() : candidates.first();
}

#endif // TTC_CLIPDECK_QTMM

// Minimal RIFF walker: find the fmt/data chunks of the PCM WAVs pw-record
// writes, scale the samples, rewrite the file.
bool ClipDeck::normalizeWav(const QString& wavPath, double targetPeak,
                            double maxGain) {
    QFile file(wavPath);
    if (!file.open(QIODevice::ReadWrite)) return false;
    QByteArray b = file.readAll();
    if (b.size() < 44 || !b.startsWith("RIFF") || b.mid(8, 4) != "WAVE")
        return false;
    const auto u16 = [&](qsizetype o) {
        return quint16(quint8(b[o])) | quint16(quint8(b[o + 1])) << 8;
    };
    const auto u32 = [&](qsizetype o) {
        return quint32(u16(o)) | quint32(u16(o + 2)) << 16;
    };
    qsizetype dataOff = -1, dataLen = 0;
    bool pcm16 = false;
    for (qsizetype o = 12; o + 8 <= b.size();) {
        const QByteArray id = b.mid(o, 4);
        const qsizetype len = u32(o + 4);
        if (id == "fmt " && len >= 16)
            pcm16 = u16(o + 8) == 1 && u16(o + 22) == 16;  // PCM, 16-bit
        if (id == "data") {
            dataOff = o + 8;
            dataLen = std::min<qsizetype>(len, b.size() - dataOff);
            break;
        }
        o += 8 + len + (len & 1);                  // chunks are word-aligned
    }
    if (!pcm16 || dataOff < 0 || dataLen < 2) return false;
    auto* s = reinterpret_cast<qint16*>(b.data() + dataOff);
    const qsizetype n = dataLen / 2;
    int peak = 0;
    for (qsizetype i = 0; i < n; ++i) peak = std::max(peak, std::abs(int(s[i])));
    if (peak == 0) return false;                   // dead silence: leave it
    const double gain = std::min(targetPeak * 32767.0 / peak, maxGain);
    if (gain <= 1.0) return true;                  // already hot enough
    for (qsizetype i = 0; i < n; ++i)
        s[i] = qint16(std::clamp(int(std::lround(s[i] * gain)), -32768, 32767));
    file.seek(0);
    file.write(b);
    return true;
}

} // namespace ttc
