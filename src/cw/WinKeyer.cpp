// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/WinKeyer.h"

#include <QSerialPort>
#include <QThread>
#include <QTimer>
#include <algorithm>

namespace ttc {

namespace {
// WK2 status byte (0xC0 | bits), K1EL datasheet values.
constexpr quint8 kStBreakIn = 0x02;
constexpr quint8 kStBusy    = 0x04;
} // namespace

WinKeyer::WinKeyer(QObject* parent) : QObject(parent) {
    ser_ = new QSerialPort(this);
    connect(ser_, &QSerialPort::readyRead, this, &WinKeyer::onReadyRead);
    potTimer_ = new QTimer(this);
    potTimer_->setSingleShot(true);
    potTimer_->setInterval(250);
    connect(potTimer_, &QTimer::timeout, this, [this] {
        if (potPending_ >= 0 && potPending_ != potEmitted_) {
            potEmitted_ = potPending_;
            emit potChanged(potEmitted_);
        }
    });
}

WinKeyer::~WinKeyer() { close(); }

bool WinKeyer::open(const QString& device) {
    if (open_) close();
    ser_->setPortName(device);
    ser_->setBaudRate(1200);
    ser_->setDataBits(QSerialPort::Data8);
    ser_->setParity(QSerialPort::NoParity);
    ser_->setStopBits(QSerialPort::TwoStop);
    ser_->setFlowControl(QSerialPort::NoFlowControl);
    if (!ser_->open(QIODevice::ReadWrite)) {
        err_ = ser_->errorString();
        emit errorOccurred("WinKeyer: " + err_);
        return false;
    }
    ser_->setDataTerminalReady(true);
    ser_->setRequestToSend(false);
    // Handshake reads are blocking; keep the readyRead slot from eating
    // the reply bytes mid-wait.
    const QSignalBlocker noAsync(ser_);
    // The WKUSB needs a beat after DTR asserts before it listens — without
    // this the echo test times out (found the hard way; 800 ms is safe).
    QThread::msleep(800);
    ser_->clear();
    // Wake + sanity: three nulls, then Admin:Echo of one byte, then
    // Admin:Host Open (returns the firmware revision).
    const char nulls[] = {0x13, 0x13, 0x13};
    ser_->write(nulls, 3);
    ser_->flush();
    QThread::msleep(300);
    ser_->readAll();
    const char echo[] = {0x00, 0x04, 0x14};
    ser_->write(echo, 3);
    ser_->flush();
    QByteArray got;
    for (int i = 0; i < 10 && !got.contains(char(0x14)); ++i)
        if (ser_->waitForReadyRead(300)) got += ser_->readAll();
    if (!got.contains(char(0x14))) {
        err_ = "no echo response — is this the WinKeyer port?";
        emit errorOccurred("WinKeyer: " + err_);
        ser_->close();
        return false;
    }
    const char hostOpen[] = {0x00, 0x02};
    ser_->write(hostOpen, 2);
    ser_->flush();
    ser_->waitForReadyRead(600);
    ser_->readAll();                       // firmware revision byte
    open_ = true;
    setSpeed(wpm_);
    return true;
}

void WinKeyer::close() {
    if (!ser_->isOpen()) return;
    if (open_) {
        stop();
        const char hostClose[] = {0x00, 0x03};
        ser_->write(hostClose, 2);
        ser_->flush();
    }
    ser_->close();
    open_ = false;
    if (busy_) { busy_ = false; emit busyChanged(false); }
}

void WinKeyer::setSpeed(int wpm) {
    wpm_ = std::clamp(wpm, 5, 99);
    if (!open_) return;
    const char cmd[] = {0x02, char(wpm_)};
    ser_->write(cmd, 2);
}

void WinKeyer::setPotRange(int minWpm, int maxWpm) {
    potMin_ = std::clamp(minWpm, 5, 60);
    if (!open_) return;
    const int range = std::clamp(maxWpm - potMin_, 1, 63);
    const char cmd[] = {0x05, char(potMin_), char(range), 0x00};
    ser_->write(cmd, 4);
    const char getPot[] = {0x07};          // prime with the knob's position
    ser_->write(getPot, 1);
}

void WinKeyer::send(const QString& text) {
    if (!open_) return;
    QByteArray out;
    for (QChar qc : text.toUpper()) {
        const char c = qc.toLatin1();
        // WK sends A-Z 0-9 and common prosign-able punctuation; pass space
        // and the characters the keyer's table knows, drop the rest.
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '
            || c == '?' || c == '/' || c == '.' || c == ',' || c == '='
            || c == '+' || c == '-' || c == ';' || c == ':' || c == '\''
            || c == '(' || c == ')' || c == '"' || c == '@' || c == '!')
            out.append(c);
    }
    if (out.isEmpty()) return;
    ser_->write(out);
    if (!busy_) { busy_ = true; emit busyChanged(true); }
}

void WinKeyer::stop() {
    if (!open_) return;
    const char cmd[] = {0x0A};
    ser_->write(cmd, 1);
    ser_->flush();
}

void WinKeyer::tune(bool on) {
    if (!open_) return;
    const char cmd[] = {0x0B, char(on ? 1 : 0)};
    ser_->write(cmd, 2);
}

void WinKeyer::backspace() {
    if (!open_) return;
    const char cmd[] = {0x08};
    ser_->write(cmd, 1);
}

void WinKeyer::onReadyRead() {
    const QByteArray data = ser_->readAll();
    for (unsigned char b : data) {
        if ((b & 0xC0) == 0xC0) {          // status byte
            const bool busy = b & kStBusy;
            if (b & kStBreakIn) emit breakIn();
            if (busy != busy_) { busy_ = busy; emit busyChanged(busy); }
        } else if ((b & 0xC0) == 0x80) {   // speed pot: offset from pot min
            // Coalesce with a short settle timer. The keyer reports each
            // NEW value exactly once, so the old "same value twice in a
            // row" debounce could never fire on a normal turn — the pot
            // was completely dead (live-found). The timer keeps the
            // original goal (a pot flapping ±1 between detents doesn't
            // chatter the speed) while a real turn lands ~250 ms after
            // the knob stops.
            potPending_ = potMin_ + (b & 0x3F);
            potTimer_->start();
        }
        // anything else would be serial echoback — we never enable it
    }
}

} // namespace ttc
