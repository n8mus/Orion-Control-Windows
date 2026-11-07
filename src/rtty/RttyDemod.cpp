// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/RttyDemod.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ttc {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

RttyDemod::RttyDemod() : RttyDemod(Params{}) {}
RttyDemod::RttyDemod(const Params& p) { setParams(p); }

void RttyDemod::setParams(const Params& p) {
    p_ = p;
    ita_.setUsos(p.usos);
    decim_ = std::max(1, int(std::lround(p.sampleRate / 1000.0)));
    const double rate1k = p.sampleRate / decim_;
    bitLen_ = rate1k / p.baud;
    mHz_ = p.reverse ? p.spaceHz : p.markHz;
    sHz_ = p.reverse ? p.markHz : p.spaceHz;
    afcHz_ = 0.0;
    setRotations();
    const size_t L = std::max<size_t>(2, size_t(std::lround(bitLen_)));
    ringM_.assign(L, {0, 0});
    ringS_.assign(L, {0, 0});
    afcDecim_ = std::max(1, int(std::lround(p.sampleRate / 8000.0)));
    const double afcRate = p.sampleRate / afcDecim_;
    afcRing_.assign(size_t(std::lround(afcRate * 0.256)), {0, 0});
    afcWin_.resize(afcRing_.size());
    for (size_t i = 0; i < afcWin_.size(); ++i)
        afcWin_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * (i + 0.5) / afcWin_.size());
    reset();
}

void RttyDemod::setRotations() {
    rotM_ = std::polar(1.0, -2.0 * kPi * (mHz_ + afcHz_) / p_.sampleRate);
    rotS_ = std::polar(1.0, -2.0 * kPi * (sHz_ + afcHz_) / p_.sampleRate);
}

void RttyDemod::resetAfc() {
    afcHz_ = 0.0;
    setRotations();
    afcFill_ = 0;
    afcTrack_ = false;
    afcMiss_ = 0;
    st_.afcHz = 0.0f;
}

void RttyDemod::reset() {
    phM_ = phS_ = {1, 0};
    accM_ = accS_ = {0, 0};
    accN_ = 0;
    std::fill(ringM_.begin(), ringM_.end(), std::complex<double>{0, 0});
    std::fill(ringS_.begin(), ringS_.end(), std::complex<double>{0, 0});
    sumM_ = sumS_ = {0, 0};
    ringPos_ = 0;
    pkM_ = pkS_ = floor_ = 1e-3;
    quality_ = 0.0;
    warm_ = 0;
    uart_ = Uart::Idle;
    t_ = 0;
    dataBits_ = 0;
    code_ = 0;
    lastBit_ = 1;
    ita_.reset();
    st_ = {};
    std::fill(afcRing_.begin(), afcRing_.end(), std::complex<double>{0, 0});
    afcPos_ = 0;
    afcFill_ = afcTick_ = 0;
    afcAcc_ = {0, 0};
    afcAccN_ = 0;
    st_.afcHz = float(afcHz_);
}

void RttyDemod::processReal(const float* s, size_t n) {
    // Small stack batches: no per-call allocation on the audio thread.
    std::complex<float> buf[256];
    while (n) {
        const size_t k = std::min<size_t>(n, 256);
        for (size_t i = 0; i < k; ++i) buf[i] = {s[i], 0.0f};
        process(buf, k);
        s += k;
        n -= k;
    }
}

