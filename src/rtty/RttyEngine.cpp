// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/RttyEngine.h"

#include <QDateTime>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ttc {

namespace {
constexpr int kRate = 48000;
constexpr int kChunk = kRate / 50;            // 20 ms per pacing step
constexpr int kLeadMs = 140;                  // audio queued ahead of the clock
constexpr int kTailMs = 200;                  // mark after the last stop bit

const QRegularExpression& callRe() {
    static const QRegularExpression re(
        "^[A-Z0-9]{1,3}[0-9][A-Z0-9]*[A-Z](/[A-Z0-9]+)?$|"
        "^[A-Z0-9]+/[A-Z0-9]{1,3}[0-9][A-Z0-9]*[A-Z]$");
    return re;
}
}  // namespace

RttyEngine::Settings RttyEngine::loadSettings() {
    QSettings q;
    Settings s;
    s.baud = q.value("rtty/baud", s.baud).toDouble();
    s.shiftHz = q.value("rtty/shift", s.shiftHz).toInt();
    s.markHz = q.value("rtty/mark", s.markHz).toInt();
    s.reverse = q.value("rtty/reverse", s.reverse).toBool();
    s.usos = q.value("rtty/usos", s.usos).toBool();
    s.diddle = q.value("rtty/diddle", s.diddle).toBool();
    s.txLevelDb = q.value("rtty/txLevelDb", s.txLevelDb).toInt();
    s.squelch = q.value("rtty/squelch", s.squelch).toDouble();
    s.afc = q.value("rtty/afc", s.afc).toBool();
    return s;
}

void RttyEngine::saveSettings(const Settings& s) {
    QSettings q;
    q.setValue("rtty/baud", s.baud);
    q.setValue("rtty/shift", s.shiftHz);
    q.setValue("rtty/mark", s.markHz);
    q.setValue("rtty/reverse", s.reverse);
    q.setValue("rtty/usos", s.usos);
    q.setValue("rtty/diddle", s.diddle);
    q.setValue("rtty/txLevelDb", s.txLevelDb);
    q.setValue("rtty/squelch", s.squelch);
    q.setValue("rtty/afc", s.afc);
}

RttyEngine::RttyEngine(QObject* parent) : QObject(parent) {
    demod_.onChar = [this](char c) { onChar(c); };
    apply(loadSettings());
}

RttyEngine::~RttyEngine() { stopProcess(); }

void RttyEngine::apply(const Settings& s, bool save) {
    s_ = s;
    const bool rev = s.reverse != usb_;        // USB flips the sense itself
    RttyDemod::Params dp;
    dp.sampleRate = kRate;
    dp.markHz = s.markHz;
    dp.spaceHz = s.markHz + s.shiftHz;
    dp.baud = s.baud;
    dp.reverse = rev;
    dp.usos = s.usos;
    dp.squelch = s.squelch;
    dp.afc = s.afc;
    demod_.setParams(dp);                      // resets the channel
    RttyMod::Params mp;
    mp.sampleRate = kRate;
    mp.markHz = s.markHz;
    mp.spaceHz = s.markHz + s.shiftHz;
    mp.baud = s.baud;
    mp.stopBits = s.baud > 60 ? 2.0 : 1.5;
    mp.reverse = rev;
    mp.usos = s.usos;
    mp.diddle = s.diddle;
    mp.level = float(std::pow(10.0, std::clamp(s.txLevelDb, -30, 0) / 20.0));
    mod_.setParams(mp);
    if (save) saveSettings(s);
}

void RttyEngine::setSidebandUsb(bool usb) {
    if (usb == usb_) return;
    usb_ = usb;
    apply(s_, false);
}

void RttyEngine::setToneCenterHz(int hz) {
    Settings s = s_;
    s.markHz = std::clamp(hz - s.shiftHz / 2, 300, 3500);
    apply(s, false);
    demod_.setAfcTrack(true);           // the operator's pick: hold here
}

double RttyEngine::effectiveMarkHz() const {
    return s_.markHz + demod_.afcHz();
}

// ---- receive ---------------------------------------------------------------

void RttyEngine::setRxEnabled(bool on) {
    if (on == rxOn_) return;
    rxOn_ = on;
    if (on) { demod_.reset(); line_.clear(); }
}

void RttyEngine::feedAudio(const float* s, int n) {
    if (!rxOn_ || n <= 0) return;
    demod_.processReal(s, size_t(n));
}

void RttyEngine::onChar(char c) {
    const QChar qc = QChar::fromLatin1(c);
    emit textDecoded(QString(qc));
    if (c == '\n') { scanLine(); line_.clear(); return; }
    line_ += qc;
    if (line_.size() > 400) line_.remove(0, 200);
    // Calls are found at word boundaries so a line without a LF (most
    // contest exchanges) still reports.
    if (c == ' ') scanLine();
}

