// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/SkimmerEngine.h"
#include "cw/CwDecoder.h"

#include <QDateTime>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ttc {

namespace {
// US CW/data segments (where a skimmer is worth pointing). Wider band
// edges live in Bands.h; these are the slices where straight CW happens.
struct Seg { qint64 lo, hi; };
const Seg kCwSegs[] = {
    {1800000, 1840000},   {3500000, 3600000},   {7000000, 7125000},
    {10100000, 10130000}, {14000000, 14150000}, {18068000, 18110000},
    {21000000, 21200000}, {24890000, 24930000}, {28000000, 28300000},
    {50050000, 50100000},
};

// Callsign shape: 1-2 letter (or letter-digit) prefix, area digit(s),
// 1-4 letter suffix. Deliberately loose — the cty.dat validator and the
// seen-twice/after-DE rule do the real filtering.
const QRegularExpression& callRe() {
    static const QRegularExpression re(
        QStringLiteral("^(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])[0-9]{1,2}"
                       "[A-Z]{1,4}$"));
    return re;
}
} // namespace

SkimmerEngine::SkimmerEngine(double inputRate, int channels, QObject* parent)
    : QObject(parent), inputRate_(inputRate) {
    ch_.resize(std::clamp(channels, 1, 64));
    for (int i = 0; i < int(ch_.size()); ++i) {
        ch_[i].dec = new CwDecoder(inputRate, 0.0, this);
        connect(ch_[i].dec, &CwDecoder::textDecoded, this,
                [this, i](const QString& t) { onText(i, t); },
                Qt::QueuedConnection);
        connect(ch_[i].dec, &CwDecoder::wpmEstimated, this,
                [this, i](int wpm) { ch_[i].wpm = wpm; },
                Qt::QueuedConnection);
    }
}

void SkimmerEngine::setEnabled(bool on) {
    enabled_.store(on, std::memory_order_relaxed);
    if (!on)
        for (auto& c : ch_) freeChannel(c);
}

void SkimmerEngine::processIq(const std::complex<float>* d, size_t n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    for (auto& c : ch_) c.dec->processIq(d, n);   // each gated by its atomic
}

bool SkimmerEngine::cwSegment(qint64 dialHz, qint64& lo, qint64& hi) {
    for (const Seg& s : kCwSegs)
        if (dialHz >= s.lo - 50000 && dialHz <= s.hi + 50000) {
            lo = s.lo;
            hi = s.hi;
            return true;
        }
    return false;
}