void RttyDemod::process(const std::complex<float>* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const std::complex<double> x(s[i].real(), s[i].imag());
        accM_ += x * phM_;
        accS_ += x * phS_;
        if (p_.afc && !afcRing_.empty()) {
            afcAcc_ += x * phM_;
            if (++afcAccN_ >= afcDecim_) {
                afcRing_[afcPos_] = afcAcc_;
                if (++afcPos_ >= afcRing_.size()) afcPos_ = 0;
                if (afcFill_ < afcRing_.size()) ++afcFill_;
                afcAcc_ = {0, 0};
                afcAccN_ = 0;
            }
        }
        phM_ *= rotM_;
        phS_ *= rotS_;
        if (++accN_ >= decim_) {
            // Re-normalise the phasors now and then (recurrence drift).
            phM_ /= std::abs(phM_);
            phS_ /= std::abs(phS_);
            // Boxcar over one bit at 1 kHz.
            sumM_ += accM_ - ringM_[ringPos_];
            sumS_ += accS_ - ringS_[ringPos_];
            ringM_[ringPos_] = accM_;
            ringS_[ringPos_] = accS_;
            if (++ringPos_ >= ringM_.size()) ringPos_ = 0;
            // The AFC looks every 250 ms once its window has filled.
            if (p_.afc && !afcRing_.empty() && ++afcTick_ >= 250) {
                afcTick_ = 0;
                if (afcFill_ >= afcRing_.size()) acquire();
            }
            accM_ = accS_ = {0, 0};
            accN_ = 0;
            decimated(float(std::abs(sumM_)), float(std::abs(sumS_)));
        }
    }
}

// One DFT bin of the AFC window at `hz` relative to the mark mixer.
std::complex<double> RttyDemod::probe(double hz) const {
    const std::complex<double> rot =
        std::polar(1.0, -2.0 * kPi * hz * afcDecim_ / p_.sampleRate);
    std::complex<double> ph(1, 0), acc(0, 0);
    const size_t N = afcRing_.size();
    for (size_t i = 0; i < N; ++i) {
        size_t k = afcPos_ + i;
        if (k >= N) k -= N;
        acc += afcRing_[k] * (afcWin_[i] * ph);
        ph *= rot;
    }
    return acc;
}

// RTTY keys: one tone at a time. Over the window in 16 ms pieces, each
// tone must be clearly the louder one in some pieces (a 22 ms element
// always covers one). Two steady carriers 170 Hz apart — a busy band has them — fail
// this; they held a false lock live (2026-11-07, 614 + 790 Hz).
bool RttyDemod::pairKeyed(double d) const {
    const double rel = sHz_ - mHz_;
    const size_t N = afcRing_.size();
    const size_t piece = std::max<size_t>(8, size_t(std::lround(0.016 * p_.sampleRate / afcDecim_)));
    const std::complex<double> rotM = std::polar(1.0, -2.0 * kPi * d * afcDecim_ / p_.sampleRate);
    const std::complex<double> rotS = std::polar(1.0, -2.0 * kPi * (d + rel) * afcDecim_ / p_.sampleRate);
    std::complex<double> phM(1, 0), phS(1, 0), am(0, 0), as(0, 0);
    int nM = 0, nS = 0;
    size_t inPiece = 0;
    for (size_t i = 0; i < N; ++i) {
        size_t k = afcPos_ + i;
        if (k >= N) k -= N;
        am += afcRing_[k] * phM;
        as += afcRing_[k] * phS;
        phM *= rotM;
        phS *= rotS;
        if (++inPiece >= piece) {
            const double em = std::abs(am), es = std::abs(as);
            if (em > 2.0 * es) ++nM;
            else if (es > 2.0 * em) ++nS;
            am = as = {0, 0};
            inPiece = 0;
        }
    }
    // Two pieces each at 2:1 — text keys both tones ~half the time; two
    // carriers wobbling about equal level never swap that decisively.
    return nM >= 2 && nS >= 2;
}

