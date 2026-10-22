// SPDX-License-Identifier: GPL-2.0-or-later
// FldigiClient verification against tests/fake_fldigi.py:
//   python3 tests/fake_fldigi.py 17362 &  # then
//   cmake --build build --target fldigitest && ./build/fldigitest
// Checks connect + modem/carrier readout, incremental RX text delivery,
// and that set_carrier round-trips through the server.
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

#include "net/FldigiClient.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ttc::FldigiClient fl;
    // Default pairs with fake_fldigi.py; FLTEST_PORT points it at a real
    // fldigi instead (FLTEST_LOOSE=1 then skips the scripted-text checks —
    // a real instance decodes noise, not our story).
    const char* pEnv = std::getenv("FLTEST_PORT");
    const bool loose = std::getenv("FLTEST_LOOSE") != nullptr;
    fl.setEndpoint("127.0.0.1", pEnv ? quint16(atoi(pEnv)) : 17362);

    QString gotModem, gotText;
    int gotCarrier = -1;
    bool sentMove = false;
    QObject::connect(&fl, &ttc::FldigiClient::statusChanged, &fl,
                     [&](bool on, const QString& m, int c) {
                         printf("[status] %s  modem=%s  carrier=%d\n",
                                on ? "up" : "down", qPrintable(m), c);
                         if (on) { gotModem = m; gotCarrier = c; }
                         if (on && !sentMove) {
                             sentMove = true;
                             fl.setCarrier(1500);   // round-trip check
                         }
                     });
    QObject::connect(&fl, &ttc::FldigiClient::rxText, &fl,
                     [&](const QString& t) {
                         gotText += t;
                         printf("[rx] +%d chars: %s\n", int(t.size()),
                                qPrintable(gotText));
                     });
    fl.setActive(true);

    QTimer::singleShot(6000, &app, [&] {
        int fails = 0;
        if (gotModem.isEmpty()) { printf("FAIL: never connected\n"); ++fails; }
        if (!loose && gotModem != "BPSK31") {
            printf("FAIL modem '%s'\n", qPrintable(gotModem));
            ++fails;
        }
        if (gotCarrier != 1500) { printf("FAIL carrier %d (wanted the "
                                         "1500 round-trip)\n", gotCarrier);
                                  ++fails; }
        // The client deliberately skips text that predates the connection
        // (backlog), so only the tail is guaranteed.
        if (!loose && !gotText.contains("W1AW W1AW PSE K")) {
            printf("FAIL rx text '%s'\n", qPrintable(gotText));
            ++fails;
        }
        printf(fails ? "FLDIGITEST: %d FAILURE(S)\n"
                     : "FLDIGITEST: PASS\n", fails);
        app.exit(fails ? 1 : 0);
    });
    return app.exec();
}
