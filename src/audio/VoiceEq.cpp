// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/VoiceEq.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <vector>

#include "audio/ClipDeck.h"
#include "dsp/ClientComp.h"
#include "dsp/ClientDeEss.h"
#include "dsp/ClientFinalLimiter.h"
#include "dsp/ClientGate.h"

namespace ttc {

namespace {
using FT = ClientEq::FilterType;

constexpr FT kSlotTypes[VoiceEqSettings::kBands] = {
    FT::HighPass, FT::LowShelf, FT::Peak, FT::Peak,
    FT::Peak,     FT::Peak,     FT::HighShelf, FT::LowPass,
};

ClientEq::BandParams band(float hz, float gainDb, float q, bool on,
                          FT type, int slope = 12) {
    ClientEq::BandParams b;
    b.freqHz = hz;
    b.gainDb = gainDb;
    b.q = q;
    b.type = type;
    b.enabled = on;
    b.slopeDbPerOct = slope;
    return b;
}

// Normalizing up after an EQ cut is capped: a take that was mostly rumble
// must not have its hiss raised 30 dB because the high-pass removed the
// rumble. 4x = +12 dB.
constexpr double kMaxMakeupGain = 4.0;
}  // namespace

ClientEq::FilterType VoiceEqSettings::slotType(int i) {
    return kSlotTypes[std::clamp(i, 0, kBands - 1)];
}

QString VoiceEqSettings::slotName(int i) {
    static const char* names[kBands] = {"High-pass", "Low shelf", "Peak 1",
                                        "Peak 2",    "Peak 3",    "Peak 4",
                                        "High shelf", "Low-pass"};
    return QString::fromLatin1(names[std::clamp(i, 0, kBands - 1)]);
}

QStringList VoiceEqSettings::presetNames() {
    return {"Flat", "SSB voice", "Wide SSB", "DX punch (2.4 kHz)",
            "ESSB smooth", "Shure 444", "Heil PR40"};
}

// SSB voice: the usual broadcast-voice moves for a dynamic mic on SSB —
// rumble/proximity gone below 100 Hz, a little mud out around 300 Hz,
// presence up around 2.5 kHz. Wide SSB keeps more bottom (50 Hz HPF,
// gentle low shelf) for the Orion's 3.9 kHz TX filter.
static VoiceEqSettings eqPreset(VoiceEqSettings::Preset p) {
    VoiceEqSettings s;
    if (p == VoiceEqSettings::Preset::DxPunch) {
        // Heil DX target, fitted inside a 300-2700 Hz (2.4 kHz) TX filter:
        // within ~1-2 dB of it across the passband, steep outside so the
        // ALC/processor never spend drive on audio the filter removes.
        // Pair with TX BW 2400 on the TX bar.
        s.enabled = true;
        s.bands[0] = band(230.f, 0, 0.707f, true, FT::HighPass, 24);
        s.bands[1] = band(120.f, 0, 0.707f, false, FT::LowShelf);
        s.bands[2] = band(450.f, -2.f, 1.0f, true, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1600.f, 2.f, 1.0f, true, FT::Peak);
        s.bands[5] = band(2600.f, 6.f, 1.3f, true, FT::Peak);
        s.bands[6] = band(3000.f, 0, 0.707f, false, FT::HighShelf);
        s.bands[7] = band(2900.f, 0, 0.707f, true, FT::LowPass, 24);
        return s;
    }
    if (p == VoiceEqSettings::Preset::HeilPr40) {
        // Least-squares fit to the Heil PR40 target (digitised from Heil's
        // chart) over 50-4000 Hz: rms 0.32 dB, worst 0.95 dB. Its gentle
        // low-end roll-off is the 60 Hz high-pass, the slight mid scoop a
        // broad dip at 390 Hz, its rising presence a 3.1 kHz peak plus a
        // high shelf.
        s.enabled = true;
        s.bands[0] = band(60.f, 0, 0.707f, true, FT::HighPass, 12);
        s.bands[1] = band(120.f, 0, 0.707f, false, FT::LowShelf);
        s.bands[2] = band(390.f, -1.f, 0.4f, true, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1600.f, 0, 1.0f, false, FT::Peak);
        s.bands[5] = band(3100.f, 4.f, 0.9f, true, FT::Peak);
        s.bands[6] = band(4800.f, 6.f, 0.707f, true, FT::HighShelf);
        s.bands[7] = band(3800.f, 0, 0.707f, false, FT::LowPass, 24);
        return s;
    }
    if (p == VoiceEqSettings::Preset::Shure444) {
        // Least-squares fit to the Shure 444 target over 50-4000 Hz (the
        // Orion's widest TX passband): rms 0.75 dB, worst 2 dB. The 444's
        // thin bottom is mostly the -6 dB shelf; its rising presence is a
        // broad 1.8 kHz lift plus a narrower 2.8 kHz peak.
        s.enabled = true;
        s.bands[0] = band(90.f, 0, 0.707f, true, FT::HighPass, 12);
        s.bands[1] = band(240.f, -6.f, 0.707f, true, FT::LowShelf);
        s.bands[2] = band(300.f, 0, 1.0f, false, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1800.f, 2.f, 0.8f, true, FT::Peak);
        s.bands[5] = band(2800.f, 3.f, 1.8f, true, FT::Peak);
        s.bands[6] = band(3000.f, 0, 0.707f, false, FT::HighShelf);
        s.bands[7] = band(3800.f, 0, 0.707f, false, FT::LowPass, 24);
        return s;
    }
    if (p == VoiceEqSettings::Preset::EssbSmooth) {
        // Full, smooth bottom (flat to 60 Hz, gentle shelf) with the mud
        // trimmed around 300 Hz and a broad +3..4.5 dB lift from 2 to
        // 3.8 kHz — for the Orion's 50-3900 Hz TX filter.
        s.enabled = true;
        s.bands[0] = band(40.f, 0, 0.707f, true, FT::HighPass, 12);
        s.bands[1] = band(110.f, 2.f, 0.6f, true, FT::LowShelf);
        s.bands[2] = band(300.f, -2.5f, 0.8f, true, FT::Peak);
        s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
        s.bands[4] = band(1800.f, 1.5f, 0.7f, true, FT::Peak);
        s.bands[5] = band(3000.f, 3.f, 0.9f, true, FT::Peak);
        s.bands[6] = band(3400.f, 1.5f, 0.707f, true, FT::HighShelf);
        s.bands[7] = band(3800.f, 0, 0.707f, false, FT::LowPass, 24);
        return s;
    }
    s.enabled = p != VoiceEqSettings::Preset::Flat;
    const bool wide = p == VoiceEqSettings::Preset::WideSsb;
    const bool on = p != VoiceEqSettings::Preset::Flat;
    s.bands[0] = band(wide ? 50.f : 100.f, 0, 0.707f, on, FT::HighPass, 24);
    s.bands[1] = band(120.f, wide ? 2.f : 0.f, 0.707f, wide, FT::LowShelf);
    s.bands[2] = band(wide ? 350.f : 300.f, wide ? -2.f : -3.f, 1.0f, on, FT::Peak);
    s.bands[3] = band(800.f, 0, 1.0f, false, FT::Peak);
    s.bands[4] = band(1600.f, 0, 1.0f, false, FT::Peak);
    s.bands[5] = band(wide ? 2800.f : 2500.f, wide ? 2.f : 3.f, wide ? 0.9f : 1.0f,
                      on, FT::Peak);
    s.bands[6] = band(3000.f, 0, 0.707f, false, FT::HighShelf);
    s.bands[7] = band(wide ? 3800.f : 3000.f, 0, 0.707f, false, FT::LowPass, 24);
    return s;
}

// The dynamics each built-in starts from: the EQ curve says what the voice
// sounds like, these say how hard it is driven. All tune-by-ear starting
// points; the final limiter is the safety net in every one but Flat.
// Flat is a TRUE bypass — limiter off too: its 25 Hz DC block tilts flat-
// topped (clipped) takes into excursions bigger than the speech, which
// then set the peak and turn the message down (live-found on a clipped
// ONN take: -6 dB). AetherSDR ships its final limiter off by default too.
VoiceEqSettings VoiceEqSettings::preset(Preset p) {
    VoiceEqSettings s = eqPreset(p);
    s.limit = Limit{};
    if (p == Preset::Flat) {                       // nothing touches the take
        s.limit.on = false;
        return s;
    }
    s.gate.on = true;                              // gentle expander
    s.comp.on = true;
    switch (p) {
    case Preset::DxPunch:                          // pile-up talk power
        s.gate.thresholdDb = -42.f;
        s.gate.ratio = 3.f;
        s.comp.thresholdDb = -24.f;
        s.comp.ratio = 4.f;
        s.comp.attackMs = 5.f;
        s.comp.releaseMs = 120.f;
        s.comp.driveDb = 3.f;
        s.deess.on = true;                         // the +6 dB presence peak
        s.deess.freqHz = 3500.f;                   // makes esses spit
        s.deess.amountDb = -4.f;
        break;
    case Preset::EssbSmooth:                       // open, unsquashed
        s.comp.thresholdDb = -20.f;
        s.comp.ratio = 2.f;
        s.comp.attackMs = 15.f;
        s.comp.releaseMs = 250.f;
        s.comp.kneeDb = 8.f;
        s.deess.on = true;
        s.deess.freqHz = 3800.f;
        s.deess.amountDb = -5.f;
        break;
    case Preset::WideSsb:
        s.comp.ratio = 2.5f;
        s.comp.thresholdDb = -22.f;
        break;
    default:                                       // SSB voice, Shure 444
        break;
    }
    return s;
}

QJsonObject VoiceEqSettings::toJson() const {
    QJsonArray arr;
    for (const auto& b : bands)
        arr.append(QJsonObject{{"on", b.enabled}, {"hz", b.freqHz},
                               {"db", b.gainDb}, {"q", b.q},
                               {"slope", b.slopeDbPerOct}});
    return QJsonObject{
        {"enabled", enabled}, {"bands", arr},
        {"gate", QJsonObject{{"on", gate.on}, {"thr", gate.thresholdDb},
                             {"ratio", gate.ratio}, {"floor", gate.floorDb},
                             {"hold", gate.holdMs}, {"rel", gate.releaseMs},
                             {"ret", gate.returnDb}, {"look", gate.lookaheadMs}}},
        {"deess", QJsonObject{{"on", deess.on}, {"hz", deess.freqHz},
                              {"q", deess.q}, {"thr", deess.thresholdDb},
                              {"amt", deess.amountDb}, {"slope", deess.slopeStages}}},
        {"comp", QJsonObject{{"on", comp.on}, {"thr", comp.thresholdDb},
                             {"ratio", comp.ratio}, {"att", comp.attackMs},
                             {"rel", comp.releaseMs}, {"knee", comp.kneeDb},
                             {"drive", comp.driveDb}, {"phase", comp.phaseStages},
                             {"lim", comp.limiterOn}, {"ceil", comp.ceilingDb},
                             {"mk", comp.makeupDb}}},
        {"limit", QJsonObject{{"on", limit.on}, {"ceil", limit.ceilingDb},
                              {"dc", limit.dcBlock}}},
    };
}

VoiceEqSettings VoiceEqSettings::fromJson(const QJsonObject& o) {
    VoiceEqSettings s = preset(Preset::Flat);
    s.enabled = o.value("enabled").toBool();
    const QJsonArray arr = o.value("bands").toArray();
    for (int i = 0; i < kBands && i < arr.size(); ++i) {
        const QJsonObject b = arr.at(i).toObject();
        auto& d = s.bands[size_t(i)];
        d.type = slotType(i);
        d.enabled = b.value("on").toBool();
        d.freqHz = std::clamp(float(b.value("hz").toDouble(d.freqHz)), 20.f, 8000.f);
        d.gainDb = std::clamp(float(b.value("db").toDouble(0)), -24.f, 24.f);
        d.q = std::clamp(float(b.value("q").toDouble(d.q)), 0.1f, 18.f);
        d.slopeDbPerOct = std::clamp(b.value("slope").toInt(12), 12, 48) / 12 * 12;
    }
    // Dynamics: absent (a curve saved before they existed) = all off,
    // final limiter included, so it renders exactly as it did. Ranges are
    // the ported classes' own clamps.
    const auto f = [](const QJsonObject& g, const char* k, float def, float lo, float hi) {
        return std::clamp(float(g.value(k).toDouble(def)), lo, hi);
    };
    const QJsonObject g = o.value("gate").toObject();
    s.gate.on = g.value("on").toBool(false);
    s.gate.thresholdDb = f(g, "thr", s.gate.thresholdDb, -80, 0);
    s.gate.ratio = f(g, "ratio", s.gate.ratio, 1, 10);
    s.gate.floorDb = f(g, "floor", s.gate.floorDb, -80, 0);
    s.gate.holdMs = f(g, "hold", s.gate.holdMs, 0, 500);
    s.gate.releaseMs = f(g, "rel", s.gate.releaseMs, 5, 2000);
    s.gate.returnDb = f(g, "ret", s.gate.returnDb, 0, 20);
    s.gate.lookaheadMs = f(g, "look", s.gate.lookaheadMs, 0, 5);
    const QJsonObject d = o.value("deess").toObject();
    s.deess.on = d.value("on").toBool(false);
    s.deess.freqHz = f(d, "hz", s.deess.freqHz, 1000, 12000);
    s.deess.q = f(d, "q", s.deess.q, 0.5f, 5);
    s.deess.thresholdDb = f(d, "thr", s.deess.thresholdDb, -60, 0);
    s.deess.amountDb = f(d, "amt", s.deess.amountDb, -24, 0);
    s.deess.slopeStages = std::clamp(d.value("slope").toInt(s.deess.slopeStages), 1, 4);
    const QJsonObject c = o.value("comp").toObject();
    s.comp.on = c.value("on").toBool(false);
    s.comp.thresholdDb = f(c, "thr", s.comp.thresholdDb, -60, 0);
    s.comp.ratio = f(c, "ratio", s.comp.ratio, 1, 20);
    s.comp.attackMs = f(c, "att", s.comp.attackMs, 0.1f, 200);
    s.comp.releaseMs = f(c, "rel", s.comp.releaseMs, 5, 2000);
    s.comp.kneeDb = f(c, "knee", s.comp.kneeDb, 0, 24);
    s.comp.driveDb = f(c, "drive", s.comp.driveDb, 0, 18);
    s.comp.phaseStages = std::clamp(c.value("phase").toInt(s.comp.phaseStages), 0, 6);
    s.comp.limiterOn = c.value("lim").toBool(true);
    s.comp.ceilingDb = f(c, "ceil", s.comp.ceilingDb, -12, 0);
    s.comp.makeupDb = f(c, "mk", s.comp.makeupDb, 0, 24);
    const QJsonObject l = o.value("limit").toObject();
    s.limit.on = l.value("on").toBool(false);
    s.limit.ceilingDb = f(l, "ceil", s.limit.ceilingDb, -12, 0);
    s.limit.dcBlock = l.value("dc").toBool(true);
    return s;
}

VoiceEqSettings VoiceEqSettings::load() {
    const QByteArray raw = QSettings().value("vk/eq").toByteArray();
    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isObject()) {
        // Never configured: the voice preset's curve, but OFF — the
        // operator's existing messages stay exactly as recorded until he
        // turns it on.
        VoiceEqSettings s = preset(Preset::SsbVoice);
        s.enabled = false;
        return s;
    }
    return fromJson(doc.object());
}

