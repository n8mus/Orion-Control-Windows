// SPDX-License-Identifier: GPL-3.0-or-later
// Copy Assist:
//   cmake --build build --target asrtest && ./build/asrtest
// - callsign extraction from whisper-style text (one word, letter runs,
//   hyphens, phonetics, trailing number words, non-calls)
// - with the base.en model present: G8SEZ's test recording (rade_c's
//   input_sample.wav): with G8SEZ as a band-map hint the call copies as
//   G8SEZ; and three copies back to back in real time (a talker who never
//   pauses) are cut into <= 7 s pieces that keep up (each within 8 s).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QFileInfo>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "asr/AsrEngine.h"

using namespace ttc;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
}

static QString calls(const QString& t) {
    QStringList l;
    for (const CallHit& h : extractCalls(t)) l << h.call;
    return l.join(',');
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    struct Case { const char* text; const char* want; };
    const Case cases[] = {
        {"G8SEZ test. Testing G8SEZ test.", "G8SEZ,G8SEZ"},
        {"this is Kilo One Alpha Bravo Charlie, you're five nine", "K1ABC"},
        {"Kilo One Alpha Bravo Charlie five nine", "K1ABC"},
        {"G-8-S-E-Z test", "G8SEZ"},
        {"CQ CQ de N8EM N 8 E M", "N8EM,N8EM"},
        {"VK5DGR portable, W1AW/P and 9A1A", "VK5DGR,W1AW/P,9A1A"},
        {"Victor Kilo Five Delta Golf Romeo", "VK5DGR"},
        {"1, 2, 3, 4, 5. Five nine nine. QSL 73", ""},
        {"I have a new antenna at 40 feet", ""},
    };
    for (const Case& c : cases) {
        const QString got = calls(QString::fromUtf8(c.text));
        std::printf("    \"%s\" -> [%s]\n", c.text, qPrintable(got));
        check(got == QLatin1String(c.want), "  extracted as expected");
    }

    const QString rep = AsrEngine::collapseRepeats("testing G8SEZ test 1, 2, 3, 3, 3, 3, 3, 3, 3, 3");
    std::printf("    repetition loop -> \"%s\"\n", qPrintable(rep));
    check(rep.count('3') <= 4 && rep.contains("G8SEZ"), "repetition loop collapses");
    check(AsrEngine::collapseRepeats("five nine, five nine") == "five nine, five nine",
          "  ordinary repeats untouched");

    const QString model = AsrEngine::defaultModelPath();
#ifdef RADE_WAV_DIR
    const char* wavPath = RADE_WAV_DIR "/input_sample.wav";
#else
    const char* wavPath = "input_sample.wav";
#endif
    std::ifstream f(wavPath, std::ios::binary);
    if (!QFileInfo::exists(model) || !f) {
        std::printf("  SKIP whisper checks (model %s, wav %s)\n", qPrintable(model), wavPath);
    } else {
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<float> pcm16;
        for (size_t p = 12; p + 8 <= b.size();) {
            uint32_t len;
            std::memcpy(&len, &b[p + 4], 4);
            if (std::memcmp(&b[p], "data", 4) == 0) {
                for (size_t i = 0; i + 1 < len && p + 9 + i < b.size(); i += 2) {
                    int16_t v;
                    std::memcpy(&v, &b[p + 8 + i], 2);
                    pcm16.push_back(v / 32768.f);
                }
                break;
            }
            p += 8 + len;
        }
        AsrEngine eng;
        QString err;
        check(eng.start(model, 8, &err), "whisper loads base.en");
        // Direct: with and without the band-map hint.
        const AsrEngine::Result plain = eng.transcribe(pcm16, {});
        const AsrEngine::Result hinted = eng.transcribe(pcm16, {"G8SEZ", "N8EM"});
        std::printf("    no hint (%.1f s): %s  [conf %.2f]\n", plain.decodeMs / 1000.0,
                    qPrintable(plain.text), plain.confidence);
        std::printf("    hinted  (%.1f s): %s  [conf %.2f]\n", hinted.decodeMs / 1000.0,
                    qPrintable(hinted.text), hinted.confidence);
        check(plain.text.contains("test", Qt::CaseInsensitive), "speech transcribes");
        check(plain.decodeMs < 1000.0 * pcm16.size() / 16000, "faster than real time");
        bool g8 = false;
        for (const CallHit& h : hinted.calls) g8 |= h.call == "G8SEZ";
        check(g8, "band-map hint copies the call as G8SEZ");

        // Live path, in REAL time like the radio: hiss, then the recording
        // three times back to back with no pause between (a talker who
        // never stops), then hiss. Pieces must stay short and the copy
        // must keep up; lag = result arrival - end of that piece's audio.
        QVector<AsrEngine::Result> got;
        QVector<double> lagS;
        QElapsedTimer clock;
        clock.start();
        double fedS = 0;                           // audio seconds fed so far
        qint64 startMs = 0;
        QObject::connect(&eng, &AsrEngine::transcribed, [&](const AsrEngine::Result& r) {
            got.push_back(r);
            lagS.push_back((QDateTime::currentMSecsSinceEpoch() - r.atMs) / 1000.0 - r.seconds);
        });
        eng.setHints({"G8SEZ"}, 14236000);
        std::vector<float> feed;
        unsigned seed = 3;
        const auto hiss = [&] { seed = seed * 1103515245u + 12345u; return 0.003f * (((seed >> 9) & 0xffff) / 32768.f - 1.f); };
        for (int i = 0; i < 48000 * 2; ++i) feed.push_back(hiss());
        for (int rep = 0; rep < 3; ++rep)
            for (size_t i = 0; i + 1 < pcm16.size(); ++i)
                for (int k = 0; k < 3; ++k)
                    feed.push_back(0.5f * (pcm16[i] + (pcm16[i + 1] - pcm16[i]) * k / 3.f) + hiss());
        for (int i = 0; i < 48000 * 3; ++i) feed.push_back(hiss());
        startMs = QDateTime::currentMSecsSinceEpoch();
        clock.restart();
        for (size_t o = 0; o + 480 <= feed.size(); o += 480) {
            eng.feed48k(&feed[o], 480, false);
            fedS += 0.01;
            while (clock.elapsed() < qint64(fedS * 1000)) app.processEvents(QEventLoop::AllEvents, 2);
        }
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 12000) app.processEvents(QEventLoop::AllEvents, 50);   // stragglers
        std::printf("    nonstop talker, %.0f s of speech: %d piece(s)\n", 3 * pcm16.size() / 16000.0,
                    int(got.size()));
        double maxLen = 0, maxLag = 0, sumLag = 0;
        QString all;
        for (int i = 0; i < got.size(); ++i) {
            std::printf("      %.1f s, +%.1f s late: %s [conf %.2f]\n", got[i].seconds, lagS[i],
                        qPrintable(got[i].text), got[i].confidence);
            maxLen = std::max(maxLen, got[i].seconds);
            maxLag = std::max(maxLag, lagS[i]);
            sumLag += lagS[i];
            all += got[i].text + ' ';
        }
        std::printf("    lag: mean %.1f s, worst %.1f s\n", got.isEmpty() ? 0 : sumLag / got.size(), maxLag);
        check(got.size() >= 4, "the nonstop over is cut into pieces");
        check(maxLen <= 7.6, "no piece longer than ~7 s");
        check(all.count("G8SEZ") >= 3 && all.contains("test", Qt::CaseInsensitive),
              "the copy keeps the call and the words");
        check(maxLag < 8.0, "each piece lands within 8 s of its end");
        eng.stop();
    }
    if (fails == 0) std::printf("asrtest: all checks passed\n");
    return fails ? 1 : 0;
}
