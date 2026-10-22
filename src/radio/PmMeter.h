// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "radio/TxMeter.h"

#include <QByteArray>

class QTimer;

namespace ttc {

class SerialPort;

// Array Solutions PowerMaster (original series) RF wattmeter.
//
// Scalar meter: forward, reflected, SWR — no phase, so zValid is always
// false and sweeps taken through it draw SWR curves but never a Smith
// chart. What it has instead is speed (<10 ms peak capture) and a
// hardware alarm relay, which is why it guards the amplifier.
//
// Protocol (bench-verified on the operator's meter 2026-08-02, 38400 8N1,
// straight-through DB9 — NOT a null modem; a null-modem cable reflects
// DTR/RTS back as DSR/CTS/CD and reads as a dead port): unlike the
// LP-100A there is no polling. Send the 9-byte activation
//
//   02 44 31 03 43 30 0d 53 00        (STX "D1" ETX "C0" CR "S" NUL)
//
// and the meter answers one ACK frame, then free-runs data frames:
//
//   (STX)A(ETX)8F\r\n
//   (STX)D,    0.0,    0.0, 0.00,0;0;0;0;0(ETX)06\r\n
//
// Payload is ASCII CSV between STX/ETX: 'D', forward watts, reflected
// watts, SWR, then a ';'-separated flag block. The two hex characters
// after ETX are CRC-8 over the payload only — polynomial 0xB1, init
// 0x00, no reflection, final XOR 0xFF. Cracked by brute force against
// three known frames (two live captures + IW0FFK's published example);
// exactly one algorithm satisfied all three.
//
// At idle the meter reports SWR 0.00 — meaningless without a carrier, so
// Reading::valid gates on forward power like the LP-100A's zValid does.
class PmMeter : public TxMeter {
    Q_OBJECT
public:
    explicit PmMeter(QObject* parent = nullptr);
    ~PmMeter() override;

    // pollMs is ignored: the meter streams at its own rate (~25 Hz seen
    // live). The baud is the meter's front-panel menu setting; 38400 is
    // both the factory recommendation and this station's setting.
    bool start(const QString& device, int pollMs = 100) override;
    void stop() override;
    bool isOpen() const;
    bool isAlive() const override { return alive_; }

    // Exposed for pmtest: the CRC and the payload parser, no hardware.
    static quint8 crc8(const QByteArray& payload);
    static bool parsePayload(const QByteArray& payload, Reading& out);

private:
    void onBytes(const QByteArray& chunk);
    void watchdog();
    void sendActivation();
    void setAlive(bool a);

    SerialPort* port_ = nullptr;
    QTimer*     dog_  = nullptr;
    QByteArray  rx_;
    bool        alive_ = false;
    qint64      lastGoodMs_ = 0;
    qint64      lastActMs_  = 0;
};

} // namespace ttc
