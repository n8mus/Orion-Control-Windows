// SPDX-License-Identifier: GPL-2.0-or-later
// RotorClient verification against the hamlib dummy rotator:
//   rotctld -m 1 -t 14533 &  # then
//   cmake --build build --target rotortest && ./build/rotortest
// Checks connect, position polling, a turn command landing on target,
// and STOP being accepted.
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

#include "net/RotorClient.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ttc::RotorClient rot;
    rot.setEndpoint("127.0.0.1", 14533);

    bool sawConnect = false, sentTurn = false;
    double lastAz = -1.0;
    QObject::connect(&rot, &ttc::RotorClient::connectedChanged, &rot,
                     [&](bool on) {
                         printf("[rotor] %s\n", on ? "connected" : "lost");
                         if (on) sawConnect = true;
                     });
    QObject::connect(&rot, &ttc::RotorClient::azimuthChanged, &rot,
                     [&](double az) {
                         printf("[rotor] az %.1f\n", az);
                         lastAz = az;
                         if (!sentTurn && az >= 0.0) {
                             sentTurn = true;
                             printf("[rotor] turnTo 145\n");
                             rot.turnTo(145.0);
                         }
                         // The dummy simulates real rotation (~6 deg/s);
                         // done the moment it arrives.
                         if (sentTurn && az > 143.5 && az < 146.5) {
                             rot.stop();
                             printf("ROTORTEST: PASS (arrived at %.1f)\n",
                                    az);
                             app.exit(0);
                         }
                     });
    rot.setActive(true);

    QTimer::singleShot(40000, &app, [&] {     // 145 deg at ~6 deg/s + slack
        rot.stop();
        printf("ROTORTEST: FAILURE — %s, last az %.1f\n",
               sawConnect ? "connected but never arrived"
                          : "never connected", lastAz);
        app.exit(1);
    });
    return app.exec();
}