void SkimmerEngine::updateFromSpectrum(const std::vector<float>& db,
                                       int spanHz, qint64 dialHz,
                                       int loOffsetHz) {
    if (!enabled()) return;
    const int n = static_cast<int>(db.size());
    if (n < 64 || spanHz <= 0) return;
    const double binHz = double(spanHz) / n;
    const qint64 loAbs = dialHz + loOffsetHz;      // bin n/2

    // Dial moved: re-express every surviving channel relative to the new
    // LO; channels that slid out of the capture are freed.
    if (dialHz != lastDial_) {
        lastDial_ = dialHz;
        for (auto& c : ch_) {
            if (!c.active) continue;
            const qint64 off = c.hz - loAbs;
            if (std::llabs(off) > spanHz / 2 - 2000) freeChannel(c);
            else c.dec->retune(double(off));
        }
    }

    qint64 segLo = 0, segHi = 0;
    if (!cwSegment(dialHz, segLo, segHi)) {
        // Not parked on a CW segment — let existing channels finish but
        // assign nothing new.
        segLo = segHi = 0;
    }

    // Expire channels: 90 s without a decoded character means the station
    // is gone (or was never real). A channel still trickling junk after
    // 45 s (mostly E/T/*, no call) is parked on noise the consistency
    // squelch couldn't fully silence — free it for a real peak.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto& c : ch_) {
        if (!c.active) continue;
        const qint64 idleSince = std::max(c.lastCharMs, c.assignedMs);
        if (nowMs - idleSince > 90000) { freeChannel(c); continue; }
        if (c.call.isEmpty() && nowMs - c.assignedMs > 45000
            && c.text.size() > 8) {
            int junk = 0;
            for (const QChar ch : c.text)
                if (ch == 'E' || ch == 'T' || ch == ' ' || ch == '*'
                    || ch == 'I')
                    ++junk;
            if (junk * 10 >= c.text.size() * 8) freeChannel(c);
        }
    }

    if (segLo == 0) return;
    // Bin range covering (CW segment) ∩ (capture, minus edge margins).
    const auto binOf = [&](qint64 absHz) {
        return n / 2 + int(std::lround(double(absHz - loAbs) / binHz));
    };
    int i0 = std::max(binOf(segLo), 32);
    int i1 = std::min(binOf(segHi), n - 33);
    if (i1 - i0 < 32) return;

    // Average the spectrum across assignment passes: single-frame noise
    // spikes look exactly like weak carriers (replay-found: half the bank
    // parked on QRM), a 3-frame EMA keeps only what persists.
    if (int(specAvg_.size()) != n) specAvg_.assign(db.begin(), db.end());
    else
        for (int i = 0; i < n; ++i)
            specAvg_[i] += 0.4f * (db[i] - specAvg_[i]);

    // Noise floor: median of the segment.
    std::vector<float> sorted(specAvg_.begin() + i0,
                              specAvg_.begin() + i1 + 1);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2,
                     sorted.end());
    const float floorDb = sorted[sorted.size() / 2];

    // Peaks: local max over ±3 bins, well above the floor, away from the
    // LO's DC artifact.
    struct Peak { int bin; float db; };
    std::vector<Peak> peaks;
    const std::vector<float>& sp = specAvg_;
    for (int i = i0; i <= i1; ++i) {
        if (sp[i] < floorDb + 15.0f) continue;
        if (std::abs(i - n / 2) * binHz < 1500.0) continue;   // DC hump
        bool best = true;
        for (int k = -3; k <= 3 && best; ++k)
            if (k != 0 && sp[i + k] > sp[i]) best = false;
        if (best) peaks.push_back({i, sp[i]});
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const Peak& a, const Peak& b) { return a.db > b.db; });

    for (const Peak& pk : peaks) {
        // Parabolic interpolation on the peak and its neighbors: the true
        // carrier is rarely at a bin center, and ±30 Hz of assignment
        // error is exactly the kind of thing AFC then has to claw back.
        const double ym = specAvg_[pk.bin - 1], y0 = specAvg_[pk.bin],
                     yp = specAvg_[pk.bin + 1];
        const double den = ym - 2.0 * y0 + yp;
        const double d =
            (std::abs(den) > 1e-9) ? std::clamp(0.5 * (ym - yp) / den,
                                                -0.5, 0.5) : 0.0;
        const qint64 hz = loAbs
            + qint64(std::lround((pk.bin - n / 2 + d) * binHz));
        // Already covered? (Assigned channels hold their frequency; CW
        // stations don't wander.)
        bool covered = false;
        for (const auto& c : ch_)
            if (c.active && std::llabs(c.hz - hz) < 150) covered = true;
        if (covered) continue;
        Chan* free = nullptr;
        for (auto& c : ch_)
            if (!c.active) { free = &c; break; }
        if (!free) break;                          // bank is full
        free->active = true;
        free->hz = hz;
        free->wpm = 0;
        free->call.clear();
        free->text.clear();
        free->lastCharMs = 0;
        free->assignedMs = nowMs;
        free->dec->retune(double(hz - loAbs));
        free->dec->setEnabled(true);
        if (std::getenv("TTC_SKIMDBG"))
            fprintf(stderr, "[skim] ch -> %.1f kHz (%.0f dB peak)\n",
                    hz / 1000.0, double(pk.db));
    }
}

