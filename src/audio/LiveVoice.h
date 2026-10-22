// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <vector>

#include "audio/VoiceEq.h"

class QProcess;

namespace ttc {

class RadeTx;

// LIVE MIC (PC voice chain phase 2): the operator's mic, through the same
// gate -> EQ -> de-esser -> compressor -> limiter chain as the VK
// messages, streamed into the radio's sound device (SignaLink) the whole
// time it is on. Keying is NOT ours: the operator keys the radio with his
// own PTT and the radio sends whatever is on its line input — so the
// SignaLink's own Auto-PTT must not be wired (its PTT jumper out), or this
// stream would key the transmitter by itself.
//
// Rides the pulse layer like the console's other live audio (AudioCwSource,
// RipAudio): parec captures 48 kHz s16 mono, pacat plays. Small latency
// targets keep the operator's ear (Orion MON) and mouth together.
class LiveVoice : public QObject {
    Q_OBJECT
public:
    explicit LiveVoice(QObject* parent = nullptr);
    ~LiveVoice() override;

    bool start(const QString& sourceNode, const QString& sinkNode,
               const VoiceEqSettings& s);
    void stop();
    bool running() const;

    void setSettings(const VoiceEqSettings& s);    // live edits
    // Muted = silence out, stream kept up (VK playback owns the SignaLink
    // meanwhile; the two must not mix).
    void setMuted(bool m) { muted_ = m; }

    // FreeDV RADE transmit: the mic is encoded to RADE modem audio instead
    // of going through the voice chain (false = no RADE in this build).
    bool setRade(bool on, const QString& callsign);
    bool rade() const { return radeTx_ != nullptr; }
    // MOX released: the end-of-over frame (callsign) + 200 ms silence go
    // out, then silence; radeOverSent() once it has all played into the
    // SignaLink — only then may PTT drop.
    void endOverRade();
    bool radeEnding() const { return radeEnding_; }
    void setRadeLevelDb(float db);                 // TX level trim (<= 0 dB)

    // Meters of the last processed block, for the UI.
    float inputPeakDb() const { return inPeakDb_; }
    // Audio queued between us and the SignaLink, ms (what the operator
    // hears as delay on top of the device latencies).
    double backlogMs() const;
    int droppedBlocks() const { return dropped_; }
    float compGrDb() const { return chain_.compGrDb(); }
    float gateGrDb() const { return chain_.gateGrDb(); }
    float outputPeakDb() const { return outPeakDb_; }  // what goes to the SignaLink
    bool  gateOn() const { return chain_.gateOn(); }
    bool  gateOpen() const { return chain_.gateOpen(); }

signals:
    void failed(const QString& why);
    void radeOverSent();

private:
    void onCaptured();
    qint64 backlogBytes() const;

    QProcess* rec_ = nullptr;
    QProcess* play_ = nullptr;
    VoiceChain chain_;
    QByteArray carry_;                             // partial sample bytes
    std::vector<float> buf_;
    bool muted_ = false;
    bool stopping_ = false;
    float inPeakDb_ = -120.f;
    float outPeakDb_ = -120.f;
    int probeFd_ = -1;                             // read end of pacat's stdin,
                                                   // opened only to measure it
    int dropped_ = 0;
    RadeTx* radeTx_ = nullptr;
    bool radeEnding_ = false, radeDrainArmed_ = false;
    void onCapturedRade(int take);
};

} // namespace ttc
