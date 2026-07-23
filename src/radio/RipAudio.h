// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QProcess>

class QUdpSocket;
class QTimer;

namespace ttc {

// RIP — the Omni VII's receive audio over Ethernet (One Plug REMOTE mode).
// The radio streams its speaker audio as UDP datagrams on CMD port +2:
// 12-byte header (variant 0x00 carries the live S-meter in octets 8-11,
// variant 0x80 plain RTP) + 128 signed 8-bit linear samples at ~7013 Hz
// (both live-ruled: tools/omni7_ruler.py rip, WWV tone test for the
// encoding). Firmware 1.036 REQUIRES the 8-bit compression request —
// *T <0x01> <0x02> — plain 16-bit is silently refused (One Plug addendum
// A, note 2, and live-found). The enable is sent TO the audio port so the
// radio learns the return address, and it times out after 5 s, so it is
// re-sent every 2 s. The RIP level follows the radio's AF volume.
//
// SAFETY: the enable rides the *T Transmit command — d1 bit 2 keys the
// TRANSMITTER. This class only ever sends d1 = 0x01 (RIP on) or 0x00
// (off); bit 2 must never be set here.
//
// Playback pipes s16 PCM into pw-play (the ClipDeck pattern — PipeWire CLI,
// no audio library dependency). Under TTC_SELFTEST no player is spawned;
// packets are counted and reported on stop so the harness can assert flow.
class RipAudio : public QObject {
    Q_OBJECT
public:
    explicit RipAudio(QObject* parent = nullptr);
    ~RipAudio() override;

    // host = radio IPv4 (host byte order), cmdPort = UDP CMD port; the
    // audio stream lives on cmdPort+2. Returns false if the bind fails.
    bool start(quint32 host, quint16 cmdPort, quint16 passcode);
    void stop();
    bool active() const { return sock_ != nullptr; }

private:
    void sendEnable(bool on);
    void onDatagram();

    QUdpSocket* sock_ = nullptr;
    QTimer* keepalive_ = nullptr;
    QProcess* player_ = nullptr;
    quint32 host_ = 0;
    quint16 audioPort_ = 0;
    quint16 passcode_ = 0;
    quint64 pkts_ = 0;
    bool selftest_ = false;
};

} // namespace ttc
