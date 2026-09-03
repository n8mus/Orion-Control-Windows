// SPDX-License-Identifier: GPL-2.0-or-later
// Headless capture smoke test for the Qt Multimedia audio path (the
// Windows sibling of sdr_probe): lists every input Qt sees, opens the one
// matching argv[1] (default "USB Audio") through the console's own
// AudioCapture, and prints bytes + RMS twice a second. Distinguishes
// "no device" / "device but silence" (permission, dead level) / "audio
// flowing" (any remaining problem is downstream of capture).
#include "audio/AudioIo.h"

#include <QCoreApplication>
#include <QTimer>
#include <cmath>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString match = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("USB Audio");

    printf("inputs Qt sees:\n");
    for (const QString& d : ttc::AudioCapture::inputDescriptions())
        printf("  \"%s\"\n", qPrintable(d));
    printf("outputs Qt sees:\n");
    for (const QString& d : ttc::AudioPlayback::outputDescriptions())
        printf("  \"%s\"\n", qPrintable(d));

    ttc::AudioCapture cap;
    QObject::connect(&cap, &ttc::AudioCapture::errorText,
                     [](const QString& e) {
                         printf("ERROR: %s\n", qPrintable(e));
                     });
    static qint64 bytes = 0;
    static double sumSq = 0.0;
    static qint64 samples = 0;
    QObject::connect(&cap, &ttc::AudioCapture::chunk,
                     [](const QByteArray& c) {
                         bytes += c.size();
                         const auto* s =
                             reinterpret_cast<const int16_t*>(c.constData());
                         const qsizetype n = c.size() / 2;
                         for (qsizetype i = 0; i < n; ++i)
                             sumSq += double(s[i]) * double(s[i]);
                         samples += n;
                     });
    if (!cap.start(match, 48000)) {
        printf("capture did not start\n");
        return 1;
    }
    printf("opened: \"%s\"  (match \"%s\")\n",
           qPrintable(cap.deviceDescription()), qPrintable(match));

    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [] {
        const double rms =
            samples ? std::sqrt(sumSq / double(samples)) : 0.0;
        const double db = 20.0 * std::log10(rms / 32768.0 + 1e-12);
        printf("bytes=%lld  rms=%.0f (%.1f dBFS)\n",
               static_cast<long long>(bytes), rms, db);
        fflush(stdout);
        bytes = 0; sumSq = 0.0; samples = 0;
    });
    tick.start(500);
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);
    return app.exec();
}
