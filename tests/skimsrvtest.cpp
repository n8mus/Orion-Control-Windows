// SPDX-License-Identifier: GPL-2.0-or-later
// SkimServer protocol check: login chatter, spot line format, and the
// per-call re-announce throttle.
//   cmake --build build --target skimsrvtest && ./build/skimsrvtest
#include <QCoreApplication>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>

#include "cw/SkimServer.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ttc::SkimServer srv;
    srv.setSpotterCall("N8EM");
    if (!srv.start(0)) {                   // ephemeral port
        printf("SKIMSRVTEST: FAILURE — listen failed\n");
        return 1;
    }
    QTcpSocket cli;
    QString rx;
    QObject::connect(&cli, &QTcpSocket::readyRead, &cli, [&] {
        rx += QString::fromLatin1(cli.readAll());
    });
    cli.connectToHost("127.0.0.1", srv.port());

    int step = 0;
    auto* drive = new QTimer(&app);
    QObject::connect(drive, &QTimer::timeout, &app, [&] {
        switch (step++) {
            case 1:
                cli.write("N8EM-TEST\r\n");        // login reply
                break;
            case 3:
                srv.announce("W1AW", 14012000, 25);
                srv.announce("W1AW", 14012000, 25); // must be throttled
                srv.announce("DL1ABC", 14025500, 30);
                break;
            case 6: {
                int fails = 0;
                if (!rx.contains("enter your call"))
                    { printf("FAIL: no login prompt\n"); ++fails; }
                if (!rx.contains("N8EM-TEST de N8EM-#"))
                    { printf("FAIL: no login ack\n"); ++fails; }
                static const QRegularExpression spotRe(
                    "DX de N8EM-#:\\s+14012\\.0\\s+W1AW\\s+CW 25 WPM\\s+"
                    "\\d{4}Z");
                if (!spotRe.match(rx).hasMatch())
                    { printf("FAIL: spot line format\n"); ++fails; }
                if (rx.count("W1AW") != 1)
                    { printf("FAIL: throttle (W1AW x%d)\n",
                             int(rx.count("W1AW"))); ++fails; }
                if (!rx.contains("DL1ABC"))
                    { printf("FAIL: second spot missing\n"); ++fails; }
                printf("--- received ---\n%s----------------\n",
                       qPrintable(rx));
                printf(fails ? "SKIMSRVTEST: %d FAILURE(S)\n"
                             : "SKIMSRVTEST: PASS\n", fails);
                app.exit(fails ? 1 : 0);
                break;
            }
            default: break;
        }
    });
    drive->start(250);
    QTimer::singleShot(8000, &app, [&app] {
        printf("SKIMSRVTEST: FAILURE — timeout\n");
        app.exit(1);
    });
    return app.exec();
}
