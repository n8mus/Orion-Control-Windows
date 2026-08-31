// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QByteArray>
#include <string>

class QSerialPort;

namespace ttc {

// Minimal serial wrapper on QSerialPort (cross-platform: the Windows port
// made the "portable choice later" note come due — one implementation for
// /dev/* and COMx alike). Line-oriented (CR-terminated) to match the
// Ten-Tec ASCII protocol. QSerialPort also keeps the old POSIX guarantees:
// it opens O_CLOEXEC through qt_safe_open (audio-helper children must not
// inherit CAT ports — measured live) and takes the platform's exclusive
// lock, so a second opener fails loudly instead of splitting the stream.
class SerialPort : public QObject {
    Q_OBJECT
public:
    explicit SerialPort(QObject* parent = nullptr);
    ~SerialPort() override;

    // 57600 8N1 for both Ten-Tec radios; hwHandshake=true for the Omni VII.
    bool open(const std::string& device, int baud = 57600, bool hwHandshake = false);
    void close();
    bool isOpen() const;

    void write(const QByteArray& data);
    // Raw mode: emit bytesReceived with unframed chunks instead of CR-split
    // lines — the Omni VII's binary payloads can contain 0x0D.
    void setRawMode(bool on) { rawMode_ = on; }

signals:
    void lineReceived(const QByteArray& line);   // one CR-delimited response
    void bytesReceived(const QByteArray& chunk); // raw mode only
    void ioError(const QString& what);

private slots:
    void onReadable();

private:
    QSerialPort* port_ = nullptr;
    QByteArray rxBuf_;
    bool rawMode_ = false;
};

} // namespace ttc
