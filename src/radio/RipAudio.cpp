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
            // ClipDeck pattern: PipeWire's own CLI, no audio library.
            // Rate as ruled from the RTP timestamps (~7013 Hz); PipeWire
            // resamples to the sink. --raw is REQUIRED: the stream is
            // headerless PCM, and without it pw-play tries to sniff a file
            // format, fails, and exits immediately (no audio). radio/ripSink
            // pins the output node (name or id) so it doesn't follow the
            // system default onto an HDMI monitor; empty = default sink.
            QStringList args{"--raw", "--format=s16", "--rate=7013",
                             "--channels=1"};
            const QString sink =
                QSettings().value("radio/ripSink").toString().trimmed();
            if (!sink.isEmpty()) args << "--target" << sink;
            args << "-";
            player_ = new QProcess(this);
            player_->start("pw-play", args);
            if (!player_->waitForStarted(1500)) {
                player_->deleteLater();
                player_ = nullptr;
                return;                              // no PipeWire: count only
            }
        }
        // Payload: signed 8-bit linear (top byte of s16, WWV-tone-verified)
        // — widen to s16le for pw-play.
        const int n = d.size() - 12;
        QByteArray pcm(n * 2, 0);
        const char* src = d.constData() + 12;
        char* dst = pcm.data();
        for (int i = 0; i < n; ++i) {
            dst[2 * i] = 0;                          // low byte
            dst[2 * i + 1] = src[i];                 // high byte = s8 sample
        }
        player_->write(pcm);
    }
}

} // namespace ttc
