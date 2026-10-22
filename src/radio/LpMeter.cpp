// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/LpMeter.h"

#include "radio/SerialPort.h"

#include <QDateTime>
#include <QSignalBlocker>
#include <QTimer>

#include <cmath>

namespace ttc {

namespace {
// The reply is fixed width: ';' + 7 + 1 + 5 + 1 + 5 + 1 + 1 + 1 + 6 + 1 +
// 1 + 1 + 1 + 1 + 4 + 1 + 4 = 43. There is no terminator, so the length is
// the frame delimiter.
constexpr int kFrameLen = 43;

// Below this, the meter's impedance engine has no carrier to work with and
// the |Z|/phase fields free-run. Chosen well under the console's minimum
// TUNE watts so a legitimate low-power sweep still yields impedance.
constexpr double kZFloorW = 0.5;

// One 'F' press per this long. The meter needs ~200 ms to repaint and
// re-report; pressing sooner double-steps past the target.
constexpr int kPressGapMs = 300;

// Give up seeking after this many presses. A full cycle is 3, so 5 means
// we tried a lap and a half before admitting the meter is not listening.
constexpr int kMaxPresses = 5;
} // namespace

LpMeter::LpMeter(QObject* parent) : TxMeter(parent) {}

// Silence our own signals before tearing down: stop() ends in
// setAlive(false), and a destructor is the one place that transition is not
// news to anyone. Emitted from here it lands in MainWindow's aliveChanged
// slot while MainWindow is ITSELF mid-destruction — the meter is one of its
// children, deleted from ~QWidget — and statusBar() on a half-torn-down
// window segfaults. Live crash on every clean exit, reproduced 2026-08-08;
// same shape as the ~RotorClient fix. Runtime stop() still signals normally.
LpMeter::~LpMeter() {
    QSignalBlocker silenceOurSignals(this);
    stop();
}

bool LpMeter::start(const QString& device, int pollMs) {
    stop();
    pollMs_ = std::max(50, std::min(5000, pollMs));

    port_ = new SerialPort(this);
    port_->setRawMode(true);           // no CR framing: frames are ';' + 43
    connect(port_, &SerialPort::bytesReceived, this, &LpMeter::onBytes);
    connect(port_, &SerialPort::ioError, this, [this](const QString& e) {
        setAlive(false);
        emit error(e);
    });
    if (!port_->open(device.toStdString(), 115200, /*hwHandshake=*/false)) {
        port_->deleteLater();
        port_ = nullptr;
        return false;
    }

    tick_ = new QTimer(this);
    tick_->setInterval(pollMs_);
    connect(tick_, &QTimer::timeout, this, &LpMeter::poll);
    tick_->start();
    return true;
}

void LpMeter::stop() {
    if (tick_) { tick_->stop(); tick_->deleteLater(); tick_ = nullptr; }
    if (port_) { port_->close(); port_->deleteLater(); port_ = nullptr; }
    rx_.clear();
    seekTarget_ = Mode::Unknown;
    presses_    = 0;
    setAlive(false);
}

bool LpMeter::isOpen() const { return port_ && port_->isOpen(); }

void LpMeter::poll() {
    if (!port_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Staleness: five polls without a good frame and the meter is gone as
    // far as consumers are concerned (unplugged, powered off, cable pulled
    // mid-sweep). Five, not three, because a mode seek deliberately skips
    // a poll to let 'F' settle — at a 100 ms poll that alone opens a
    // ~200 ms hole, which a three-poll deadline would call a disconnect.
    if (alive_ && now - lastGoodMs_ > std::max(500, pollMs_ * 5))
        setAlive(false);

    // Interleave a mode press before the poll so its effect shows up in the
    // very next frame.
    if (seekTarget_ != Mode::Unknown && now - lastPressMs_ >= kPressGapMs) {
        if (Mode(last_.mode) == seekTarget_) {
            seekTarget_ = Mode::Unknown;         // landed
            presses_    = 0;
        } else if (presses_ >= kMaxPresses) {
            const Mode t = seekTarget_;
            seekTarget_ = Mode::Unknown;
            presses_    = 0;
            emit modeSeekFailed(t);
        } else {
            // Bare 'F', not ";F?" — the '?' form is a query and does not
            // move the mode (tried on the real meter: three ";F?" in a row,
            // field 6 never budged). See the header.
            port_->write(QByteArrayLiteral("F"));
            lastPressMs_ = now;
            presses_++;
            return;                              // let 'F' settle; poll next tick
        }
    }
    port_->write(QByteArrayLiteral(";P?"));      // the vendor's own poll
}

void LpMeter::onBytes(const QByteArray& chunk) {
    rx_.append(chunk);
    for (;;) {
        const int i = rx_.indexOf(';');
        if (i < 0) {                             // no frame start in flight
            if (rx_.size() > 256) rx_.clear();
            return;
        }
        if (i > 0) rx_.remove(0, i);             // drop pre-';' litter
        if (rx_.size() < kFrameLen) {
            if (rx_.size() > 256) rx_.clear();   // desync guard
            return;
        }
        Reading r;
        if (!parseFrame(rx_.left(kFrameLen), r)) {
            // Malformed. Drop only the leading ';' and hunt for the next
            // one: consuming all 43 bytes here would swallow the start of
            // a good frame whenever the ';' we latched onto was litter.
            rx_.remove(0, 1);
            continue;
        }
        rx_.remove(0, kFrameLen);
        last_       = r;
        lastGoodMs_ = r.tsMs;
        setAlive(true);
        emit reading(last_);
    }
}

bool LpMeter::parseFrame(const QByteArray& f, Reading& out) {
    if (f.size() != kFrameLen || f[0] != ';') return false;
    const QList<QByteArray> p = f.mid(1).split(',');
    if (p.size() != 9) return false;

    bool ok = true, all = true;
    auto num = [&](const QByteArray& b) {
        const double v = b.trimmed().toDouble(&ok);
        all = all && ok;
        return v;
    };
    const double watts = num(p[0]);
    const double z     = num(p[1]);
    const double ph    = num(p[2]);
    const double dbm   = num(p[7]);
    const double swr   = num(p[8]);
    if (!all) return false;

    out.watts    = watts;
    out.zOhm     = z;
    out.phaseDeg = ph;
    out.dbm      = dbm;
    out.swr      = swr;
    out.callsign = QString::fromLatin1(p[4]).trimmed();
    out.range    = p[5].trimmed().isEmpty() ? 0 : p[5].trimmed()[0] - '0';

    const char m = p[6].trimmed().isEmpty() ? 'x' : p[6].trimmed()[0];
    out.mode = int((m == '0') ? Mode::Average
                 : (m == '1') ? Mode::PeakHold
                 : (m == '2') ? Mode::Tune
                              : Mode::Unknown);

    // Rectangular form for the R+jX curve. Only meaningful with a carrier;
    // see the class comment on the idle-noise trap.
    out.zValid = watts >= kZFloorW;
    out.valid  = out.zValid;         // same gate: carrier present
    if (out.zValid) {
        const double rad = ph * M_PI / 180.0;
        out.rOhm = z * std::cos(rad);
        out.xOhm = z * std::sin(rad);
    }
    out.tsMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void LpMeter::seekMode(Mode target) {
    seekTarget_  = target;
    presses_     = 0;
    lastPressMs_ = 0;                            // first press on the next tick
}

void LpMeter::setAlive(bool a) {
    if (alive_ == a) return;
    alive_ = a;
    emit aliveChanged(alive_);
}

} // namespace ttc
