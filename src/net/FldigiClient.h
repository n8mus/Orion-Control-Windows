// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariant>
#include <functional>

class QTimer;

namespace ttc {

// Minimal XML-RPC client for fldigi (default 127.0.0.1:7362). The console
// stays rig master (fldigi follows the dial through rigctld :4532); this
// link adds what rig control can't carry: the active modem + audio
// carrier, the decoded RX text stream, and set_carrier so clicking a
// trace in the panadapter passband drops fldigi's cursor on it.
//
// Polling only runs while setActive(true) (the DIGI window is open);
// fldigi absent = connected() false and a retry every poll tick, nothing
// noisier.
class FldigiClient : public QObject {
    Q_OBJECT
public:
    explicit FldigiClient(QObject* parent = nullptr);

    void setEndpoint(const QString& host, quint16 port);
    void setActive(bool on);

    bool    connected() const { return connected_; }
    QString modem()   const { return modem_; }
    int     carrier() const { return carrier_; }

    void setCarrier(int audioHz);          // modem.set_carrier

signals:
    // Fires on connect/disconnect and whenever modem or carrier moves.
    void statusChanged(bool connected, const QString& modem, int carrierHz);
    void rxText(const QString& text);      // decoded characters, incremental

private:
    // One XML-RPC call; done(ok, value) with the first <value> flattened
    // to a string (base64 decoded when tagged as such).
    void call(const QString& method, const QList<QVariant>& args,
              std::function<void(bool, const QString&)> done);
    void poll();
    void setConnected(bool on);

    QNetworkAccessManager nam_;
    QTimer* timer_ = nullptr;
    QString host_ = QStringLiteral("127.0.0.1");
    quint16 port_ = 7362;
    bool    connected_ = false;
    QString modem_;
    int     carrier_ = 0;
    qint64  rxLen_ = -1;                   // fldigi RX-buffer chars consumed
    int     inFlight_ = 0;                 // don't stack polls on a dead link
};

} // namespace ttc
