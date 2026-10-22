// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/PskReporter.h"

#include <QDateTime>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QUdpSocket>

namespace ttc {

namespace {
// The two IPFIX templates, byte for byte as freedv-gui sends them.
const unsigned char kRxTemplate[] = {
    0x00, 0x03, 0x00, 0x24, 0x99, 0x92, 0x00, 0x03, 0x00, 0x00,
    0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,          // receiverCallsign
    0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,          // receiverLocator
    0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,          // decodingSoftware
    0x00, 0x00};
const unsigned char kTxTemplate[] = {
    0x00, 0x02, 0x00, 0x34, 0x99, 0x93, 0x00, 0x06,
    0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,          // senderCallsign
    0x80, 0x05, 0x00, 0x05, 0x00, 0x00, 0x76, 0x8F,          // frequency (5 bytes)
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,          // sNR
    0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,          // mode
    0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,          // informationSource
    0x00, 0x96, 0x00, 0x04};                                 // flowStartSeconds
void put16(QByteArray& b, int at, quint16 v) { b[at] = char(v >> 8); b[at + 1] = char(v); }
void app32(QByteArray& b, quint32 v) {
    for (int s = 24; s >= 0; s -= 8) b.append(char((v >> s) & 0xff));
}
void appStr(QByteArray& b, const QString& s) {
    const QByteArray u = s.toLatin1().left(255);
    b.append(char(u.size()));
    b.append(u);
}
void pad4(QByteArray& b, int setStart) {
    while ((b.size() - setStart) % 4) b.append('\0');
    put16(b, setStart + 2, quint16(b.size() - setStart));    // set length
}
}  // namespace

PskReporter::PskReporter(QObject* parent) : QObject(parent) {
    id_ = QRandomGenerator::global()->generate();
    timer_.setInterval(5 * 60 * 1000);                       // freedv-gui's cadence
    connect(&timer_, &QTimer::timeout, this, &PskReporter::flush);
}

void PskReporter::setIdentity(const QString& call, const QString& grid, const QString& software) {
    call_ = call.toUpper();
    grid_ = grid;
    software_ = software;
}

void PskReporter::setEnabled(bool on) {
    if (on == enabled_) return;
    if (!on) flush();                              // what we heard still counts
    enabled_ = on;
    if (on) timer_.start(); else timer_.stop();
}

void PskReporter::addHeard(const QString& call, qint64 hz, int snrDb) {
    if (!enabled_ || call.isEmpty() || hz <= 0) return;
    recs_.push_back({call.toUpper(), hz, snrDb, QDateTime::currentSecsSinceEpoch()});
    if (recs_.size() >= 50) flush();
}

QByteArray PskReporter::encode(const QString& rxCall, const QString& rxGrid,
                               const QString& software, const QVector<Rec>& recs,
                               quint32 seq, quint32 randomId, qint64 nowSecs) {
    QByteArray b;
    b.append(char(0x00));                          // IPFIX version 10
    b.append(char(0x0A));
    b.append(2, '\0');                             // total length, below
    app32(b, quint32(nowSecs));
    app32(b, seq);
    app32(b, randomId);
    b.append(reinterpret_cast<const char*>(kRxTemplate), sizeof kRxTemplate);
    if (!recs.isEmpty())
        b.append(reinterpret_cast<const char*>(kTxTemplate), sizeof kTxTemplate);
    // Receiver record set (template 0x9992).
    int set = int(b.size());
    b.append(char(0x99)); b.append(char(0x92)); b.append(2, '\0');
    appStr(b, rxCall);
    appStr(b, rxGrid);
    appStr(b, software);
    pad4(b, set);
    // Sender records (template 0x9993).
    if (!recs.isEmpty()) {
        set = int(b.size());
        b.append(char(0x99)); b.append(char(0x93)); b.append(2, '\0');
        for (const Rec& r : recs) {
            appStr(b, r.call);
            const quint64 f = quint64(r.hz);
            for (int s = 32; s >= 0; s -= 8) b.append(char((f >> s) & 0xff));
            b.append(char(qint8(std::clamp(r.snr, -128, 127))));
            appStr(b, QStringLiteral("FREEDV"));
            b.append(char(1));                     // informationSource: automatic
            app32(b, quint32(r.atSecs));
        }
        pad4(b, set);
    }
    put16(b, 2, quint16(b.size()));
    return b;
}

void PskReporter::flush() {
    if (recs_.isEmpty() || call_.isEmpty() || grid_.isEmpty()) { recs_.clear(); return; }
    const QByteArray dg = encode(call_, grid_, software_, recs_, seq_++, id_,
                                 QDateTime::currentSecsSinceEpoch());
    const int n = int(recs_.size());
    recs_.clear();
    if (!sock_) sock_ = new QUdpSocket(this);
    QHostInfo::lookupHost("report.pskreporter.info", this, [this, dg, n](const QHostInfo& h) {
        for (const QHostAddress& a : h.addresses()) {
            if (a.protocol() != QAbstractSocket::IPv4Protocol) continue;
            if (sock_->writeDatagram(dg, a, 4739) == dg.size()) emit sent(n);
            return;
        }
    });
}

} // namespace ttc