// Find the tone pair: the offset d (within the range) where BOTH a tone
// at d and one at d + (space - mark) stand out — scored by the WEAKER of
// the two bins, so a lone tone scores like noise. (The geometric mean
// was tried first: a mark idling alone at 40 dB made sqrt(tone x noise)
// pass at d = 0 and d = -shift alike, and the first characters of every
// over came out garbled while it guessed.) 10 Hz grid, then 2 Hz about
// the best, 60 % of the way per step. No pair = hold: a station idling
// on mark between overs must not lose its trim.
void RttyDemod::acquire() {
    const double rel = sHz_ - mHz_;
    const double range = afcTrack_ ? p_.afcTrackHz : p_.afcRangeHz;
    // The audio feed's tones must stay inside the audio band; the IQ feed
    // (tones at +-shift/2 about 0) has no such edge.
    const bool audio = std::abs(p_.markHz) > 400.0;
    const double lo = std::min(mHz_, sHz_) + afcHz_, hi = std::max(mHz_, sHz_) + afcHz_;
    std::vector<double> pm;
    pm.reserve(256);
    std::vector<std::pair<double, double>> cand;   // (score, d)
    for (double d = -range; d <= range + 1e-9; d += 10.0) {
        if (audio && (lo + d < 200.0 || hi + d > 3600.0)) continue;
        const double Pm = std::norm(probe(d));
        const double Ps = std::norm(probe(d + rel));
        pm.push_back(Pm);
        cand.emplace_back(std::min(Pm, Ps), d);
    }
    if (pm.size() < 3) return;
    std::nth_element(pm.begin(), pm.begin() + pm.size() / 2, pm.end());
    const double median = pm[pm.size() / 2];
    // The strongest pairs over the floor, keyed ones only (three looks).
    std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    double bestD = 0.0, bestS = -1.0;
    for (size_t i = 0; i < cand.size() && i < 3; ++i) {
        if (cand[i].first < 10.0 * std::max(median, 1e-30)) break;
        if (pairKeyed(cand[i].second)) { bestS = cand[i].first; bestD = cand[i].second; break; }
    }
#ifdef RTTY_AFC_DEBUG
    std::fprintf(stderr, "[afc] %s afc=%.0f top d=%.0f (%.1fx) keyed-best d=%.0f q=%.2f miss=%u\n",
                 afcTrack_ ? "track" : "acq", afcHz_, cand.empty() ? 0.0 : cand[0].second,
                 cand.empty() ? 0.0 : cand[0].first / std::max(median, 1e-30), bestS < 0 ? 999.0 : bestD,
                 quality_, afcMiss_);
#endif
    // Nothing keyed, or a lock whose squelch stays shut: count it; after
    // 5 s of that a lock is released and the full range searched again.
    const bool quiet = p_.squelch > 0.0 && quality_ < p_.squelch - 0.1;
    if (bestS < 0.0 || (afcTrack_ && quiet)) {
        if (afcTrack_ && ++afcMiss_ >= 20) { afcTrack_ = false; afcMiss_ = 0; }
        return;
    }
    afcMiss_ = 0;
    afcTrack_ = true;                   // locked: from here on, follow it
    double rd = bestD, rs = bestS;
    for (double d = bestD - 10.0; d <= bestD + 10.0 + 1e-9; d += 2.0) {
        const double sc = std::min(std::norm(probe(d)), std::norm(probe(d + rel)));
        if (sc > rs) { rs = sc; rd = d; }
    }
    // Clearly off (a click on the other line): all the way at once; near:
    // 60 % per step, so the trim settles without hunting.
    const double step = std::abs(rd) >= 30.0 ? 1.0 : 0.6;
    // Clamp the ABSOLUTE trim to the acquisition range, never the track
    // window (that snapped a lock 1070 Hz away back to -60, live-found).
    const double next = std::clamp(afcHz_ + step * rd, -p_.afcRangeHz, p_.afcRangeHz);
    if (std::abs(next - afcHz_) >= 1.0) {
        afcHz_ = next;
        setRotations();
        st_.afcHz = float(afcHz_);
    }
}

