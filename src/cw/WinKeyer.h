// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>

class QSerialPort;
class QTimer;

namespace ttc {

// K1EL WinKeyer USB client (WK2/WK3 protocol, 1200 8N2, DTR on / RTS off).
// The keyer hardware owns everything that matters: element timing, the
// paddle (which instantly interrupts buffered sending — break-in is a
// hardware feature, we just get told), and the speed pot. Protocol per the
// K1EL datasheet, cross-checked against the GPL implementations in cqrlog
// (uCWKeying.pas) and fldigi. We deliberately never write the keyer's mode
// register, so paddle mode/swap/sidetone stay exactly as the owner set them.
class WinKeyer : public QObject {
    Q_OBJECT
public:
    explicit WinKeyer(QObject* parent = nullptr);
    ~WinKeyer() override;

    bool open(const QString& device);      // null + echo test + host open
    void close();                          // host close; keyer standalone again
    bool isOpen() const { return open_; }
    QString lastError() const { return err_; }

    void setSpeed(int wpm);                // host speed (0x02)
    int  speed() const { return wpm_; }
    void setPotRange(int minWpm, int maxWpm);  // 0x05 min range 0
    void send(const QString& text);        // buffered ASCII (uppercased)
    void stop();                           // 0x0A: dump the buffer, key up
    void tune(bool on);                    // 0x0B: steady carrier
    void backspace();                      // 0x08: unsend last buffered char

signals:
    void potChanged(int wpm);              // physical speed pot moved
    void busyChanged(bool sending);        // buffered sending active
    void breakIn();                        // paddle touched — buffer dumped
    void errorOccurred(const QString& msg);

private:
    void onReadyRead();

    QSerialPort* ser_ = nullptr;
    bool open_ = false;
    bool busy_ = false;
    int  wpm_ = 25;
    int  potMin_ = 7;                      // pot byte is offset from this
    int  potPending_ = -1;                 // latest raw pot report
    int  potEmitted_ = -1;                 // last value actually emitted
    QTimer* potTimer_ = nullptr;           // settle timer (see onReadyRead)
    QString err_;
};

} // namespace ttc
