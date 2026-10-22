// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/DcuRotor.h"

#include "radio/SerialPort.h"

#include <QDateTime>
#include <QTimer>

namespace ttc {

// The controller answers a query in ~17 ms and streams on its own while
// turning, so this is only the "it has gone quiet, is it still there?"
// prod — fast enough that a rose needle started by someone else's turn
// (cqrlog's SP/LP buttons through our :4533) picks up within a second.
static constexpr int  kPollMs   = 800;
// Two silent prods in a row means nothing is listening on the wire.
static constexpr qint64 kDeadMs = 2500;
// A wedged or mis-wired port (a radio's CAT stream, say) must not grow
// the buffer without bound; frames are 4 bytes, so this is generous.
static constexpr int  kMaxBuf   = 512;
// Gap between the "set target" and "execute" writes of a turn — the
// controller's parser drops the execute without it (hamlib's
// post_write_delay is the same 500 ms).
static constexpr int  kExecDelayMs = 500;

DcuRotor::DcuRotor(QObject* parent) : QObject(parent) {
    port_ = new SerialPort(this);
    port_->setRawMode(true);           // "ddd;" frames, no CR anywhere
    connect(port_, &SerialPort::bytesReceived, this, &DcuRotor::onBytes);
    timer_ = new QTimer(this);
    timer_->setInterval(kPollMs);
    connect(timer_, &QTimer::timeout, this, &DcuRotor::poll);
}

DcuRotor::~DcuRotor() = default;

void DcuRotor::setDevice(const QString& dev) {
    if (dev == dev_) return;
    dev_ = dev;
    if (active_) { setActive(false); setActive(true); }
}

void DcuRotor::setActive(bool on) {
    if (on == active_) return;
    active_ = on;
    if (on) {
        buf_.clear();
        // An unconfigured port is not an error to shout about — the
        // station simply has no rotor yet.
        if (dev_.isEmpty()) { active_ = false; setConnected(false); return; }
        if (!port_->open(dev_.toStdString(), 4800, false)) {
            active_ = false;             // ioError already told the user
            setConnected(false);
            return;
        }
        lastFrameMs_ = QDateTime::currentMSecsSinceEpoch();
        timer_->start();
        poll();                          // ask once immediately
    } else {
        timer_->stop();
        port_->close();
        az_ = -1.0;
        target_ = -1.0;
        setConnected(false);
        emit azimuthChanged(-1.0);
    }
}

bool DcuRotor::holding() const { return port_->isOpen(); }

void DcuRotor::poll() {
    if (!active_ || !port_->isOpen()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (connected_ && now - lastFrameMs_ > kDeadMs) {
        az_ = -1.0;
        setConnected(false);
        emit azimuthChanged(-1.0);
    }
    port_->write(QByteArrayLiteral("AI1;"));
}

bool DcuRotor::turnTo(double azDeg) {
    while (azDeg < 0.0) azDeg += 360.0;
    while (azDeg >= 360.0) azDeg -= 360.0;
    target_ = azDeg;
    if (!port_->isOpen()) return false;
    // Target, GAP, then execute. The DCU-3's command parser drops the
    // execute if it arrives hard on the heels of the target — hamlib
    // leaves post_write_delay (500 ms) between the two writes and only a
    // gapped pair ever moved the rotor on the bench (2026-08-09). The
    // controller picks the direction itself and takes the long way round
    // rather than cross its mechanical stop (134 -> 200 went anticlockwise
    // through north).
    port_->write(QByteArrayLiteral("AP1")
                 + QByteArray::number(qRound(azDeg)).rightJustified(3, '0')
                 + ';');
    QTimer::singleShot(kExecDelayMs, this, [this] {
        if (port_->isOpen()) port_->write(QByteArrayLiteral("AM1;"));
    });
    return true;
}

bool DcuRotor::stop() {
    target_ = -1.0;
    if (!port_->isOpen()) return false;
    port_->write(QByteArrayLiteral(";"));
    return true;
}

void DcuRotor::onBytes(const QByteArray& chunk) {
    buf_ += chunk;
    if (buf_.size() > kMaxBuf) buf_ = buf_.right(kMaxBuf);
    const double az = parseFrames(buf_);
    if (az < 0.0) return;
    lastFrameMs_ = QDateTime::currentMSecsSinceEpoch();
    setConnected(true);
    if (az == az_) return;
    az_ = az;
    emit azimuthChanged(az_);
}

double DcuRotor::parseFrames(QByteArray& buf) {
    double last = -1.0;
    int keepFrom = 0;                    // start of the unconsumed tail
    for (int i = 0; i + 3 < buf.size(); ++i) {
        // A frame is three digits followed by ';'. Anything else is
        // stream damage and is skipped a byte at a time.
        if (buf[i + 3] != ';') continue;
        const char a = buf[i], b = buf[i + 1], c = buf[i + 2];
        if (a < '0' || a > '9' || b < '0' || b > '9' || c < '0' || c > '9')
            continue;
        const int deg = (a - '0') * 100 + (b - '0') * 10 + (c - '0');
        if (deg > 360) continue;
        last = (deg == 360) ? 0.0 : double(deg);
        i += 3;                          // consume the frame
        keepFrom = i + 1;
    }
    if (last >= 0.0) {
        buf.remove(0, keepFrom);
    } else if (buf.size() > 3) {
        // No frame in sight: keep only what could still be the head of
        // one, so damage cannot accumulate.
        buf = buf.right(3);
    }
    return last;
}

void DcuRotor::setConnected(bool on) {
    if (on == connected_) return;
    connected_ = on;
    emit connectedChanged(on);
}

} // namespace ttc