// One decimated sample (1 kHz) of each tone's matched-filter magnitude.
void RttyDemod::decimated(float m, float s) {
    // Peak trackers: instant attack, ~1 s decay (an idle sender holds
    // mark for seconds; space shows in every start bit, so both stay
    // fresh while anyone keys).
    constexpr double kDecay = 1.0 - 1.0 / 1000.0;
    pkM_ = std::max(double(m), pkM_ * kDecay);
    pkS_ = std::max(double(s), pkS_ * kDecay);
    // A tone nobody has keyed for a while (an idle sender holds mark)
    // leaves its tracker on the OTHER tone's leakage, and normalising to
    // that reads the leakage as a full tone — the first start bit then
    // came out a bit and a half long. Neither peak may sit more than
    // 12 dB under the other; a real selective fade deeper than that is
    // lost copy anyway.
    const double hi = std::max(pkM_, pkS_);
    pkM_ = std::max(pkM_, hi * 0.25);
    pkS_ = std::max(pkS_, hi * 0.25);
    // Noise floor: the quieter tone, tracked with a slow rise and a
    // quick fall. With a signal present one tone is always off, so the
    // floor reads the noise in a bit-wide filter — the right ruler.
    const double lo = std::min(double(m), double(s));
    if (lo < floor_) floor_ += (lo - floor_) * 0.05;
    else             floor_ += (lo - floor_) * 0.0005;
    const double pk = std::max(pkM_, pkS_);
    const double snr = 20.0 * std::log10(std::max(pk, 1e-9) / std::max(floor_, 1e-9));
    // Keying contrast: with a signal one tone is on and the other off, so
    // |m-s|/(m+s) sits near 1 most of the time; two noise envelopes give
    // ~0.4 at any level. Averaged over ~1 s it is the squelch's ruler —
    // a level-free one, so a fade changes nothing but the SNR readout.
    const double c = std::abs(double(m) - double(s)) / (double(m) + double(s) + 1e-12);
    quality_ += (c - quality_) * 0.001;
    if (warm_ < 300) ++warm_;           // filters and trackers settling
    st_.mark = float(m / pkM_);
    st_.space = float(s / pkS_);
    st_.snrDb = float(snr);
    st_.quality = float(quality_);
    if (p_.squelch <= 0.0) st_.open = warm_ >= 300;
    else if (st_.open) st_.open = warm_ >= 300 && quality_ > p_.squelch - 0.05;
    else st_.open = warm_ >= 300 && quality_ > p_.squelch;
    // ATC: compare each tone against its own recent peak.
    const bool mark = (m / pkM_) >= (s / pkS_);
    bit(mark);
}

// UART at 1 kHz with a fractional bit length. Start = first space after
// mark; verified half a bit later; data sampled at the bit centres; the
// stop bit must read mark or the character is a framing error (dropped,
// and the next space edge starts over — that is the resync).
void RttyDemod::bit(bool mark) {
    const int b = mark ? 1 : 0;
    switch (uart_) {
        case Uart::Idle:
            if (lastBit_ == 1 && b == 0) { uart_ = Uart::Start; t_ = 0; }
            break;
        case Uart::Start:
            t_ += 1.0;
            if (t_ >= bitLen_ * 0.5) {
                if (b == 0) { uart_ = Uart::Data; dataBits_ = 0; code_ = 0; }
                else uart_ = Uart::Idle;           // a glitch, not a start
            }
            break;
        case Uart::Data:
            t_ += 1.0;
            if (dataBits_ < 5) {
                if (t_ >= bitLen_ * (1.5 + dataBits_)) {
                    code_ |= uint8_t(b << dataBits_);
                    ++dataBits_;
                }
            } else if (t_ >= bitLen_ * 6.5) {
                uart_ = Uart::Idle;
                if (b == 1) {
                    if (st_.open) {
                        const char c = ita_.feed(code_);
                        if (c && c != '\r') {
                            ++chars_;
                            if (onChar) onChar(c);
                        }
                    }
                } else {
                    ++frameErr_;
                }
            }
            break;
    }
    lastBit_ = b;
}

}  // namespace ttc
