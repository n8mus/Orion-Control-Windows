// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/ClipDeck.h"

#include <QFile>
#include <algorithm>
#include <cmath>
#include <csignal>

namespace ttc {

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

bool ClipDeck::launch(const QString& exe, const QStringList& args, State s) {
    stopping_ = false;
    proc_.start(exe, args);
    if (!proc_.waitForStarted(2000)) return false; // errorOccurred already fired
    state_ = s;
    return true;
}

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
