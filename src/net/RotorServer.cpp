// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/RotorServer.h"

#include "radio/DcuRotor.h"

#include <QTcpSocket>

namespace ttc {

RotorServer::RotorServer(DcuRotor* rotor, QObject* parent)
    : QObject(parent), rotor_(rotor) {
    connect(&srv_, &QTcpServer::newConnection, this,
            &RotorServer::onNewConnection);
}

bool RotorServer::start(quint16 port) {
    stop();
    // Loopback only: this is the station's own rotor, not a service for
    // the network (same call as SkimServer makes).
    return srv_.listen(QHostAddress::LocalHost, port);
}

void RotorServer::stop() {
    if (srv_.isListening()) srv_.close();
}

void RotorServer::onNewConnection() {
    while (QTcpSocket* c = srv_.nextPendingConnection()) {
        connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        connect(c, &QTcpSocket::readyRead, this, [this, c] {
            while (c->canReadLine())
                handle(c, QString::fromLatin1(c->readLine()).trimmed());
        });
    }
}

void RotorServer::handle(QTcpSocket* c, const QString& line) {
    if (line.isEmpty()) return;
    // "+p" asks for the labelled long form; the plain form is what
    // cqrlog and Not1MM send. Either way the command is the rest.
    QString cmd = line;
    bool ext = false;
    if (cmd.startsWith(QLatin1Char('+'))) { ext = true; cmd = cmd.mid(1).trimmed(); }

    const QStringList tok = cmd.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString verb = tok.value(0);
    const auto reply = [c](const QString& s) { c->write(s.toLatin1()); };
    const auto rprt = [&](int n) {
        if (ext) reply(QString("%1:\nRPRT %2\n").arg(verb).arg(n));
        else if (n != 0) reply(QString("RPRT %1\n").arg(n));
    };

    if (verb == QLatin1String("p") || verb == QLatin1String("get_pos")) {
        const double az = rotor_->azimuth();
        // No reading yet (or the link is down) is an error, not a lie —
        // a client that gets told "0.0" points a beam at north.
        if (az < 0.0) { rprt(-5); return; }   // RIG_ETIMEOUT
        if (ext)
            reply(QString("get_pos:\nAzimuth: %1\nElevation: %2\nRPRT 0\n")
                      .arg(az, 0, 'f', 2).arg(0.0, 0, 'f', 2));
        else
            reply(QString("%1\n%2\n").arg(az, 0, 'f', 2).arg(0.0, 0, 'f', 2));
        return;
    }
    if (verb == QLatin1String("P") || verb == QLatin1String("set_pos")) {
        bool ok = false;
        const double az = tok.value(1).toDouble(&ok);
        if (!ok || az < 0.0 || az > 360.0) { rprt(-1); return; }  // RIG_EINVAL
        const bool sent = rotor_->turnTo(az);
        if (ext) reply(QString("set_pos: %1 %2\n").arg(tok.value(1), tok.value(2)));
        rprt(sent ? 0 : -5);              // -5 = RIG_ETIMEOUT: no rotor here
        return;
    }
    if (verb == QLatin1String("S") || verb == QLatin1String("stop")) {
        rprt(rotor_->stop() ? 0 : -5);
        return;
    }
    if (verb == QLatin1String("q") || verb == QLatin1String("Q")
        || verb == QLatin1String("quit")) {
        c->disconnectFromHost();
        return;
    }
    if (verb == QLatin1String("_") || verb == QLatin1String("get_info")) {
        reply(QStringLiteral("DCU2/DCU3/YRC-1\n"));
        return;
    }
    if (verb == QLatin1String("\\dump_state")) {
        // Protocol version 1, hamlib's Hy-Gain DCU2/DCU3/YRC-1 model
        // number, azimuth-only: what a client gets from the real daemon.
        reply(QStringLiteral("1\n406\nmin_az=0.000000\nmax_az=360.000000\n"
                             "min_el=0.000000\nmax_el=0.000000\n"
                             "south_zero=0\nrot_type=Other\ndone\n"));
        return;
    }
    rprt(-11);                                  // RIG_ENIMPL
}

} // namespace ttc
