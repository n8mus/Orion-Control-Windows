// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

namespace ttc {

class DcuRotor;

// rotctld-compatible server (:4533) in front of our own DcuRotor — the
// console is the single master of the rotor exactly as it is of the rig
// (:4532), the WinKeyer (:6789) and the skimmer's spots (:7300), and
// cqrlog's SP/LP buttons keep pointing at the same address they always
// did. Speaks the small dialect those clients actually use: "p", "P az
// el", "S", "q", "_", "\dump_state", "\get_info", each also in hamlib's
// extended "+" form.
class RotorServer : public QObject {
    Q_OBJECT
public:
    explicit RotorServer(DcuRotor* rotor, QObject* parent = nullptr);

    bool start(quint16 port = 4533);
    void stop();
    bool listening() const { return srv_.isListening(); }
    quint16 port() const { return srv_.serverPort(); }  // 0 asks for any

private:
    void onNewConnection();
    void handle(QTcpSocket* c, const QString& line);

    QTcpServer srv_;
    DcuRotor* rotor_ = nullptr;
};

} // namespace ttc
