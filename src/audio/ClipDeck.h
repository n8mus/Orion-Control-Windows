// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QProcess>

namespace ttc {

// One-clip-at-a-time WAV recorder/player riding PipeWire's own CLI tools
// (pw-record / pw-play), so no audio library dependency and device routing
// uses the same node names pactl shows. Backs the DVR: off-air RX capture,
// voice-keyer message record, and playback either to the speakers or out
// the radio's sound path (SignaLink) for transmit.
class ClipDeck : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Recording, Playing };
    explicit ClipDeck(QObject* parent = nullptr);
    ~ClipDeck() override;

    State state() const { return state_; }
    // targetNode = PipeWire node name; empty = the system default device.
    bool record(const QString& wavPath, const QString& targetNode);
    bool play(const QString& wavPath, const QString& targetNode);
    void stop();                      // SIGINT so pw-cat finalizes the WAV header

    // First sink/source whose node name contains `match` (case-insensitive,
    // .monitor sources excluded); empty if none. Shells out to pactl once.
    static QString findSink(const QString& match);
    static QString findSource(const QString& match);
    // First capture device that is NOT `avoid` (USB devices preferred) — the
    // system default source is often the radio codec itself, which would make
    // a voice-keyer take record the radio instead of the operator.
    static QString findSourceExcluding(const QString& avoid);
    // Peak-normalize a 16-bit PCM WAV in place so quiet takes (radio line
    // out, timid mic) still drive the rig. Gain is capped so a near-silent
    // file can't be turned into pure noise. The cap is 50x (~34 dB): the
    // SignaLink RX feed sits deliberately low (~-37 dBFS peak, calibrated for
    // WSJT-X's noise floor — do NOT raise the RX knob), so off-air retransmit
    // takes need more than the old 26 dB to reach full drive; a mic keyer take
    // is far hotter and hits targetPeak long before the cap.
    static bool normalizeWav(const QString& wavPath, double targetPeak = 0.7,
                             double maxGain = 50.0);

signals:
    void finished();                  // deck is idle again (ended or stopped)
    void failed(const QString& why);

private:
    bool launch(const QString& exe, const QStringList& args, State s);
    QProcess proc_;
    State state_ = State::Idle;
    bool stopping_ = false;           // our SIGINT, not a real error
};

} // namespace ttc
