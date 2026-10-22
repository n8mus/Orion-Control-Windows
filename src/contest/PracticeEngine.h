// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QRandomGenerator>

#include "contest/ContestDef.h"

class QProcess;

namespace ttc {

class CtyLookup;

// PRACTICE mode: a Morse-Runner-style simulated station that answers the
// operator's CQ with a real callsign and a contest-correct exchange, all
// in synthesized audio to the SPEAKERS. The deck routes keyed text here
// instead of the keyer while practice is on.
//
// Isolation is the whole design (the operator's requirement: "does not
// mess with the actual contest operation in any way"):
//   - This class has NO path to the transmitter: it never includes or
//     touches CwWindow / WinKeyer / cwdaemon — it only writes PCM to a
//     playback process. Keep it that way.
//   - It has NO ContestDb dependency: practice QSOs live in counters
//     here and are verified against the sim's own truth, never stored.
//   - It does not read the SDR or inject IQ; the panadapter is
//     untouched.
//
// P1 scope (single caller, adaptive): one station at a time, run-mode
// flow (CQ -> call -> exchange -> TU), copy verified on log, WPM adapts
// RufzXP-style (clean copy speeds the next caller up, a bust slows it
// down). Pileup, QRM/QRN/QSB and LIDs are the next phases.
//
// The clock is deterministic: everything advances in pump(ms) off an
// 8 kHz sample position — live mode drives it with a QTimer, tests call
// pump() directly and assert on the same state machine.
class PracticeEngine : public QObject {
    Q_OBJECT
public:
    explicit PracticeEngine(QObject* parent = nullptr);
    ~PracticeEngine() override;

    // pool = candidate callsigns (master SCP minus already-worked).
    // seed != 0 makes every random choice reproducible (tests).
    void start(const ContestDef* def, const CtyLookup* cty,
               const QStringList& pool, int startWpm, quint32 seed = 0);
    void stop();
    bool active() const { return active_; }

    void setAudioEnabled(bool on) { audioEnabled_ = on; }   // tests: off

    // Everything the operator keys while practicing lands here (the
    // deck's keyText choke point). Plays as sidetone and drives the
    // protocol: CQ summons a caller, his call + exchange answers it,
    // TU closes it, "?"/AGN asks for a repeat.
    void opKeyed(const QString& text, int wpm);
    void abortSending();             // Esc: silence, keep the session

    // The operator pressed log. Compare the entry against what the sim
    // station actually sent, adapt the speed, count the QSO.
    struct Verdict {
        bool haveCaller = false;     // false: nothing live to verify
        bool allGood = false;
        QString call;                // the truth, revealed post-log
        QStringList misses;          // "NAME: sent BILL — you had BULL"
        int qsos = 0, good = 0, streak = 0, wpm = 0;
    };
    Verdict verifyLog(const CQsoValues& typed);

    int callerWpm() const { return wpm_; }
    int qsoCount() const { return qsos_; }
    int practiceSerial() const { return qsos_ + 1; }   // {SENTNR} in practice

    // Test hooks: drive the clock, inspect the sim's truth.
    void pumpForTest(int ms) { pump(ms); }
    QString truthCall() const { return truth_.call; }
    const CQsoValues& truthValues() const { return truth_; }
    bool callerLive() const { return state_ != State::Idle; }

signals:
    // Generic progress for the deck status line.
    void status(const QString& line);
    // What the caller just finished keying — the deck's CW READ pane
    // prints it exactly as the real decoder would print an on-air
    // station (the reader is part of how the operator copies; a silent
    // pane made practice harder than the real thing).
    void callerText(const QString& text);

private:
    enum class State { Idle, CallerCalling, WaitOpReply, CallerExch,
                       WaitOpClose };
    enum class TxKind { OpCq, OpReply, OpTu, OpQuery, OpOther,
                        CallerCall, CallerExch };
    struct Tx {
        QVector<qint16> pcm;
        qint64 start = 0;            // sample position
        TxKind kind = TxKind::OpOther;
        bool reported = false;       // completion handled
    };
    struct Ev { qint64 at; int gen; int what; };   // deferred protocol steps

    void pump(int ms);
    void onTxDone(TxKind k);
    void fireEvent(int what);
    void spawnCaller(int delayMs);
    void queueCallerTx(const QString& text, TxKind kind, int delayMs);
    void queueOpTx(const QString& text, int wpm, TxKind kind);
    CQsoValues makeTruth(const QString& call) const;
    QString callerExchText() const;  // what the caller keys, field order
    TxKind classify(const QString& up) const;
    void adapt(bool good);
    void openAudio();
    void writeAudio(const char* data, qint64 len);
    QVector<qint16> synth(const QString& text, int wpm, double pitchHz,
                          double amp) const;
    int randInt(int lo, int hi);     // inclusive

    const ContestDef* def_ = nullptr;
    const CtyLookup* cty_ = nullptr;
    QStringList pool_;
    bool active_ = false;
    bool audioEnabled_ = true;
    QRandomGenerator rng_;

    State state_ = State::Idle;
    int gen_ = 0;                    // bumps on reset; stale events skip
    CQsoValues truth_;               // what the current caller sends
    QString callerExchKeyed_;        // its exchange as keyed (for repeats)
    double callerPitch_ = 600, callerAmp_ = 0.6;
    int callerWpmActual_ = 25;
    int repeats_ = 0;                // unanswered re-calls so far
    bool opReplied_ = false;         // reply landed while caller still keying
    bool loggedPending_ = false;     // verdict done, next caller on TU end

    int wpm_ = 25;                   // adaptive target, [12,45]
    int qsos_ = 0, good_ = 0, streak_ = 0;

    qint64 pos_ = 0;                 // playback clock, samples @ 8 kHz
    QList<Tx> txs_;
    QList<Ev> evs_;

    QProcess* player_ = nullptr;
    QTimer tick_;
    QElapsedTimer wall_;
    qint64 pumped_ = 0;              // wall ms already pumped
};

} // namespace ttc
