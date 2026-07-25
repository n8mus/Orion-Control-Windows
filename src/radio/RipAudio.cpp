// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/RipAudio.h"

#include <QHostAddress>
#include <QSettings>
#include <QTimer>
#include <QUdpSocket>
#include <cstdio>
#include <cstdlib>

namespace ttc {

RipAudio::RipAudio(QObject* parent) : QObject(parent) {
    selftest_ = qEnvironmentVariableIsSet("TTC_SELFTEST");
}

RipAudio::~RipAudio() { stop(); }

bool RipAudio::start(quint32 host, quint16 cmdPort, quint16 passcode) {
    if (sock_) return true;
    host_ = host;
    audioPort_ = quint16(cmdPort + 2);
    passcode_ = passcode;
    sock_ = new QUdpSocket(this);
    // Symmetric ports, same rule as the command channel: the radio streams
    // TO the audio port, so we must own it locally.
    if (!sock_->bind(QHostAddress::AnyIPv4, audioPort_)) {
        delete sock_;
        sock_ = nullptr;
        return false;
    }
    connect(sock_, &QUdpSocket::readyRead, this, &RipAudio::onDatagram);
    sendEnable(true);
    keepalive_ = new QTimer(this);
    keepalive_->setInterval(2000);      // radio drops RIP after 5 s of silence
    connect(keepalive_, &QTimer::timeout, this, [this] { sendEnable(true); });
    keepalive_->start();
    return true;
}

void RipAudio::stop() {
    if (!sock_) return;
    if (keepalive_) {
        keepalive_->stop();
        keepalive_->deleteLater();
        keepalive_ = nullptr;
    }
    sendEnable(false);
    sock_->deleteLater();
    sock_ = nullptr;
    if (player_) {
        player_->closeWriteChannel();
        player_->terminate();
        player_->waitForFinished(500);
        player_->deleteLater();
        player_ = nullptr;
    }
    if (selftest_)
        fprintf(stderr, "[rip] %llu audio pkts received\n",
                static_cast<unsigned long long>(pkts_));
}

void RipAudio::setVolume(int pct) {
    volPct_ = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void RipAudio::pause() {
    if (!sock_) return;
    if (keepalive_) keepalive_->stop();       // stop resending RIP-on *T
    if (player_) {                            // let the player go idle/close
        player_->closeWriteChannel();
        player_->terminate();
        player_->waitForFinished(300);
        player_->deleteLater();
        player_ = nullptr;
    }
}

void RipAudio::resume() {
    if (!sock_ || !keepalive_) return;
    sendEnable(true);                         // re-arm RIP immediately
    keepalive_->start();
}

void RipAudio::sendEnable(bool on) {
    if (!sock_) return;
    QByteArray d;
    d.append(char(passcode_ >> 8));
    d.append(char(passcode_ & 0xff));
    // *T <d1> <d0>: d1 bit0 = RIP, d0 bit1 = 8-bit RIP compression
    // (REQUIRED on fw 1036). d1 bit2 would key the TRANSMITTER — never
    // set from this class.
    d.append("*T", 2);
    d.append(on ? char(0x01) : char(0x00));
    d.append(on ? char(0x02) : char(0x00));
    d.append('\r');
    sock_->writeDatagram(d, QHostAddress(host_), audioPort_);
}

void RipAudio::onDatagram() {
    while (sock_ && sock_->hasPendingDatagrams()) {
        QByteArray d(int(sock_->pendingDatagramSize()), 0);
        sock_->readDatagram(d.data(), d.size());
        if (d.size() <= 12) continue;               // header-only / stray
        ++pkts_;
        if (selftest_) continue;                    // count only, no audio out
        if (!player_) {
            // Playback rides the PULSE layer (pacat -> pipewire-pulse), the
            // same path every desktop app uses — live-found: after a
            // pipewire restart the NATIVE client path (pw-play) can wedge
            // silently (stream shows RUNNING, sink monitor carries audio,
            // speakers get nothing) while the pulse layer keeps working.
            // Raw headerless s16 mono at the ruled ~7013 Hz; pipewire
            // resamples. radio/ripSink pins the output (sink name; empty =
            // default). Falls back to pw-play if pacat is missing.
            const QString sink =
                QSettings().value("radio/ripSink").toString().trimmed();
            QStringList args{"--playback", "--raw", "--format=s16le",
                             "--rate=7013", "--channels=1"};
            if (!sink.isEmpty()) args << "--device=" + sink;
            player_ = new QProcess(this);
            player_->start("pacat", args);
            if (!player_->waitForStarted(1500)) {
                player_->deleteLater();
                player_ = new QProcess(this);
                QStringList pw{"--raw", "--format=s16", "--rate=7013",
                               "--channels=1"};
                if (!sink.isEmpty()) pw << "--target" << sink;
                pw << "-";
                player_->start("pw-play", pw);
                if (!player_->waitForStarted(1500)) {
                    player_->deleteLater();
                    player_ = nullptr;
                    return;                          // no audio stack: count only
                }
            }
        }
        // Payload: signed 8-bit linear (top byte of s16, WWV-tone-verified)
        // — widen to s16le with the computer-side gain applied in-process
        // (volPct_ 100 = unity, exactly the old high-byte copy). 0 = muted:
        // skip the write, pw-play just idles until samples resume.
        if (volPct_ == 0) continue;
        const int n = d.size() - 12;
        QByteArray pcm(n * 2, 0);
        const char* src = d.constData() + 12;
        char* dst = pcm.data();
        for (int i = 0; i < n; ++i) {
            const int v = int(qint8(src[i])) * 256 * volPct_ / 100;
            dst[2 * i] = char(v & 0xff);
            dst[2 * i + 1] = char((v >> 8) & 0xff);
        }
        player_->write(pcm);
    }
}

} // namespace ttc