namespace {
QJsonObject userProfiles(const char* store) {
    return QJsonDocument::fromJson(
               QSettings().value(QLatin1String(store)).toByteArray()).object();
}
void storeUserProfiles(const char* store, const QJsonObject& o) {
    QSettings().setValue(QLatin1String(store),
                         QJsonDocument(o).toJson(QJsonDocument::Compact));
}
}  // namespace

QStringList VoiceEqSettings::userProfileNames(const char* store) {
    QStringList names = userProfiles(store).keys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool VoiceEqSettings::loadUserProfile(const QString& name, VoiceEqSettings& out,
                                      const char* store) {
    const QJsonObject all = userProfiles(store);
    if (!all.contains(name)) return false;
    out = fromJson(all.value(name).toObject());
    return true;
}

void VoiceEqSettings::saveUserProfile(const QString& name,
                                      const VoiceEqSettings& s, const char* store) {
    QJsonObject all = userProfiles(store);
    all[name] = s.toJson();
    storeUserProfiles(store, all);
}

void VoiceEqSettings::deleteUserProfile(const QString& name, const char* store) {
    QJsonObject all = userProfiles(store);
    all.remove(name);
    storeUserProfiles(store, all);
}

void VoiceEqSettings::save() const {
    QSettings().setValue("vk/eq", QJsonDocument(toJson())
                                      .toJson(QJsonDocument::Compact));
}

float VoiceEqSettings::responseDb(float hz) const {
    if (!enabled) return 0.0f;
    float db = 0.0f;
    for (const auto& b : bands)
        db += ClientEq::bandMagnitudeDb(b, hz, 48000.0);
    return db;
}

float VoiceEqTarget::dbAt(float hz) const {
    if (pts.empty()) return 0.0f;
    if (hz <= pts.front().hz) return pts.front().db;
    if (hz >= pts.back().hz) return pts.back().db;
    for (size_t i = 1; i < pts.size(); ++i)
        if (hz <= pts[i].hz) {
            const float t = std::log(hz / pts[i - 1].hz)
                          / std::log(pts[i].hz / pts[i - 1].hz);
            return pts[i - 1].db + t * (pts[i].db - pts[i - 1].db);
        }
    return pts.back().db;
}

const std::vector<VoiceEqTarget>& voiceEqTargets() {
    static const std::vector<VoiceEqTarget> t = {
        {"None", {}},
        {"AT&T 1959", {{50, -20}, {100, -12}, {200, -6}, {300, -2}, {500, 0},
                       {1000, 0}, {1500, 1}, {2000, 3}, {2500, 5}, {3000, 4},
                       {3400, 0}, {4000, -6}, {5000, -12}}},
        {"Heil DX", {{50, -25}, {100, -18}, {200, -10}, {300, -4}, {500, -1},
                     {1000, 0}, {1500, 2}, {2000, 4}, {2500, 6}, {2700, 6},
                     {3000, 5}, {3400, -2}, {4000, -10}, {5000, -18}}},
        {"Astatic D-104", {{50, -32}, {100, -22}, {200, -14}, {300, -8},
                           {500, -4}, {1000, 0}, {1500, 2}, {2000, 5},
                           {2500, 8}, {3000, 10}, {3500, 7}, {4000, 2},
                           {5000, -10}, {7000, -22}}},
        {"Shure 444", {{50, -15}, {100, -10}, {200, -4}, {300, -1}, {500, 0},
                       {1000, 1}, {1500, 2}, {2000, 3}, {2500, 4}, {3000, 4},
                       {3500, 3}, {4000, 1}, {5000, -3}, {7000, -10}}},
        {"Heil HC-5", {{50, -28}, {100, -18}, {200, -8}, {300, -3}, {500, 0},
                       {1000, 0}, {1500, 2}, {2000, 4}, {2500, 6}, {3000, 5},
                       {3500, 1}, {4000, -5}, {5000, -15}}},
        // Appended (stored target indices must not move). Ours, not AetherSDR's:
        // EV RE20 from Electro-Voice's spec text: flat 80 Hz-6 kHz, -3 dB at
        // 45 Hz, a broad +2.5 dB rise 6-14 kHz (only its start is on this
        // plot). The bass-tilt switch adds -4.5 dB from 400 down to 100 Hz.
        {"EV RE20", {{40, -4.5f}, {45, -3}, {60, -1}, {80, 0}, {1000, 0},
                     {6000, 0}, {7000, 0.6f}}},
        {"EV RE20 (bass tilt)", {{40, -9}, {45, -7.5f}, {60, -5.5f}, {80, -5},
                                 {100, -4.5f}, {150, -3}, {200, -2}, {300, -0.8f},
                                 {400, 0}, {6000, 0}, {7000, 0.6f}}},
        // Heil PR40: digitised from the on-axis (blue) curve of Heil's spec-
        // sheet chart (Heil_PR40_PS.pdf, 1/2021) at 400 dpi, relative to 1 kHz.
        {"Heil PR40", {{40, -7}, {50, -5.5f}, {63, -3.7f}, {80, -1.7f}, {100, -0.3f},
                       {150, -1}, {200, -1.2f}, {300, -1}, {500, -0.8f}, {700, -1},
                       {1000, 0}, {1500, 0.8f}, {2000, 2.3f}, {2500, 3.7f},
                       {3000, 4.9f}, {3500, 5.2f}, {4000, 5.3f}, {5000, 7.2f},
                       {6000, 7.6f}, {7000, 7.2f}}},
    };
    return t;
}

void VoiceChain::prepare(int rate, const VoiceEqSettings& s, float makeupDb) {
    rate_ = rate;
    gate_.prepare(rate);
    eq_.prepare(rate);
    deess_.prepare(rate);
    comp_.prepare(rate);
    limit_.prepare(rate);
    configure(s, makeupDb);
    eq_.snapToTargets();                           // start on the curve
}

void VoiceChain::configure(const VoiceEqSettings& s, float makeupDb) {
    s_ = s;
    gate_.setEnabled(s.gate.on);
    gate_.setThresholdDb(s.gate.thresholdDb);
    gate_.setRatio(s.gate.ratio);
    gate_.setFloorDb(s.gate.floorDb);
    gate_.setHoldMs(s.gate.holdMs);
    gate_.setReleaseMs(s.gate.releaseMs);
    gate_.setReturnDb(s.gate.returnDb);
    gate_.setLookaheadMs(s.gate.lookaheadMs);
    eq_.setEnabled(s.enabled);
    eq_.setActiveBandCount(VoiceEqSettings::kBands);
    for (int i = 0; i < VoiceEqSettings::kBands; ++i) {
        ClientEq::BandParams b = s.bands[size_t(i)];
        b.type = VoiceEqSettings::slotType(i);
        eq_.setBand(i, b);                         // smoothed in live use
    }
    deess_.setEnabled(s.deess.on);
    deess_.setFrequencyHz(s.deess.freqHz);
    deess_.setQ(s.deess.q);
    deess_.setThresholdDb(s.deess.thresholdDb);
    deess_.setAmountDb(s.deess.amountDb);
    deess_.setSlopeStages(s.deess.slopeStages);
    comp_.setEnabled(s.comp.on);
    comp_.setThresholdDb(s.comp.thresholdDb);
    comp_.setRatio(s.comp.ratio);
    comp_.setAttackMs(s.comp.attackMs);
    comp_.setReleaseMs(s.comp.releaseMs);
    comp_.setKneeDb(s.comp.kneeDb);
    comp_.setDriveDb(s.comp.driveDb);
    comp_.setMakeupDb(makeupDb);
    comp_.setPhaseRotatorStages(s.comp.phaseStages);
    comp_.setLimiterEnabled(s.comp.limiterOn);
    comp_.setLimiterCeilingDb(s.comp.ceilingDb);
    limit_.setEnabled(s.limit.on);
    limit_.setCeilingDb(s.limit.ceilingDb);
    limit_.setDcBlockEnabled(s.limit.dcBlock);
}

void VoiceChain::process(float* x, int n) {
    if (n <= 0) return;
    // Stages that are off are skipped outright — the final limiter's DC
    // block would otherwise run even with the limiter disabled.
    if (s_.gate.on) gate_.process(x, n, 1);
    if (s_.enabled) eq_.process(x, n, 1);
    if (s_.deess.on) deess_.process(x, n, 1);
    if (s_.comp.on) comp_.process(x, n, 1);
    if (s_.limit.on) limit_.process(x, n, 1);
}

void VoiceChain::reset() {
    gate_.reset();
    eq_.reset();
    deess_.reset();
    comp_.reset();
    limit_.reset();
}

// Offline level stage for processed messages (a ttc addition, not
// AetherSDR's): peak-normalize by the message's sustained level, not by
// the few word onsets a compressor's attack lets through, then hold those
// onsets to the target with a look-ahead brickwall that a live limiter
// cannot have. Gain = min-filter of the needed reduction over the next
// 2 ms, then a 2 ms moving average — the textbook overshoot-free look-
// ahead limiter. Measured on a compressed test phrase: the onsets sat
// 2-3 dB over the steady level and, used as the peak, cost all the talk
// power the compressor had made.
static void lookaheadLevel(std::vector<float>& x, int rate, float target,
                           float maxGain) {
    std::vector<float> mag(x.size());
    for (size_t i = 0; i < x.size(); ++i) mag[i] = std::fabs(x[i]);
    std::vector<float> sorted = mag;
    const size_t k = std::min(sorted.size() - 1, sorted.size() * 9995 / 10000);
    std::nth_element(sorted.begin(), sorted.begin() + long(k), sorted.end());
    const float sustained = std::max(sorted[k], 1e-6f);
    const float gain = std::min(target / sustained, maxGain);
    for (size_t i = 0; i < x.size(); ++i) { x[i] *= gain; mag[i] *= gain; }

    const size_t L = size_t(std::max(1, rate / 500));   // 2 ms
    const size_t n = x.size();
    std::vector<float> need(n);
    for (size_t i = 0; i < n; ++i)
        need[i] = mag[i] > target ? target / mag[i] : 1.0f;
    // Sliding minimum over [i, i+L) (monotone deque).
    std::vector<float> g(n);
    std::vector<size_t> dq;
    dq.reserve(n);
    size_t head = 0;
    for (size_t j = 0; j < n + L; ++j) {
        if (j < n) {
            while (dq.size() > head && need[dq.back()] >= need[j]) dq.pop_back();
            dq.push_back(j);
        }
        if (j + 1 >= L) {
            const size_t i = j + 1 - L;
            if (i >= n) break;                     // window start past the end
            while (dq[head] < i) ++head;
            g[i] = need[dq[head]];
        }
    }
    // Smooth with a length-L moving average of the (shifted) minimum so the
    // gain ramps down over the 2 ms before each onset instead of stepping.
    std::vector<float> gs(n);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t src = i >= L ? i - L : 0;     // look back over the min
        acc += g[i];
        if (i >= L) acc -= g[src];
        const size_t cnt = std::min(i + 1, L);
        gs[i] = float(acc / double(cnt));
    }
    for (size_t i = 0; i < n; ++i) x[i] *= std::min(g[i], gs[i]);
}

