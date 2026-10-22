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
// guesswork the day this landed). Format ".tciq", version 2: 40-byte
// header (magic "TTCIQ02\0", double sampleRate, double centerHz = the
// LO's absolute frequency, qint64 epoch ms, double dialHz), then
// interleaved int16 I/Q. The dial is RECORDED, not derived: band frames
// and CTUN park the LO anywhere, so center − dial is not a constant
// (review-found before any capture went wrong). Readers must still take
// v1 (32-byte "TTCIQ01\0", no dial): the 2026-07 ground-truth captures
// are 500 ksps with the LO exactly 60 kHz above the dial. 1 MS/s is
// ~4 MB/s, ~14 GB/hour.
//
// feed() runs on the SDR streaming thread and only converts + appends
// under a mutex; a GUI-thread timer drains to the file so disk latency
// can never stall the capture callback.
class IqRecorder : public QObject {
    Q_OBJECT
public:
    explicit IqRecorder(QObject* parent = nullptr);

    bool start(const QString& path, double sampleRate, double centerHz,
               double dialHz);
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