// "CQ CQ DE W1AW W1AW K" -> W1AW; "N8EM DE W1AW" -> W1AW, to me.
void RttyEngine::scanLine() {
    const QStringList w = line_.toUpper().split(' ', Qt::SkipEmptyParts);
    for (int i = 1; i + 1 < w.size(); ++i) {
        if (w[i] != QLatin1String("DE")) continue;
        QString call = w[i + 1];
        call.remove(QRegularExpression(QStringLiteral("[^A-Z0-9/]")));
        if (!callRe().match(call).hasMatch()) continue;
        if (!myCall_.isEmpty() && call.section('/', 0, 0) == myCall_) continue;
        const bool toMe = !myCall_.isEmpty() && w[i - 1] == myCall_;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (call == lastHeard_ && now - lastHeardSecs_ < 10 && !toMe) continue;
        lastHeard_ = call;
        lastHeardSecs_ = now;
        emit heard(call, toMe);
    }
}

// ---- transmit --------------------------------------------------------------

bool RttyEngine::running() const {
    return play_ && play_->state() != QProcess::NotRunning;
}

bool RttyEngine::start(const QString& sinkNode) {
    if (running()) return true;
    if (sinkNode.isEmpty()) { emit txFailed("no SignaLink playback device"); return false; }
    stopProcess();
    mod_.clear();
    {
        // Transmit where we receive (fldigi's rule): the AFC's trim rides
        // along, so the far end hears us on the pair it is sending on.
        RttyMod::Params mp = mod_.params();
        const int mark = int(std::lround(effectiveMarkHz()));
        mp.markHz = mark;
        mp.spaceHz = mark + s_.shiftHz;
        mod_.setParams(mp);
    }
    ending_ = tailDone_ = false;
    tailSamples_ = 0;
    play_ = new QProcess(this);
    play_->start("pacat", {"--playback", "--raw", "--format=s16le",
                           "--rate=48000", "--channels=1",
                           "--latency-msec=30", "--device=" + sinkNode});
    if (!play_->waitForStarted(2000)) {
        stopProcess();
        emit txFailed("pacat would not start");
        return false;
    }
    connect(play_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (play_ && !ending_) emit txFailed(QString("SignaLink playback ended (%1)").arg(code));
    });
    written_ = 0;
    clock_.start();
    if (!pacer_) {
        pacer_ = new QTimer(this);
        pacer_->setInterval(10);
        connect(pacer_, &QTimer::timeout, this, &RttyEngine::pace);
    }
    pacer_->start();
    pace();
    return true;
}

void RttyEngine::send(const QString& text) {
    if (!running()) return;
    if (ending_) {
        // Text before the over finished draining: the same over goes on.
        ending_ = tailDone_ = false;
        tailSamples_ = 0;
        RttyMod::Params p = mod_.params();
        p.diddle = s_.diddle;
        mod_.setParams(p);
    }
    mod_.queue(text.toLatin1().toStdString());
    emit txProgress(int(mod_.pending()));
}

void RttyEngine::end() {
    if (!running()) return;
    ending_ = true;                            // pace() finishes the job
}

void RttyEngine::abort() {
    mod_.clear();
    ending_ = true;
    stopProcess();
}

void RttyEngine::stopProcess() {
    if (pacer_) pacer_->stop();
    if (!play_) return;
    play_->disconnect(this);
    play_->closeWriteChannel();
    play_->kill();
    play_->waitForFinished(300);
    play_->deleteLater();
    play_ = nullptr;
}

// Keep ~140 ms of audio queued past the wall clock: enough that a busy
// GUI thread never starves pacat, little enough that abort() is quick
// and the end of the over is on the air within a quarter second of the
// last character.
void RttyEngine::pace() {
    if (!running()) return;
    const qint64 due = clock_.elapsed() * kRate / 1000 + kLeadMs * kRate / 1000;
    while (written_ < due) {
        fbuf_.resize(kChunk);
        if (!ending_) {
            mod_.render(fbuf_.data(), kChunk);
        } else if (mod_.busy()) {
            // Let the queued text finish; no diddle after it.
            RttyMod::Params p = mod_.params();
            if (p.diddle) { p.diddle = false; mod_.setParams(p); }
            mod_.render(fbuf_.data(), kChunk);
        } else if (!tailDone_) {
            mod_.renderMark(fbuf_.data(), kChunk);
            tailSamples_ += kChunk;
            if (tailSamples_ >= kTailMs * kRate / 1000) tailDone_ = true;
        } else {
            break;
        }
        sbuf_.resize(kChunk);
        for (int i = 0; i < kChunk; ++i)
            sbuf_[i] = int16_t(std::lround(std::clamp(fbuf_[i], -1.0f, 1.0f) * 32000.0f));
        play_->write(reinterpret_cast<const char*>(sbuf_.data()), kChunk * 2);
        written_ += kChunk;
    }
    if (ending_ && !mod_.busy()) {
        emit txProgress(0);
        // Everything is written: wait until the clock says it has played.
        if (tailDone_ && clock_.elapsed() * kRate / 1000 >= written_) {
            RttyMod::Params p = mod_.params();
            p.diddle = s_.diddle;
            mod_.setParams(p);
            tailSamples_ = 0;
            stopProcess();
            ending_ = false;
            emit txDrained();
        }
    } else if (!ending_) {
        emit txProgress(int(mod_.pending()));
    }
}

}  // namespace ttc
