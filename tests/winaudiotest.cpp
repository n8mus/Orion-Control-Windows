// SPDX-License-Identifier: GPL-3.0-or-later
// Windows audio endpoints behind the Linux pacat / parec seams:
//   cmake --build build-win --target winaudiotest && build-win\winaudiotest
//   winaudiotest --offline     the sample-rate converter only: no device opened
//
// - PcmResampler, pure arithmetic: a tone converted in ragged chunks must
//   come out sample for sample what it does in one piece, stay a clean
//   tone, and never step further than a sine can. (The first Windows
//   practice run buzzed: a wrong sample or five at every chunk boundary.)
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

// ---- the rate converter, offline ------------------------------------------

static std::vector<int16_t> tone(double hz, int rate, double secs, double amp) {
    std::vector<int16_t> x(static_cast<size_t>(rate * secs));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = int16_t(std::lround(amp * std::sin(2.0 * M_PI * hz * double(i) / rate)));
    return x;
}

static void runResampler(const char* name, double hz, int from, int to) {
    std::printf("%s\n", name);
    const double amp = 16000.0;
    const std::vector<int16_t> in = tone(hz, from, 2.0, amp);

    PcmResampler whole;
    whole.reset(from, to);
    std::vector<int16_t> ref;
    whole.process(in.data(), in.size(), ref);

    // The same stream cut the way a 5 ms pump cuts it: uneven, down to
    // single samples.
    PcmResampler ragged;
    ragged.reset(from, to);
    std::vector<int16_t> cut;
    static const size_t sizes[] = {1, 7, 40, 80, 3, 160, 33, 2, 480, 11};
    size_t at = 0, k = 0;
    while (at < in.size()) {
        const size_t n = std::min(sizes[k++ % 10], in.size() - at);
        ragged.process(in.data() + at, n, cut);
        at += n;
    }
    check(cut == ref, "ragged chunks give the same samples as one piece");

    // Against the ideal tone at the output rate (output m sits at input
    // position m * from / to; the converter adds no shift of its own).
    double sig = 0.0, err = 0.0, worstStep = 0.0;
    const size_t skip = size_t(to) / 100;              // let the start settle
    for (size_t m = skip; m < ref.size(); ++m) {
        const double ideal = amp * std::sin(2.0 * M_PI * hz * double(m) / to);
        sig += ideal * ideal;
        err += (ref[m] - ideal) * (ref[m] - ideal);
        worstStep = std::max(worstStep, std::fabs(double(ref[m]) - double(ref[m - 1])));
    }
    const double snr = 10.0 * std::log10(sig / std::max(err, 1e-9));
    const double sineStep = amp * 2.0 * M_PI * hz / to;    // steepest a sine gets
    std::printf("  %zu -> %zu samples, %.1f dB clean, largest step %.0f (a sine: %.0f)\n",
                in.size(), ref.size(), snr, worstStep, sineStep);
    check(std::llabs(static_cast<long long>(ref.size())
                     - static_cast<long long>(double(in.size()) * to / from)) <= 16,
          "the right number of samples come out");
    check(snr > 45.0, "stays a clean tone (better than 45 dB)");
    check(worstStep < sineStep * 1.15 + 2.0, "no click: never steps further than the tone can");
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

    runResampler("PcmResampler 8 kHz -> 48 kHz, 600 Hz (practice callers)", 600.0, 8000, 48000);
    runResampler("PcmResampler 7013 Hz -> 48 kHz, 700 Hz (Omni RIP audio)", 700.0, 7013, 48000);
    runResampler("PcmResampler 48 kHz -> 44.1 kHz, 1 kHz (a 44.1 kHz sound card)", 1000.0, 48000, 44100);
    runResampler("PcmResampler 44.1 kHz -> 48 kHz, 1 kHz (capture from one)", 1000.0, 44100, 48000);
    {
        std::printf("PcmResampler, equal rates\n");
        const std::vector<int16_t> in = tone(600.0, 48000, 0.1, 16000.0);
        PcmResampler same;
        same.reset(48000, 48000);
        std::vector<int16_t> out;
        same.process(in.data(), in.size(), out);
        check(out == in, "passes the samples through untouched");
    }
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--offline")) {
        std::printf(g_fail ? "winaudiotest (offline): %d FAILED\n"
                           : "winaudiotest (offline): all checks passed\n", g_fail);
        return g_fail ? 1 : 0;
    }

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
