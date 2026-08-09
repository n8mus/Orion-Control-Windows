// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/PmMeter.h"

#include "radio/SerialPort.h"

#include <QDateTime>
#include <QSignalBlocker>
#include <QTimer>

namespace ttc {

namespace {
constexpr char kStx = '\x02';
constexpr char kEtx = '\x03';
// The documented activation command (community capture, verified live on
// this station's meter): STX "D1" ETX "C0" CR "S" NUL.
const char kActivate[] = {'\x02', 'D', '1', '\x03', 'C', '0', '\r', 'S', '\0'};

// Below this the meter has no carrier and its SWR field free-runs (0.00
// at idle on the live unit).
constexpr double kValidFloorW = 0.5;
} // namespace

PmMeter::PmMeter(QObject* parent) : TxMeter(parent) {}

// Same rule as LpMeter: stop() ends in setAlive(false), and emitting that
// from a destructor reaches consumers that may themselves be mid-teardown.
PmMeter::~PmMeter() {
    QSignalBlocker silenceOurSignals(this);
    stop();
}

quint8 PmMeter::crc8(const QByteArray& payload) {
    // CRC-8 poly 0xB1, init 0x00, unreflected, final XOR 0xFF — the unique
    // solution over three known frames (see header). Payload only: the
    // STX/ETX framing bytes are not covered.
    quint8 c = 0x00;
    for (const char b : payload) {
        c ^= quint8(b);
        for (int i = 0; i < 8; ++i)
            c = (c & 0x80) ? quint8((c << 1) ^ 0xB1) : quint8(c << 1);
    }
    return c ^ 0xFF;
}

bool PmMeter::parsePayload(const QByteArray& payload, Reading& out) {
    if (payload.isEmpty()) return false;
    if (payload[0] == 'A') return false;       // ACK: alive, but no data
    if (payload[0] != 'D') return false;
    const QList<QByteArray> f = payload.split(',');
    if (f.size() < 4) return false;
    bool okF = false, okR = false, okS = false;
    const double fwd = f[1].trimmed().toDouble(&okF);
    const double rev = f[2].trimmed().toDouble(&okR);
    const double swr = f[3].trimmed().toDouble(&okS);
    if (!okF || !okR || !okS) return false;
    out = Reading{};
    out.watts    = fwd;
    out.refWatts = rev;
    out.swr      = swr;
    out.valid    = fwd >= kValidFloorW && swr >= 1.0;
    out.zValid   = false;                      // scalar meter, always
    out.tsMs     = QDateTime::currentMSecsSinceEpoch();
    return true;
}

bool PmMeter::start(const QString& device, int /*pollMs*/) {
    stop();
    port_ = new SerialPort(this);
    port_->setRawMode(true);                   // frames carry their own STX/ETX
    connect(port_, &SerialPort::bytesReceived, this, &PmMeter::onBytes);
    connect(port_, &SerialPort::ioError, this, [this](const QString& e) {
        setAlive(false);
        emit error(e);
    });
    if (!port_->open(device.toStdString(), 38400, /*hwHandshake=*/false)) {
        port_->deleteLater();
        port_ = nullptr;
        return false;
    }
    sendActivation();
    // Watchdog doubles as the re-activation nag: the meter streams only
    // after the command, so a power-cycled meter goes quiet until we ask
    // again. 1 s cadence keeps reconnects prompt without chatter.
    dog_ = new QTimer(this);
    dog_->setInterval(1000);
    connect(dog_, &QTimer::timeout, this, &PmMeter::watchdog);
    dog_->start();
    return true;
}

void PmMeter::stop() {
    if (dog_)  { dog_->stop(); dog_->deleteLater(); dog_ = nullptr; }
    if (port_) { port_->close(); port_->deleteLater(); port_ = nullptr; }
    rx_.clear();
    setAlive(false);
}

bool PmMeter::isOpen() const { return port_ && port_->isOpen(); }

void PmMeter::sendActivation() {
    if (!port_) return;
    // CR first: the meter's command parser accumulates whatever bytes ever
    // hit the port (wrong-baud probes, another meter's poll string) and
    // answers "(STX)!No STX(ETX)" to a perfectly good command while
    // desynced — seen live. A newline flushes its line buffer so the STX
    // that follows lands clean.
    port_->write(QByteArrayLiteral("\r\n")
                 + QByteArray(kActivate, int(sizeof(kActivate))));
    lastActMs_ = QDateTime::currentMSecsSinceEpoch();
}

void PmMeter::watchdog() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastGoodMs_ > 3000) {
        setAlive(false);
        if (now - lastActMs_ >= 1000) sendActivation();
    }
}

void PmMeter::onBytes(const QByteArray& chunk) {
    rx_.append(chunk);
    for (;;) {
        const int s = rx_.indexOf(kStx);
        if (s < 0) {
            if (rx_.size() > 512) rx_.clear();
            return;
        }
        if (s > 0) rx_.remove(0, s);
        const int e = rx_.indexOf(kEtx);
        if (e < 0) {
            if (rx_.size() > 512) rx_.clear();   // desync guard
            return;
        }
        if (rx_.size() < e + 3) return;          // checksum not in yet
        const QByteArray payload = rx_.mid(1, e - 1);
        const QByteArray ckHex   = rx_.mid(e + 1, 2);
        rx_.remove(0, e + 3);
        bool okCk = false;
        const quint8 want = quint8(ckHex.toUInt(&okCk, 16));
        if (!okCk || crc8(payload) != want) continue;   // corrupt: resync
        if (!payload.isEmpty() && payload[0] == 'A') {
            // Activation ACK — the stream is coming.
            lastGoodMs_ = QDateTime::currentMSecsSinceEpoch();
            setAlive(true);
            continue;
        }
        Reading r;
        if (!parsePayload(payload, r)) continue;
        last_       = r;
        lastGoodMs_ = r.tsMs;
        setAlive(true);
        emit reading(last_);
    }
}

void PmMeter::setAlive(bool a) {
    if (alive_ == a) return;
    alive_ = a;
    emit aliveChanged(alive_);
}

} // namespace ttc
