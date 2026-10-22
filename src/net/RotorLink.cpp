// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/RotorLink.h"

#include "net/RotorClient.h"
#include "net/RotorServer.h"
#include "radio/DcuRotor.h"

#include <QSettings>

namespace ttc {

RotorLink::RotorLink(QObject* parent) : QObject(parent) {
    dev_ = new DcuRotor(this);
    srv_ = new RotorServer(dev_, this);
    tcp_ = new RotorClient(this);
    connect(dev_, &DcuRotor::azimuthChanged, this, &RotorLink::azimuthChanged);
    connect(dev_, &DcuRotor::connectedChanged, this, &RotorLink::connectedChanged);
    connect(tcp_, &RotorClient::azimuthChanged, this, &RotorLink::azimuthChanged);
    connect(tcp_, &RotorClient::connectedChanged, this, &RotorLink::connectedChanged);
}

void RotorLink::configure() {
    QSettings s;
    const bool wasActive = active_;
    if (wasActive) setActive(false);
    direct_ = s.value("rotor/mode", "direct").toString()
              != QLatin1String("rotctld");
    // No default device: unset means "no rotor configured yet", same as
    // every other port in Setup. TTC_ROTOR_DEV overrides for testing.
    const QByteArray env = qgetenv("TTC_ROTOR_DEV");
    dev_->setDevice(env.isEmpty() ? s.value("rotor/device").toString()
                                  : QString::fromLocal8Bit(env));
    tcp_->setEndpoint(s.value("rotor/host", "127.0.0.1").toString(),
                      quint16(s.value("rotor/port", 4533).toUInt()));
    if (wasActive) setActive(true);
}

void RotorLink::setActive(bool on) {
    active_ = on;
    if (direct_) {
        tcp_->setActive(false);
        dev_->setActive(on);
        // Only serve the rotor while we actually hold it — a dead
        // :4533 tells cqrlog the truth instead of stalling it, and it
        // leaves the port free for a real rotctld if this station goes
        // back to one.
        if (on && dev_->holding()) {
            const quint16 port =
                quint16(QSettings().value("rotor/port", 4533).toUInt());
            srv_->start(port);
        } else {
            srv_->stop();
        }
    } else {
        dev_->setActive(false);
        srv_->stop();
        tcp_->setActive(on);
    }
}

bool RotorLink::connected() const {
    return direct_ ? dev_->connected() : tcp_->connected();
}

double RotorLink::azimuth() const {
    return direct_ ? dev_->azimuth() : tcp_->azimuth();
}

double RotorLink::target() const {
    return direct_ ? dev_->target() : tcp_->target();
}

void RotorLink::turnTo(double azDeg) {
    if (direct_) dev_->turnTo(azDeg);
    else         tcp_->turnTo(azDeg);
}

void RotorLink::stop() {
    if (direct_) dev_->stop();
    else         tcp_->stop();
}

} // namespace ttc
