// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/PracticeEngine.h"

#include <QProcess>
#include <cmath>

#include "contest/ContestEngine.h"   // nearMissCall (pure functions only)
#include "util/CtyLookup.h"

namespace ttc {

namespace {
constexpr int kRate = 8000;          // synth/playback sample rate
constexpr int kWpmFloor = 12, kWpmCeil = 45;

// Text -> elements. Self-contained on purpose: the decoder's table maps
// the other direction and lives behind its own file — sharing it would
// couple the sim to the decode path for zero gain.
const QHash<QChar, QByteArray>& morse() {
    static const QHash<QChar, QByteArray> m = {
        {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
        {'E', "."},    {'F', "..-."}, {'G', "--."},  {'H', "...."},
        {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
        {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
        {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
        {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
        {'Y', "-.--"}, {'Z', "--.."},
        {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
        {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
        {'8', "---.."}, {'9', "----."},
        {'/', "-..-."}, {'?', "..--.."}, {'.', ".-.-.-"}, {',', "--..--"},
        {'=', "-...-"},
    };
    return m;
}

const QStringList& namePool() {
    static const QStringList n = {
        "BILL", "STEVE", "JIM", "DAVE", "RON", "GEO", "AL", "ED",
        "JOHN", "MIKE", "BOB", "DAN", "RICH", "KEN", "TOM", "ART",
        "HANK", "JACK", "GARY", "SAM"};
    return n;
}
const QStringList& statePool() {
    static const QStringList s = {
        "OH", "MI", "TX", "FL", "CA", "NY", "PA", "IL", "VA", "NC",
        "GA", "TN", "MO", "WI", "MN", "CO", "AZ", "WA", "OR", "NJ"};
    return s;
}
const QStringList& sectPool() {
    static const QStringList s = {
        "OH", "MI", "EPA", "STX", "SFL", "ENY", "IL", "VA", "NC",
        "GA", "TN", "MO", "WI", "MN", "CO", "AZ", "WWA", "OR", "SNJ"};
    return s;
}

// Copy comparison: case-blind, cut numbers unrolled (N->9, T->0 — the
// sim doesn't send cut numbers yet, but the operator may TYPE them).
QString normCopy(QString s) {
    s = s.trimmed().toUpper();
    QString digits = s;
    digits.replace('N', '9').replace('T', '0');
    bool num = !digits.isEmpty();
    for (QChar c : digits)
        if (!c.isDigit()) { num = false; break; }
    if (num) {
        while (digits.size() > 1 && digits.startsWith('0'))
            digits.remove(0, 1);
        return digits;
    }
    return s;
}

// A token that could be a callsign (letters+digit, plausible length).
bool callish(const QString& t) {
    if (t.size() < 3 || t.size() > 10) return false;
    bool letter = false, digit = false;
    for (QChar c : t) {
        if (c.isLetter()) letter = true;
        else if (c.isDigit()) digit = true;
        else return false;
    }
    return letter && digit;
}
} // namespace

PracticeEngine::PracticeEngine(QObject* parent) : QObject(parent) {
    tick_.setInterval(40);
    connect(&tick_, &QTimer::timeout, this, [this] {
        const qint64 now = wall_.elapsed();
        pump(int(now - pumped_));
        pumped_ = now;
    });
}

PracticeEngine::~PracticeEngine() { stop(); }

void PracticeEngine::start(const ContestDef* def, const CtyLookup* cty,
                           const QStringList& pool, int startWpm,
                           quint32 seed) {
    stop();
    def_ = def;
    cty_ = cty;
    pool_.clear();
    for (const QString& c : pool)
        if (callish(c.trimmed().toUpper()))
            pool_.push_back(c.trimmed().toUpper());
    if (pool_.isEmpty())
        pool_ = {"N5OT", "K4RUM", "W1UJ", "VE3KI", "AA3B", "K3WW",
                 "N4YDU", "W9RE", "K5ZD", "N2NT", "K1DG", "W4NZ"};
    rng_ = seed ? QRandomGenerator(seed) : QRandomGenerator(
                      QRandomGenerator::global()->generate());
    wpm_ = std::clamp(startWpm, kWpmFloor, kWpmCeil);
    qsos_ = good_ = streak_ = 0;
    pos_ = 0;
    pumped_ = 0;
    gen_ = 0;
    state_ = State::Idle;
    truth_ = {};
    loggedPending_ = false;
    txs_.clear();
    evs_.clear();
    active_ = true;
    if (audioEnabled_) {
        openAudio();
        // A head start of silence so the first dits aren't eaten by
        // stream start-up.
        const QByteArray lead(kRate / 5 * 2, 0);
        writeAudio(lead.constData(), lead.size());
    }
    wall_.start();
    tick_.start();
    emit status("PRACTICE — send CQ to bring a caller");
}

void PracticeEngine::stop() {
    active_ = false;
    tick_.stop();
    if (player_) {
        player_->closeWriteChannel();
        player_->waitForFinished(300);
        player_->kill();
        player_->deleteLater();
        player_ = nullptr;
    }
    txs_.clear();
    evs_.clear();
    state_ = State::Idle;
}

void PracticeEngine::abortSending() {
    // Esc: silence everything queued but keep the session alive. The
    // caller's protocol also resets — dead air after an abort, exactly
    // like dumping the keyer mid-QSO on the air.
    txs_.clear();
    evs_.clear();
    gen_++;
    state_ = State::Idle;
    loggedPending_ = false;
    emit status("stopped — CQ to bring the next caller");
}

// ---- operator input -------------------------------------------------------

PracticeEngine::TxKind PracticeEngine::classify(const QString& up) const {
    const QStringList toks = up.split(' ', Qt::SkipEmptyParts);
    if (state_ != State::Idle && !truth_.call.isEmpty()
        && up.contains(truth_.call))
        return TxKind::OpReply;
    if (toks.contains("CQ")) return TxKind::OpCq;
    if (toks.contains("TU") || toks.contains("73") || toks.contains("EE"))
        return TxKind::OpTu;
    if (up.contains('?') || toks.contains("AGN")) return TxKind::OpQuery;
    // A call-shaped token that is NOT the caller = busted copy on the
    // air; the station reacts by repeating its call.
    if (state_ != State::Idle)
        for (const QString& t : toks)
            if (callish(t)) return TxKind::OpQuery;
    return TxKind::OpOther;
}

void PracticeEngine::opKeyed(const QString& text, int wpm) {
    if (!active_) return;
    const QString up = text.trimmed().toUpper();
    if (up.isEmpty()) return;
    // The op is transmitting: a waiting caller holds instead of
    // re-calling over him (kill the pending patience re-call).
    if (state_ == State::WaitOpReply)
        for (int i = evs_.size() - 1; i >= 0; --i)
            if (evs_[i].what == 5) evs_.removeAt(i);
    queueOpTx(up, std::clamp(wpm, 5, 60), classify(up));
}

void PracticeEngine::queueOpTx(const QString& text, int wpm, TxKind kind) {
    // Op transmissions queue back-to-back like a keyer buffer.
    qint64 start = pos_;
    for (const Tx& t : txs_)
        if (t.kind == TxKind::OpCq || t.kind == TxKind::OpReply
            || t.kind == TxKind::OpTu || t.kind == TxKind::OpOther
            || t.kind == TxKind::OpQuery)
            start = std::max(start, t.start + t.pcm.size());
    Tx tx;
    tx.pcm = synth(text, wpm, 600.0, 0.6);
    tx.start = start;
    tx.kind = kind;
    txs_.push_back(tx);
}

// ---- the simulated station ------------------------------------------------

void PracticeEngine::spawnCaller(int delayMs) {
    QString call = pool_.at(randInt(0, pool_.size() - 1));
    if (pool_.size() > 1)
        while (call == truth_.call)
            call = pool_.at(randInt(0, pool_.size() - 1));
    truth_ = makeTruth(call);
    callerExchKeyed_ = callerExchText();
    callerWpmActual_ = std::clamp(wpm_ + randInt(-2, 3), kWpmFloor, 50);
    callerPitch_ = randInt(450, 850);
    callerAmp_ = randInt(40, 95) / 100.0;
    repeats_ = 0;
    opReplied_ = false;
    queueCallerTx(truth_.call, TxKind::CallerCall, delayMs);
    emit status("a station answers — copy the call");
}

void PracticeEngine::queueCallerTx(const QString& text, TxKind kind,
                                   int delayMs) {
    Tx tx;
    tx.pcm = synth(text, callerWpmActual_, callerPitch_, callerAmp_);
    tx.start = pos_ + qint64(delayMs) * (kRate / 1000);
    tx.kind = kind;
    txs_.push_back(tx);
    state_ = kind == TxKind::CallerCall ? State::CallerCalling
                                        : State::CallerExch;
}

void PracticeEngine::onTxDone(TxKind k) {
    switch (k) {
        case TxKind::OpCq:
            // CQ always resets: a caller mid-QSO that hears you CQ has
            // been abandoned. Fresh station after a listening pause.
            gen_++;
            state_ = State::Idle;
            evs_.push_back({pos_ + qint64(randInt(300, 900)) * 8, gen_, 1});
            break;
        case TxKind::OpReply:
            if (state_ == State::WaitOpReply)
                evs_.push_back(
                    {pos_ + qint64(randInt(250, 600)) * 8, gen_, 2});
            else if (state_ == State::CallerCalling)
                opReplied_ = true;   // tail-ended; exch when its call ends
            break;
        case TxKind::OpQuery:
            if (state_ == State::WaitOpReply)
                evs_.push_back({pos_ + 250 * 8, gen_, 3});
            else if (state_ == State::WaitOpClose)
                evs_.push_back({pos_ + 250 * 8, gen_, 2});
            break;
        case TxKind::OpTu:
            if (loggedPending_) {
                loggedPending_ = false;
                evs_.push_back(
                    {pos_ + qint64(randInt(1000, 2500)) * 8, gen_, 4});
            } else if (state_ == State::WaitOpClose) {
                // TU without logging: the QSO evaporates unscored.
                gen_++;
                state_ = State::Idle;
                emit status("closed without logging — no score");
            }
            break;
        case TxKind::OpOther:
            break;
        case TxKind::CallerCall:
            emit callerText(truth_.call);
            state_ = State::WaitOpReply;
            if (opReplied_) {
                opReplied_ = false;
                evs_.push_back(
                    {pos_ + qint64(randInt(250, 600)) * 8, gen_, 2});
            } else {
                evs_.push_back(
                    {pos_ + qint64(randInt(6000, 8000)) * 8, gen_, 5});
            }
            break;
        case TxKind::CallerExch:
            emit callerText(callerExchKeyed_);
            state_ = State::WaitOpClose;
            // Patient: the op is typing what it just heard. 30 s of
            // silence before it moves on (12 s expired mid-entry).
            evs_.push_back({pos_ + 30000LL * 8, gen_, 6});
            break;
    }
}

void PracticeEngine::fireEvent(int what) {
    switch (what) {
        case 1:                              // fresh caller keys its call
            if (state_ == State::Idle) spawnCaller(0);
            break;
        case 2:                              // caller sends its exchange
            if (state_ == State::WaitOpReply
                || state_ == State::WaitOpClose)
                queueCallerTx(callerExchKeyed_, TxKind::CallerExch, 0);
            break;
        case 3:                              // repeat the call
            if (state_ == State::WaitOpReply)
                queueCallerTx(truth_.call, TxKind::CallerCall, 0);
            break;
        case 4:                              // next caller (post TU/log)
        case 7:
            loggedPending_ = false;
            if (state_ == State::Idle) spawnCaller(randInt(200, 600));
            break;
        case 5:                              // patience: op never replied
            if (state_ == State::WaitOpReply && !opReplied_) {
                if (repeats_ < 2) {
                    repeats_++;
                    queueCallerTx(truth_.call, TxKind::CallerCall, 0);
                } else {
                    gen_++;
                    state_ = State::Idle;
                    emit status("the caller gave up — CQ again");
                }
            }
            break;
        case 6:                              // op never closed/logged
            if (state_ == State::WaitOpClose) {
                gen_++;
                state_ = State::Idle;
                emit status("the caller moved on — CQ again");
            }
            break;
    }
}

// ---- truth ---------------------------------------------------------------

CQsoValues PracticeEngine::makeTruth(const QString& call) const {
    CQsoValues v;
    v.call = call;
    if (def_ && def_->hasRst) v.rstR = "599";
    if (!def_) return v;
    auto* self = const_cast<PracticeEngine*>(this);
    CtyInfo ci;
    const bool ctyOk = cty_ && cty_->info(call, ci);
    for (const ExchFieldDef& f : def_->fields) {
        const QString lbl = f.label.toUpper();
        QString val;
        if (f.col == ExchCol::RstR) {
            val = "599";
        } else if (f.col == ExchCol::SerialR) {
            val = QString::number(self->randInt(1, 300));
        } else if (f.verify == QLatin1String("cqz")) {
            val = QString::number(ctyOk && ci.cq > 0 ? ci.cq : 14);
        } else if (f.verify == QLatin1String("ituz")) {
            val = QString::number(ctyOk && ci.itu > 0 ? ci.itu : 28);
        } else if (f.historyCol == QLatin1String("name")
                   || lbl.contains("NAME")) {
            val = namePool().at(self->randInt(0, namePool().size() - 1));
        } else if (lbl.contains("STATE")) {
            // CWT's "NR / STATE": members send numbers, the rest a state.
            val = lbl.contains("NR") && self->randInt(0, 9) < 6
                      ? QString::number(self->randInt(1, 3000))
                      : statePool().at(
                            self->randInt(0, statePool().size() - 1));
        } else if (lbl.contains("SECT")) {
            val = sectPool().at(self->randInt(0, sectPool().size() - 1));
        } else if (lbl.contains("CK")) {
            val = QString("%1").arg(self->randInt(50, 99), 2, 10,
                                    QChar('0'));
        } else if (lbl.contains("PREC")) {
            static const char* p[] = {"A", "B", "Q", "M", "U", "S"};
            val = p[self->randInt(0, 5)];
        } else if (lbl.contains("AGE")) {
            val = QString::number(self->randInt(15, 80));
        } else if (lbl.contains("PWR") || lbl.contains("POWER")) {
            static const char* p[] = {"5", "100", "100", "500", "KW"};
            val = p[self->randInt(0, 4)];
        } else if (lbl.contains("GRID")) {
            static const char* g[] = {"EN83", "FN20", "EM12", "DM52",
                                      "CN87", "EN52"};
            val = g[self->randInt(0, 5)];
        } else {
            val = QString::number(self->randInt(1, 999));
        }
        switch (f.col) {
            case ExchCol::RstR:    v.rstR = val; break;
            case ExchCol::SerialR: v.serialR = val; break;
            case ExchCol::Exch1:   v.exch1 = val; break;
            case ExchCol::Exch2:   v.exch2 = val; break;
            case ExchCol::Exch3:   v.exch3 = val; break;
        }
    }
    return v;
}

QString PracticeEngine::callerExchText() const {
    QStringList out;
    bool rstDone = false;
    if (def_)
        for (const ExchFieldDef& f : def_->fields) {
            switch (f.col) {
                case ExchCol::RstR:    out << "5NN"; rstDone = true; break;
                case ExchCol::SerialR: out << truth_.serialR; break;
                case ExchCol::Exch1:   out << truth_.exch1; break;
                case ExchCol::Exch2:   out << truth_.exch2; break;
                case ExchCol::Exch3:   out << truth_.exch3; break;
            }
        }
    if (def_ && def_->hasRst && !rstDone) out.prepend("5NN");
    out.removeAll(QString());
    return out.join(' ');
}

// ---- verdict --------------------------------------------------------------

PracticeEngine::Verdict PracticeEngine::verifyLog(const CQsoValues& typed) {
    Verdict out;
    out.wpm = wpm_;
    out.qsos = qsos_;
    out.good = good_;
    out.streak = streak_;
    if (!active_ || state_ == State::Idle || truth_.call.isEmpty())
        return out;                          // nothing live to verify
    out.haveCaller = true;
    out.call = truth_.call;

    const auto miss = [&out](const QString& what, const QString& sent,
                             const QString& had) {
        out.misses << QString("%1: sent %2 — you had %3")
                          .arg(what, sent, had.isEmpty() ? "nothing" : had);
    };
    if (normCopy(typed.call) != normCopy(truth_.call))
        miss("CALL", truth_.call, typed.call);
    if (def_)
        for (const ExchFieldDef& f : def_->fields) {
            QString sent, had;
            switch (f.col) {
                case ExchCol::RstR:
                    sent = truth_.rstR; had = typed.rstR; break;
                case ExchCol::SerialR:
                    sent = truth_.serialR; had = typed.serialR; break;
                case ExchCol::Exch1:
                    sent = truth_.exch1; had = typed.exch1; break;
                case ExchCol::Exch2:
                    sent = truth_.exch2; had = typed.exch2; break;
                case ExchCol::Exch3:
                    sent = truth_.exch3; had = typed.exch3; break;
            }
            if (sent.isEmpty()) continue;
            if (normCopy(had) != normCopy(sent)) miss(f.label, sent, had);
        }

    out.allGood = out.misses.isEmpty();
    qsos_++;
    if (out.allGood) {
        good_++;
        streak_++;
    } else {
        streak_ = 0;
    }
    adapt(out.allGood);
    out.qsos = qsos_;
    out.good = good_;
    out.streak = streak_;
    out.wpm = wpm_;

    // This caller is done. The next one arrives when the op's TU
    // finishes — or after a fallback pause if no TU ever goes out.
    gen_++;
    state_ = State::Idle;
    loggedPending_ = true;
    evs_.push_back({pos_ + 5000LL * 8, gen_, 7});
    return out;
}

void PracticeEngine::adapt(bool good) {
    // RufzXP's ladder: clean copy nudges the next caller faster, a bust
    // backs off harder — the drill stays pinned at the ceiling.
    wpm_ = std::clamp(good ? wpm_ + 1 : wpm_ - 2, kWpmFloor, kWpmCeil);
}

// ---- clock + audio --------------------------------------------------------

void PracticeEngine::pump(int ms) {
    if (!active_ || ms <= 0) return;
    const qint64 target = pos_ + qint64(ms) * (kRate / 1000);

    if (audioEnabled_ && player_) {
        const int n = int(target - pos_);
        QByteArray buf(n * 2, 0);
        auto* out = reinterpret_cast<qint16*>(buf.data());
        // Cheap LCG noise floor — NOT rng_, so the protocol's random
        // sequence is identical with audio on or off (test parity).
        static quint32 lcg = 0x2545F491u;
        for (int i = 0; i < n; ++i) {
            const qint64 p = pos_ + i;
            qint32 acc = 0;
            for (const Tx& t : txs_) {
                const qint64 off = p - t.start;
                if (off >= 0 && off < t.pcm.size())
                    acc += t.pcm[int(off)];
            }
            lcg = lcg * 1664525u + 1013904223u;
            acc += int(lcg >> 22) - 512;     // ±512: a faint band hiss
            out[i] = qint16(std::clamp(acc, -32767, 32767));
        }
        writeAudio(buf.constData(), buf.size());
    }

    pos_ = target;

    for (Tx& t : txs_)
        if (!t.reported && t.start + t.pcm.size() <= pos_) {
            t.reported = true;
            onTxDone(t.kind);
        }
    for (int i = txs_.size() - 1; i >= 0; --i)
        if (txs_[i].reported) txs_.removeAt(i);

    for (int i = 0; i < evs_.size();) {
        if (evs_[i].at <= pos_) {
            const Ev e = evs_.takeAt(i);
            if (e.gen == gen_) fireEvent(e.what);
        } else {
            ++i;
        }
    }
}

QVector<qint16> PracticeEngine::synth(const QString& text, int wpm,
                                      double pitchHz, double amp) const {
    const int dit = int(std::lround(double(kRate) * 1.2 / wpm));
    const int ramp = std::min(dit / 3, kRate * 4 / 1000);   // ≤4 ms cosine
    QVector<qint16> out;
    out.reserve(text.size() * dit * 10);
    const double w = 2.0 * M_PI * pitchHz / kRate;
    qint64 phase = 0;
    const auto tone = [&](int n) {
        for (int i = 0; i < n; ++i) {
            double env = 1.0;
            if (i < ramp) env = 0.5 - 0.5 * std::cos(M_PI * i / ramp);
            else if (i >= n - ramp)
                env = 0.5 - 0.5 * std::cos(M_PI * (n - 1 - i) / ramp);
            out.push_back(qint16(std::lround(
                std::sin(w * double(phase++)) * env * amp * 30000.0)));
        }
    };
    const auto gap = [&](int n) { out.resize(out.size() + n); phase += n; };
    for (const QChar ch : text.toUpper()) {
        if (ch == ' ') {
            gap(4 * dit);                    // 3 from char end + 4 = 7
            continue;
        }
        const auto it = morse().constFind(ch);
        if (it == morse().constEnd()) continue;
        for (const char e : *it) {
            tone(e == '-' ? 3 * dit : dit);
            gap(dit);
        }
        gap(2 * dit);                        // char spacing: 1 + 2 = 3
    }
    return out;
}

void PracticeEngine::openAudio() {
    // Same stack as RipAudio: pulse layer first (pacat -> pipewire-pulse;
    // the native pw-play path can wedge silently after a pipewire
    // restart), pw-play fallback. Raw headerless s16 mono.
    player_ = new QProcess(this);
    player_->start("pacat", {"--playback", "--raw", "--format=s16le",
                             "--rate=8000", "--channels=1"});
    if (!player_->waitForStarted(1500)) {
        player_->deleteLater();
        player_ = new QProcess(this);
        player_->start("pw-play", {"--raw", "--format=s16", "--rate=8000",
                                   "--channels=1", "-"});
        if (!player_->waitForStarted(1500)) {
            player_->deleteLater();
            player_ = nullptr;
            emit status("no audio stack (pacat/pw-play) — silent run");
        }
    }
}

void PracticeEngine::writeAudio(const char* data, qint64 len) {
    if (player_) player_->write(data, len);
}

int PracticeEngine::randInt(int lo, int hi) {
    return int(rng_.bounded(quint32(hi - lo + 1))) + lo;
}

} // namespace ttc
