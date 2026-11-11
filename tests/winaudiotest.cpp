// SPDX-License-Identifier: GPL-3.0-or-later
// Windows audio endpoints behind the Linux pacat / parec seams:
//   cmake --build build-win --target winaudiotest && build-win\winaudiotest
//
// - PcmSink on the GUI thread (RTTY, the practice callers, the pickup tone)
//   and on its own thread (SDR receive): fed in real time it must keep its
//   queue bounded and play out to empty, which only happens when the sound
//   device really takes the audio.
// - LiveVoice end to end on those endpoints: mic capture -> voice chain ->
//   player, with the backlog held.
//
// Uses the PC's DEFAULT output and input only and never names the radio's
// sound device, so nothing here can reach a transmitter. What it plays is a
// 1 kHz tone at -60 dBFS (LiveVoice is muted: silence).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/AudioIo.h"
#include "audio/LiveVoice.h"

using namespace ttc;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Feed `secs` of tone at the wall clock's pace, the way the console's own
// producers do; report the deepest queue and whether it played out.
static void runSink(const char* name, bool ownThread, int rate, int secs) {
    std::printf("%s\n", name);
    PcmSink sink;
    if (ownThread) sink.useOwnThread();
    QString why;
    QObject::connect(&sink, &PcmSink::errorText, [&why](const QString& w) { why = w; });
    const bool up = sink.start(QString(), rate, 40);
    if (!up) {
        std::printf("  (no default output device: %s) - skipped\n", qPrintable(why));
        return;
    }
    const int step = rate / 50;                        // 20 ms
    std::vector<int16_t> buf(static_cast<size_t>(step));
    const double amp = 32767.0 * std::pow(10.0, -60.0 / 20.0);
    qint64 written = 0, deepest = 0;
    QElapsedTimer clock;
    clock.start();
    QTimer pace;
    pace.setTimerType(Qt::PreciseTimer);
    pace.setInterval(10);
    QEventLoop loop;
    QObject::connect(&pace, &QTimer::timeout, [&] {
        const qint64 due = clock.elapsed() * rate / 1000 + rate / 20;   // 50 ms lead
        while (written < due && written < qint64(rate) * secs) {
            for (int i = 0; i < step; ++i)
                buf[size_t(i)] = int16_t(std::lround(
                    amp * std::sin(2.0 * M_PI * 1000.0 * double(written + i) / rate)));
            sink.write(reinterpret_cast<const char*>(buf.data()), qint64(step) * 2);
            written += step;
        }
        deepest = std::max(deepest, sink.backlogBytes());
        if (written >= qint64(rate) * secs) loop.quit();
    });
    pace.start();
    loop.exec();
    pace.stop();
    const double deepestMs = double(deepest) / (rate * 2 / 1000.0);
    // The device queue is 40 ms and we lead by 50: a device that takes the
    // audio keeps the total near 100 ms. One that does not would hold the
    // whole clip.
    std::printf("  wrote %.1f s, deepest queue %.0f ms\n", double(written) / rate, deepestMs);
    check(deepestMs < 400.0, "queue stays bounded while fed in real time");
    QElapsedTimer drain;
    drain.start();
    while (sink.backlogBytes() > qint64(rate) * 2 * 20 / 1000 && drain.elapsed() < 2000) spin(20);
    std::printf("  played out in %lld ms (left %lld bytes)\n",
                static_cast<long long>(drain.elapsed()),
                static_cast<long long>(sink.backlogBytes()));
    check(sink.backlogBytes() <= qint64(rate) * 2 * 20 / 1000, "queue plays out to empty");
    sink.stop();
    check(!sink.running(), "stops");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("winaudiotest");
    QCoreApplication::setApplicationName("winaudiotest");

    std::printf("outputs: %d   inputs: %d\n",
                int(AudioPlayback::outputDescriptions().size()),
                int(AudioCapture::inputDescriptions().size()));

    runSink("PcmSink, GUI thread, 8 kHz (practice callers)", false, 8000, 3);
    runSink("PcmSink, GUI thread, 48 kHz (RTTY, pickup tone)", false, 48000, 3);
    runSink("PcmSink, own thread, 48 kHz (SDR receive)", true, 48000, 3);

    std::printf("LiveVoice, default mic -> default output, MUTED\n");
    if (AudioCapture::inputDescriptions().isEmpty() || AudioPlayback::outputDescriptions().isEmpty()) {
        std::printf("  (no default input or output device) - skipped\n");
    } else {
        LiveVoice live;
        QString why;
        QObject::connect(&live, &LiveVoice::failed, [&why](const QString& w) { why = w; });
        live.setMuted(true);                           // silence out, chain runs
        VoiceEqSettings s;
        const bool up = live.start(QString(), QString(), s);
        if (!up) std::printf("  start failed: %s\n", qPrintable(why));
        check(up, "starts on the default endpoints");
        if (up) {
            double deepest = 0.0;
            float loudest = -120.f;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 4000) {
                spin(50);
                deepest = std::max(deepest, live.backlogMs());
                loudest = std::max(loudest, live.inputPeakDb());
            }
            std::printf("  4 s: mic peak %.0f dBFS, deepest queue %.0f ms, %d blocks dropped\n",
                        loudest, deepest, live.droppedBlocks());
            check(live.running(), "still running after 4 s");
            check(loudest > -120.f, "the mic delivers samples");
            check(deepest < 200.0, "the backlog is held");
            live.stop();
            check(!live.running(), "stops");
        }
    }

    std::printf(g_fail ? "winaudiotest: %d FAILED\n" : "winaudiotest: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
