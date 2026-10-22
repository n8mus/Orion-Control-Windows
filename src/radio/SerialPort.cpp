// SPDX-License-Identifier: GPL-2.0-or-later
#include "radio/SerialPort.h"

#include <QSerialPort>

namespace ttc {

SerialPort::SerialPort(QObject* parent) : QObject(parent) {}

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string& device, int baud, bool hwHandshake) {
    close();
    // QSerialPort takes a full system path ("/dev/orion", "\\\\.\\COM12") or
    // a bare port name ("COM5") — settings are staged here and applied by
    // open(). Exclusive access and O_CLOEXEC come with it (see header).
    port_ = new QSerialPort(QString::fromStdString(device), this);
    port_->setBaudRate(baud);                          // 4800 rotor .. 115200 LP-100A
    port_->setDataBits(QSerialPort::Data8);
    port_->setParity(QSerialPort::NoParity);
    port_->setStopBits(QSerialPort::OneStop);
    port_->setFlowControl(hwHandshake ? QSerialPort::HardwareControl   // Omni VII: RTS/CTS
                                      : QSerialPort::NoFlowControl);   //   or replies truncate
    if (!port_->open(QIODevice::ReadWrite)) {
        emit ioError(QStringLiteral("open %1: %2")
                         .arg(device.c_str(), port_->errorString()));
        port_->deleteLater();
        port_ = nullptr;
        return false;
    }
    // Assert the modem lines. A POSIX tty open raises DTR+RTS on its own —
    // the behavior every device on this bus was wired against — but Windows
    // leaves them wherever the driver last had them, and the station's
    // RS-232 side goes silent with DTR low (live-found on the Orion via the
    // FT4232H, 2026-08-31: probes answered with DTR up, nothing without).
    // Under HardwareControl RTS belongs to the driver; don't touch it there.
    port_->setDataTerminalReady(true);
    if (!hwHandshake) port_->setRequestToSend(true);
    port_->clear();                                    // both queues, like tcflush
    connect(port_, &QSerialPort::readyRead, this, &SerialPort::onReadable);
    // Surface async faults (device yanked, driver error). NoError is the
    // "cleared" notification; suppress it. close() disconnects before its
    // own teardown, so we never report ourselves.
    connect(port_, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError e) {
                if (e == QSerialPort::NoError || !port_) return;
                emit ioError(QStringLiteral("serial: %1").arg(port_->errorString()));
            });
    return true;
}

void SerialPort::close() {
    if (port_) {
        port_->disconnect(this);      // no error/readyRead from our own teardown
        if (port_->isOpen()) port_->close();
        port_->deleteLater();
        port_ = nullptr;
    }
    rxBuf_.clear();
}

bool SerialPort::isOpen() const { return port_ && port_->isOpen(); }

void SerialPort::write(const QByteArray& data) {
    if (!isOpen()) return;
    if (port_->write(data) < 0)
        emit ioError(QStringLiteral("write: %1").arg(port_->errorString()));
}

void SerialPort::onReadable() {
    if (!port_) return;
    rxBuf_.append(port_->readAll());
    // Raw mode (Omni VII): binary payloads may contain 0x0D, so framing is
    // the driver's job — hand the bytes over untouched.
    if (rawMode_) {
        if (!rxBuf_.isEmpty()) {
            emit bytesReceived(rxBuf_);
            rxBuf_.clear();
        }
        return;
    }
    // Line mode (Orion): responses are terminated by CR (0x0D).
    int idx;
    while ((idx = rxBuf_.indexOf('\r')) >= 0) {
        QByteArray line = rxBuf_.left(idx);
        rxBuf_.remove(0, idx + 1);
        if (!line.isEmpty()) emit lineReceived(line);
    }
}

} // namespace ttc
