// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>
#include <vector>
#include "sdr/SdrSource.h"

class QTimer;

namespace ttc {

// Band recorder: writes the raw SDR capture to disk so the whole console
// pipeline — panadapter, waterfall, CW reader, skimmer — can be re-run
// against a real over-the-air recording (decoder tuning stopped being
// guesswork the day this landed). Format ".tciq": 32-byte header
// (magic "TTCIQ01\0", double sampleRate, double centerHz = the LO's
// absolute frequency, qint64 epoch ms), then interleaved int16 I/Q.
// 500 ksps is ~2 MB/s, ~7 GB/hour.
//
// feed() runs on the SDR streaming thread and only converts + appends
// under a mutex; a GUI-thread timer drains to the file so disk latency
// can never stall the capture callback.
class IqRecorder : public QObject {
    Q_OBJECT
public:
    explicit IqRecorder(QObject* parent = nullptr);

    bool start(const QString& path, double sampleRate, double centerHz);
    void stop();
    bool active() const { return active_; }
    QString path() const { return file_.fileName(); }
    qint64 bytesWritten() const { return written_; }

    void feed(const IqBlock& iq);          // SDR streaming thread

    static QString defaultDir();           // ~/.local/share/.../iq

signals:
    void progress(qint64 bytes, double secs);

private:
    void drain();

    QFile file_;
    QTimer* timer_ = nullptr;
    QMutex mux_;
    std::vector<int16_t> pending_;
    bool   active_ = false;
    double rate_ = 500000.0;
    qint64 written_ = 0;
};

} // namespace ttc