bool renderVoiceEq(const QString& inWav, const QString& outWav,
                   const VoiceEqSettings& s, double targetPeak,
                   QString* error, VoiceRenderStats* stats) {
    std::vector<float> x;
    int rate = 48000;
    if (!ClipDeck::readWavMono(inWav, x, rate) || x.empty()) {
        if (error) *error = "cannot read " + inWav;
        return false;
    }
    const std::vector<float> dry = x;              // for the talk-power figure
    float dryPeak = 0.0f;
    for (float v : dry) dryPeak = std::max(dryPeak, std::fabs(v));
    const float speechFloor = dryPeak * 0.0178f;   // -35 dB: "speech" blocks
    const int kBlock = rate / 100;                 // 10 ms: stages meter per block

    // One pass of the chain over a fresh copy of the take. Stages are
    // rebuilt each pass (they hold envelope state). makeupDb only matters
    // with the compressor on.
    const auto runChain = [&](float makeupDb, std::vector<float>& y,
                              VoiceRenderStats& st) {
        y = dry;
        st = VoiceRenderStats{};
        VoiceChain chain;                          // fresh envelopes per pass
        chain.prepare(rate, s, makeupDb);
        double compSum = 0;
        int speechBlocks = 0, limitBlocks = 0;
        for (size_t o = 0; o < y.size(); o += size_t(kBlock)) {
            const int n = int(std::min<size_t>(size_t(kBlock), y.size() - o));
            float inPk = 0.0f;
            for (int i = 0; i < n; ++i) inPk = std::max(inPk, std::fabs(dry[o + size_t(i)]));
            chain.process(y.data() + o, n);
            st.gateMaxDb = std::min(st.gateMaxDb, double(chain.gateGrDb()));
            st.deessMaxDb = std::min(st.deessMaxDb, double(chain.deessGrDb()));
            if (inPk >= speechFloor) {
                ++speechBlocks;
                const double gr = chain.compGrDb();
                compSum += gr;
                st.compMaxDb = std::min(st.compMaxDb, gr);
                if (chain.limitActive()) ++limitBlocks;
            }
        }
        if (speechBlocks > 0) {
            st.compAvgDb = compSum / speechBlocks;
            st.limitPct = 100.0 * limitBlocks / speechBlocks;
        }
    };

    // Auto make-up: a compressor turns loud speech DOWN; the talk power
    // comes from raising it back up to the ceiling and letting the
    // limiters catch the word onsets its attack lets through. Offline we
    // can measure instead of guess: pass 1 (no make-up) finds the level
    // the compressed speech sits at — the 95th percentile of its 10 ms
    // block peaks — and pass 2 lifts that to the compressor's ceiling.
    // (Average gain reduction is the wrong yardstick: it under-lifts the
    // loud passages and the onsets then set the peak — measured.)
    VoiceRenderStats st;
    runChain(0.0f, x, st);
    if (s.comp.on) {
        std::vector<float> blockPk;
        for (size_t o = 0; o < x.size(); o += size_t(kBlock)) {
            const size_t n = std::min<size_t>(size_t(kBlock), x.size() - o);
            float inPk = 0.0f, outPk = 0.0f;
            for (size_t i = 0; i < n; ++i) {
                inPk = std::max(inPk, std::fabs(dry[o + i]));
                outPk = std::max(outPk, std::fabs(x[o + i]));
            }
            if (inPk >= speechFloor && outPk > 0.0f) blockPk.push_back(outPk);
        }
        if (!blockPk.empty()) {
            const size_t k = std::min(blockPk.size() - 1, blockPk.size() * 95 / 100);
            std::nth_element(blockPk.begin(), blockPk.begin() + long(k), blockPk.end());
            const float p95Db = 20.0f * std::log10(blockPk[k]);
            const float makeup = std::clamp(s.comp.ceilingDb - p95Db, 0.0f, 24.0f);
            runChain(makeup, x, st);
        }
    }

    float peak = 0.0f;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    const bool dynamics = s.gate.on || s.deess.on || s.comp.on || s.limit.on;
    if (peak > 0.0f && !dynamics) {                // EQ only: as it always was
        const double gain = std::min(targetPeak / peak, kMaxMakeupGain);
        for (float& v : x) v = float(v * gain);
    } else if (peak > 0.0f) {
        lookaheadLevel(x, rate, float(targetPeak), float(kMaxMakeupGain));
        peak = 0.0f;
        for (float v : x) peak = std::max(peak, std::fabs(v));
    }
    // Talk power: RMS over the speech blocks, each signal relative to its
    // own peak — the crest-factor change the chain made. Both are measured
    // about their own mean: a take with a DC offset (cheap USB mics have
    // them) otherwise reads as "louder" before the chain's DC block / high-
    // pass than after (live-found: Flat read -6.4 dB on an ONN take).
    if (dryPeak > 0.0f && peak > 0.0f) {
        double mdry = 0, mwet = 0;
        for (size_t i = 0; i < x.size(); ++i) { mdry += dry[i]; mwet += x[i]; }
        mdry /= double(x.size());
        mwet /= double(x.size());
        double sd = 0, sw = 0;
        size_t cnt = 0;
        for (size_t o = 0; o < x.size(); o += size_t(kBlock)) {
            const size_t n = std::min<size_t>(size_t(kBlock), x.size() - o);
            float inPk = 0.0f;
            for (size_t i = 0; i < n; ++i) inPk = std::max(inPk, std::fabs(dry[o + i]));
            if (inPk < speechFloor) continue;
            for (size_t i = 0; i < n; ++i) {
                const double d = dry[o + i] - mdry, w = x[o + i] - mwet;
                sd += d * d;
                sw += w * w;
            }
            cnt += n;
        }
        double pd = 0, pw = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            pd = std::max(pd, std::fabs(dry[i] - mdry));
            pw = std::max(pw, std::fabs(x[i] - mwet));
        }
        if (cnt > 0 && sd > 0 && sw > 0 && pd > 0 && pw > 0)
            st.talkPowerDb = 10.0 * std::log10(sw / cnt) - 20.0 * std::log10(pw)
                           - (10.0 * std::log10(sd / cnt) - 20.0 * std::log10(pd));
    }
    if (stats) *stats = st;

    const QString tmp = outWav + ".tmp";
    if (!ClipDeck::writeWavMono(tmp, x, rate)) {
        if (error) *error = "cannot write " + tmp;
        QFile::remove(tmp);
        return false;
    }
    QFile::remove(outWav);
    if (!QFile::rename(tmp, outWav)) {
        if (error) *error = "cannot replace " + outWav;
        return false;
    }
    return true;
}

} // namespace ttc
