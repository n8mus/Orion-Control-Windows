// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>

class QUdpSocket;
class QTimer;

namespace ttc {

class AudioCapture;

// TRIP — Transmit audio over IP: the mirror of RIP, computer -> Omni VII.
// While the rig is keyed in REMOTE mode the radio takes its TRANSMIT audio
// from this stream instead of the mic, so remote SSB / digital / voice-keyer
// audio goes out over the one Ethernet cable. Packets go to CMD port +4
// (49156): byte 0 is an incrementing counter, bytes 1-128 are 128 signed
// 8-bit samples at ~7013 Hz (8-bit "compressed" TRIP, what fw 1036 uses —
// the mirror of the RIP encoding). Capture rides pw-record (the ClipDeck /
// RipAudio pattern: PipeWire CLI, no audio library), which paces the stream
// at the real-time sample rate, so no timer is needed.
//
// SAFETY: this class does NOT key the transmitter. Keying is the driver's
// *T command (setPtt); TripAudio only streams audio *while* the driver has
// keyed. It is created only when radio/tripAudio is explicitly enabled
// (default OFF), and only streams between start()/stop(), which the driver
// calls on PTT down/up. radio/tripSource picks the capture node (empty =
// system default input).
class TripAudio : public QObject {
    Q_OBJECT
public:
    explicit TripAudio(QObject* parent = nullptr);
    ~TripAudio() override;

    // host = radio IPv4 (host byte order), cmdPort = UDP CMD port; TX audio
    // streams to cmdPort+4. source = pw-record capture node (empty = default).
    bool start(quint32 host, quint16 cmdPort, const QString& source);
    void stop();
    bool active() const { return sock_ != nullptr; }

    // Build one TRIP datagram from little-endian s16 mono PCM (>=256 bytes;
    // exactly 128 samples are consumed). counter is advanced (wraps at 256).
    // Pure + static so the packet format is unit-testable without a radio.
    static QByteArray packetize(quint8& counter, const char* s16le);

private:
    void onCapture();
    void drain();                        // paced packet emission

    QUdpSocket* sock_ = nullptr;
    QProcess* rec_ = nullptr;
    AudioCapture* cap_ = nullptr;        // Windows capture endpoint
    QTimer* pacer_ = nullptr;            // clocks packets out at the audio rate
    QElapsedTimer clock_;                // real-time reference for pacing
    QByteArray acc_;                     // s16 capture accumulator
    quint32 host_ = 0;
    quint16 audioPort_ = 0;
    quint8 counter_ = 0;
    quint64 paced_ = 0;                  // packets emitted since start (pacing)
    quint64 pkts_ = 0;
    bool streaming_ = false;             // false = still filling the jitter buffer
    bool selftest_ = false;
};

} // namespace ttc