void SkimmerEngine::freeChannel(Chan& c) {
    c.dec->setEnabled(false);
    c.active = false;
    c.call.clear();
    c.text.clear();
    c.wpm = 0;
}

void SkimmerEngine::onText(int idx, const QString& t) {
    Chan& c = ch_[idx];
    if (!c.active) return;                         // late queued delivery
    c.text += t;
    if (c.text.size() > 160) c.text = c.text.right(120);
    c.lastCharMs = QDateTime::currentMSecsSinceEpoch();
    if (std::getenv("TTC_SKIMDBG"))
        fprintf(stderr, "[skim] %.1f kHz: %s\n", c.hz / 1000.0,
                qPrintable(c.text.right(40)));
    extractCall(c);
}

void SkimmerEngine::extractCall(Chan& c) {
    // Mine the rolling buffer: a token is a call if it has callsign shape,
    // survives the cty.dat check, and either follows "DE " (CQ CQ DE X1YZ)
    // or shows up twice — one clean copy could still be a decode artifact.
    // Only COMPLETED words count: the token still being received is a
    // prefix of the real call ("W1A" mid-"W1AW") and would spot as a
    // phantom, so it waits until its word gap arrives.
    QStringList toks = c.text.split(' ', Qt::SkipEmptyParts);
    if (!c.text.endsWith(' ') && !toks.isEmpty()) toks.removeLast();
    for (int i = toks.size() - 1; i >= 0; --i) {
        const QString& tok = toks[i];
        if (tok.contains('*')) continue;           // unreadable char inside
        if (tok.size() < 3 || tok.size() > 8) continue;
        if (!callRe().match(tok).hasMatch()) continue;
        if (validate_ && !validate_(tok)) continue;
        const bool afterDe = i > 0 && toks[i - 1] == QLatin1String("DE");
        int count = 0;
        for (const QString& u : toks)
            if (u == toks[i]) ++count;
        if (!afterDe && count < 2) continue;
        // Duplicate listener: a strong station's keying sidebands raise
        // peaks of their own, and several channels end up copying the SAME
        // station (replay-found: one CQ, three channels, spot frequency
        // thrashing between them). Channels are assigned strongest-first,
        // so the first holder has the true carrier — later ones fold.
        for (auto& other : ch_)
            if (&other != &c && other.active && other.call == tok) {
                freeChannel(c);
                return;
            }
        if (tok == c.call) {                       // same station, refresh
            for (auto& s : spots_)
                if (s.call == tok) {
                    s.atSecs = QDateTime::currentSecsSinceEpoch();
                    s.wpm = c.wpm;
                }
            // Re-announce so downstream consumers (telnet clients, the
            // band map's age column) see the station is still active;
            // they throttle their own output.
            emit spotFound(tok, c.hz, c.wpm);
            emit spotsChanged();
            return;
        }
        c.call = tok;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        bool known = false;
        for (auto& s : spots_)
            if (s.call == tok) {
                s.hz = c.hz;
                s.atSecs = now;
                s.wpm = c.wpm;
                known = true;
            }
        if (!known) spots_.push_back({tok, c.hz, now, c.wpm});
        emit spotFound(tok, c.hz, c.wpm);
        emit spotsChanged();
        return;
    }
}

const QVector<SkimmerEngine::SkimSpot>& SkimmerEngine::spots() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    spots_.erase(std::remove_if(spots_.begin(), spots_.end(),
                                [now](const SkimSpot& s) {
                                    return now - s.atSecs > 600;
                                }),
                 spots_.end());
    return spots_;
}

QVector<SkimmerEngine::ChanInfo> SkimmerEngine::channelInfo() const {
    QVector<ChanInfo> out;
    out.reserve(int(ch_.size()));
    for (const auto& c : ch_)
        out.push_back({c.active, c.hz, c.wpm, c.call,
                       c.text.right(28)});
    return out;
}

} // namespace ttc
