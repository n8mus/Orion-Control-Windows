// SPDX-License-Identifier: GPL-2.0-or-later
// The contest registry. One entry per contest, readable top-to-bottom
// like the rules it encodes. Sources: each sponsor's published rules and
// the WWROF Cabrillo tag registry — never another logger's code.
//
// Known approximations (flagged, not hidden): ARRL 10 and the QSO-party
// mults key on the COPIED exchange rather than a canonical section
// list, and WAE uses DXCC entities (Sicily & friends counted with their
// parents). Robots score those officially; these numbers steer the
// operator during the weekend.
#include "contest/ContestDef.h"

#include "contest/ContestEngine.h"
#include "util/CtyLookup.h"

namespace ttc {

namespace {

// Mult keys: "C:<country>|<band>" per-band country, "Z:<zone>|<band>"
// per-band zone, "CALL:<call>" whole-contest unique call, "P:<prefix>"
// whole-contest WPX prefix, "S:<text>" whole-contest section/state,
// "X:<text>|<mode>" per-mode exchange. The scope rides inside the key
// so computeScore needs no scope logic of its own.
QString bandOf(const QString& multKey) {
    const int bar = multKey.lastIndexOf('|');
    return bar < 0 ? QString() : multKey.mid(bar + 1);
}

bool lowBand(const QString& band) {
    return band == QLatin1String("160M") || band == QLatin1String("80M")
        || band == QLatin1String("40M");
}

bool isWVe(const CtyInfo& ci) {
    return ci.country == QLatin1String("United States")
        || ci.country == QLatin1String("Canada");
}

const QHash<int, QString> kBaseRun = {
    {1, "CQ|cq {MYCALL} {MYCALL}"},
    {2, "His Call|{HISCALL}"},
    {3, "Exch|{SNT} {EXCH}"},
    {4, "TU|tu {MYCALL}"},
    {5, "My Call|{MYCALL}"},
    {9, "AGN|agn"},
    {10, "NR?|nr?"},
};

QHash<int, QString> withOverrides(QHash<int, QString> base,
                                  const QHash<int, QString>& over) {
    for (auto it = over.constBegin(); it != over.constEnd(); ++it)
        base.insert(it.key(), it.value());
    return base;
}

// Phone contests: F-keys play voice-keyer slots. VK1=CQ, VK2=exchange,
// VK3=TU, VK4=my call — record them once via the VK buttons on the TX
// bar. His call (F2) stays empty: you SPEAK the call, then Enter/F3
// plays the exchange. The CW text keys (AGN, NR?) blank out so a stray
// F9 can't send Morse into a phone pileup.
const QHash<int, QString> kVoiceRun = {
    {1, "CQ|{VK1}"},      {2, ""},
    {3, "Exch|{VK2}"},    {4, "TU|{VK3}"},
    {5, "My Call|{VK4}"}, {9, ""},
    {10, ""},
};

int one(const CQsoValues&, const CtyInfo&, bool, const ContestContext&) {
    return 1;
}

// ---------------------------------------------------------------- CW Open
ContestDef makeCwOpen() {
    ContestDef d;
    d.id = "CW-OPEN";
    d.title = "CW Open (CWops)";
    d.cabrilloName = "CW-OPEN";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "Jon";
    d.fields = {
        {ExchCol::SerialR, "RCV NR", 6, "", true, ""},
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
    };
    d.points = one;
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        return QStringList{"CALL:" + q.call};
    };
    d.cabExch = {"serial", "exch"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq cwo {MYCALL}"},
        {3, "Exch|{SENTNR} {EXCH}"},
        {8, "NAME?|name?"},
    });
    return d;
}

// -------------------------------------------------------------------- CWT
ContestDef makeCwt() {
    ContestDef d;
    d.id = "CW-OPS";
    d.title = "CWops CWT";
    d.cabrilloName = "CW-OPS";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = false;
    d.sentExchDefault = "Jon MI";   // non-member: name + state
    d.fields = {
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
        {ExchCol::Exch2, "NR / STATE", 8, "", true, "exch1"},
    };
    d.points = one;
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        return QStringList{"CALL:" + q.call};
    };
    d.cabExch = {"exch"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq cwt {MYCALL}"},
        {3, "Exch|{EXCH}"},
        {8, "NAME?|name?"},
    });
    return d;
}

// --------------------------------------------------------------- ICWC MST
ContestDef makeMst() {
    ContestDef d;
    d.id = "ICWC-MST";
    d.title = "ICWC MST (Medium Speed Test)";
    d.cabrilloName = "ICWC-MST";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "Jon";
    d.fields = {
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
        {ExchCol::SerialR, "RCV NR", 6, "", true, ""},
    };
    d.points = one;
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        return QStringList{"CALL:" + q.call};
    };
    d.cabExch = {"exch", "serial"};
    // MST is the skill-building crowd — plain digits, no cut numbers.
    d.cutNumbers = false;
    d.serialPad = 1;
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq mst {MYCALL}"},
        {3, "Exch|{EXCH} {SENTNR}"},
        {8, "NAME?|name?"},
    });
    return d;
}

// ----------------------------------------------------------------- CQ WW
ContestDef makeCqWw(bool cw) {
    ContestDef d;
    d.id = cw ? "CQ-WW-CW" : "CQ-WW-SSB";
    d.title = cw ? "CQ WW DX CW" : "CQ WW DX SSB";
    d.cabrilloName = d.id;
    d.modeCategory = cw ? "CW" : "SSB";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "4";        // my CQ zone
    d.fields = {
        {ExchCol::RstR, "RCV", 4, cw ? "599" : "59", true, ""},
        {ExchCol::Exch1, "ZONE", 4, "", true, "", "cqz"},
    };
    // Own country 0 (still logs, still counts the mults), same continent
    // 1 — except NA-to-NA which CQ pays 2 — other continent 3.
    d.points = [](const CQsoValues&, const CtyInfo& ci, bool ok,
                  const ContestContext& ctx) {
        if (!ok) return 3;          // cty miss: assume the best, robot decides
        if (ci.country == ctx.myCountry) return 0;
        if (ci.cont == ctx.myCont)
            return ci.cont == QLatin1String("NA") ? 2 : 1;
        return 3;
    };
    // Zone AND country, both per band, both weight 1. Zone comes from
    // the COPIED exchange (what the robot cross-checks), country from
    // cty; zero-point own-country QSOs still claim both.
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        QStringList keys;
        const QString z = q.exch1.trimmed();
        if (!z.isEmpty()) keys << "Z:" + z + "|" + q.band;
        if (ok) keys << "C:" + ci.country + "|" + q.band;
        return keys;
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = cw ? withOverrides(kBaseRun, {
                         {1, "CQ|cq test {MYCALL} {MYCALL}"},
                     })
                   : withOverrides(kBaseRun, kVoiceRun);
    return d;
}

// ----------------------------------------------------------------- CQ WPX
ContestDef makeWpx(bool cw) {
    ContestDef d;
    d.id = cw ? "CQ-WPX-CW" : "CQ-WPX-SSB";
    d.title = cw ? "CQ WPX CW" : "CQ WPX SSB";
    d.cabrilloName = d.id;
    d.modeCategory = cw ? "CW" : "SSB";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = true;
    d.sentExchDefault = "";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, cw ? "599" : "59", true, ""},
        {ExchCol::SerialR, "RCV NR", 6, "", true, ""},
    };
    // Different continent 3 (6 on 160/80/40); same continent different
    // country 1 (2 low) — NA pays double: 2 (4 low); same country 1.
    d.points = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                  const ContestContext& ctx) {
        const bool low = lowBand(q.band);
        if (!ok) return low ? 6 : 3;
        if (ci.country == ctx.myCountry) return 1;
        if (ci.cont == ctx.myCont) {
            const int base = ci.cont == QLatin1String("NA") ? 2 : 1;
            return low ? base * 2 : base;
        }
        return low ? 6 : 3;
    };
    // Prefixes count ONCE for the whole contest, any band.
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        const QString p = wpxPrefix(q.call);
        return p.isEmpty() ? QStringList{} : QStringList{"P:" + p};
    };
    d.cabExch = {"rst", "serial"};
    d.fkeyRun = cw ? withOverrides(kBaseRun, {
                         {1, "CQ|cq test {MYCALL} {MYCALL}"},
                         {3, "Exch|{SNT} {SENTNR}"},
                     })
                   : withOverrides(kBaseRun, kVoiceRun);
    return d;
}

// ---------------------------------------------------------------- ARRL DX
// The W/VE side: work DX only; send RST + state, copy RST + power.
ContestDef makeArrlDx(bool cw) {
    ContestDef d;
    d.id = cw ? "ARRL-DX-CW" : "ARRL-DX-SSB";
    d.title = cw ? "ARRL DX CW" : "ARRL DX Phone";
    d.cabrilloName = d.id;
    d.modeCategory = cw ? "CW" : "SSB";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "MI";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, cw ? "599" : "59", true, ""},
        {ExchCol::Exch1, "PWR", 5, "", true, ""},
    };
    const auto valid = [](const CtyInfo& ci, bool ok) {
        return ok && !isWVe(ci);
    };
    d.points = [valid](const CQsoValues&, const CtyInfo& ci, bool ok,
                       const ContestContext&) {
        return valid(ci, ok) ? 3 : 0;
    };
    d.mults = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                      const ContestContext&) {
        if (!valid(ci, ok)) return QStringList{};
        return QStringList{"C:" + ci.country + "|" + q.band};
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = cw ? withOverrides(kBaseRun, {
                         {1, "CQ|cq test {MYCALL} {MYCALL}"},
                     })
                   : withOverrides(kBaseRun, kVoiceRun);
    return d;
}

// ------------------------------------------------------------ Sweepstakes
// The famous mouthful: serial, precedence, YOUR call, check, section.
// Work each station ONCE for the whole contest; sections count once.
ContestDef makeSs(bool cw) {
    ContestDef d;
    d.id = cw ? "ARRL-SS-CW" : "ARRL-SS-SSB";
    d.title = cw ? "ARRL Sweepstakes CW" : "ARRL Sweepstakes Phone";
    d.cabrilloName = d.id;
    d.modeCategory = cw ? "CW" : "SSB";
    d.dupe = DupeScope::Contest;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "";         // "A 71 MI" — YOUR prec/check/section
    d.fields = {
        {ExchCol::SerialR, "NR", 6, "", true, ""},
        {ExchCol::Exch1, "PREC", 2, "", true, ""},
        {ExchCol::Exch2, "CK", 3, "", true, "ck"},
        {ExchCol::Exch3, "SEC", 4, "", true, "sect"},
    };
    d.points = [](const CQsoValues&, const CtyInfo&, bool,
                  const ContestContext&) { return 2; };
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        const QString s = q.exch3.trimmed();
        return s.isEmpty() ? QStringList{} : QStringList{"S:" + s};
    };
    d.cabExch = {"serial", "exch"};
    d.fkeyRun = cw ? withOverrides(kBaseRun, {
                         {1, "CQ|cq ss {MYCALL} {MYCALL}"},
                         {3, "Exch|{SENTNR} {EXCH}"},
                     })
                   : withOverrides(kBaseRun, kVoiceRun);
    return d;
}

// --------------------------------------------------------------- ARRL 160
ContestDef makeArrl160() {
    ContestDef d;
    d.id = "ARRL-160";
    d.title = "ARRL 160 Meter";
    d.cabrilloName = "ARRL-160";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;    // one band anyway
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "MI";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true, ""},
        // DX sends no section — the field may stay empty.
        {ExchCol::Exch1, "SECT", 5, "", false, "sect"},
    };
    d.points = [](const CQsoValues&, const CtyInfo& ci, bool ok,
                  const ContestContext&) {
        return ok && !isWVe(ci) ? 5 : 2;
    };
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        const QString s = q.exch1.trimmed();
        if (!s.isEmpty()) return QStringList{"S:" + s};
        if (ok && !isWVe(ci)) return QStringList{"C:" + ci.country};
        return QStringList{};
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = kBaseRun;
    return d;
}

// ---------------------------------------------------------------- ARRL 10
ContestDef makeArrl10() {
    ContestDef d;
    d.id = "ARRL-10";
    d.title = "ARRL 10 Meter (CW+PH)";
    d.cabrilloName = "ARRL-10";
    d.modeCategory = "MIXED";
    d.dupe = DupeScope::PerBandMode;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "MI";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true, ""},
        {ExchCol::Exch1, "ST/NR", 6, "", true, "state"},
    };
    d.points = [](const CQsoValues& q, const CtyInfo&, bool,
                  const ContestContext&) {
        return q.mode == QLatin1String("CW") ? 4 : 2;
    };
    // Approximation: mult = copied exchange per mode (states, provinces,
    // Mexican states, DX serials collapse to countries on the robot).
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        bool serialLike = true;
        for (QChar ch : q.exch1)
            if (!ch.isDigit()) { serialLike = false; break; }
        if (serialLike && ok)
            return QStringList{"C:" + ci.country + "|" + q.mode};
        const QString s = q.exch1.trimmed();
        return s.isEmpty() ? QStringList{}
                           : QStringList{"X:" + s + "|" + q.mode};
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = kBaseRun;
    return d;
}

// ----------------------------------------------------------------- NAQP
ContestDef makeNaqp(bool cw) {
    ContestDef d;
    d.id = cw ? "NAQP-CW" : "NAQP-SSB";
    d.title = cw ? "NAQP CW" : "NAQP SSB";
    d.cabrilloName = d.id;
    d.modeCategory = cw ? "CW" : "SSB";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = false;
    d.sentExchDefault = "Jon MI";
    d.fields = {
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
        {ExchCol::Exch2, "STATE", 5, "", false, "state"},
    };
    d.points = one;                 // everything is a point in NAQP
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        // NA stations mult by state/province per band.
        const QString s = q.exch2.trimmed();
        if (!s.isEmpty() && ok && ci.cont == QLatin1String("NA"))
            return QStringList{"S:" + s + "|" + q.band};
        return QStringList{};
    };
    d.cabExch = {"exch"};
    d.fkeyRun = cw ? withOverrides(kBaseRun, {
                         {1, "CQ|cq naqp {MYCALL}"},
                         {3, "Exch|{EXCH}"},
                         {8, "NAME?|name?"},
                     })
                   : withOverrides(kBaseRun, kVoiceRun);
    return d;
}

// ------------------------------------------------------------- NA Sprint
ContestDef makeSprint() {
    ContestDef d;
    d.id = "NA-SPRINT-CW";
    d.title = "NA Sprint CW";
    d.cabrilloName = "NA-SPRINT-CW";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "Jon MI";
    d.fields = {
        {ExchCol::SerialR, "NR", 6, "", true, ""},
        {ExchCol::Exch1, "NAME", 10, "", true, "name"},
        {ExchCol::Exch2, "STATE", 5, "", true, "state"},
    };
    d.points = one;
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        const QString s = q.exch2.trimmed();
        return s.isEmpty() ? QStringList{} : QStringList{"S:" + s};
    };
    d.cabExch = {"serial", "exch"};
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq na {MYCALL}"},
        {3, "Exch|{SENTNR} {EXCH}"},
    });
    return d;
}

// ---------------------------------------------------------------- IARU HF
ContestDef makeIaru() {
    ContestDef d;
    d.id = "IARU-HF";
    d.title = "IARU HF Championship (CW+PH)";
    d.cabrilloName = "IARU-HF";
    d.modeCategory = "MIXED";
    d.dupe = DupeScope::PerBandMode;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "8";        // my ITU zone
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true, ""},
        {ExchCol::Exch1, "ZONE/HQ", 6, "", true, "", "ituz"},
    };
    // Same ITU zone (and HQ stations) 1, same continent 3, other
    // continent 5.
    d.points = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                  const ContestContext& ctx) {
        bool zoneNumeric = !q.exch1.isEmpty();
        for (QChar ch : q.exch1)
            if (!ch.isDigit()) { zoneNumeric = false; break; }
        if (!zoneNumeric) return 1;              // HQ / official
        if (q.exch1.toInt() == ctx.myItu) return 1;
        if (!ok) return 5;
        return ci.cont == ctx.myCont ? 3 : 5;
    };
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        const QString z = q.exch1.trimmed();
        return z.isEmpty() ? QStringList{}
                           : QStringList{"Z:" + z + "|" + q.band};
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = kBaseRun;
    return d;
}

// ---------------------------------------------------------------- CQ 160
ContestDef makeCq160() {
    ContestDef d;
    d.id = "CQ-160-CW";
    d.title = "CQ 160 Meter CW";
    d.cabrilloName = "CQ-160-CW";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "MI";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true, ""},
        {ExchCol::Exch1, "ST/CTY", 6, "", false, "state"},
    };
    d.points = [](const CQsoValues&, const CtyInfo& ci, bool ok,
                  const ContestContext& ctx) {
        if (!ok) return 5;
        if (ci.country == ctx.myCountry) return 2;
        return ci.cont == ctx.myCont ? 5 : 10;
    };
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        const QString s = q.exch1.trimmed();
        if (!s.isEmpty() && ok && isWVe(ci))
            return QStringList{"S:" + s};
        if (ok) return QStringList{"C:" + ci.country};
        return QStringList{};
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = kBaseRun;
    return d;
}

// ------------------------------------------------------------------ MIQP
// From the Michigan side: serial + county; out-of-state gives state.
ContestDef makeMiqp() {
    ContestDef d;
    d.id = "MIQP";
    d.title = "Michigan QSO Party (CW+PH)";
    d.cabrilloName = "MIQP";
    d.modeCategory = "MIXED";
    d.dupe = DupeScope::PerBandMode;
    d.hasRst = false;
    d.sentSerial = true;
    d.sentExchDefault = "";         // your county code, e.g. "SAGI"
    d.fields = {
        {ExchCol::SerialR, "NR", 6, "", true, ""},
        {ExchCol::Exch1, "CNTY/ST", 6, "", true, ""},
    };
    d.points = [](const CQsoValues& q, const CtyInfo&, bool,
                  const ContestContext&) {
        return q.mode == QLatin1String("CW") ? 2 : 1;
    };
    d.mults = [](const CQsoValues& q, const CtyInfo&, bool,
                 const ContestContext&) {
        const QString s = q.exch1.trimmed();
        return s.isEmpty() ? QStringList{}
                           : QStringList{"X:" + s + "|" + q.mode};
    };
    d.cabExch = {"serial", "exch"};
    d.cutNumbers = false;           // QSO-party crowd copies plain digits
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq miqp {MYCALL}"},
        {3, "Exch|{SENTNR} {EXCH}"},
    });
    return d;
}

// ------------------------------------------------------------- All Asian
// JARL's age contest: RST + operator's age, non-Asian side works Asia
// only, mults are Asian WPX prefixes per band. Points encode the
// NON-Asian table (160=3, 80=2, 10=2, middle bands 1 — JARL's
// hard-band bonus); an Asian entrant would need the mirror table.
ContestDef makeAllAsian(bool cw) {
    ContestDef d;
    d.id = cw ? "AADX-CW" : "AADX-SSB";
    d.title = cw ? "All Asian DX CW" : "All Asian DX Phone";
    d.cabrilloName = d.id;
    d.modeCategory = cw ? "CW" : "SSB";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = false;
    d.sentExchDefault = "";         // your AGE — JARL wants the digits
    d.fields = {
        {ExchCol::RstR, "RCV", 4, cw ? "599" : "59", true, ""},
        {ExchCol::Exch1, "AGE", 3, "", true, ""},
    };
    const auto valid = [](const CtyInfo& ci, bool ok,
                          const ContestContext& ctx) {
        if (!ok) return false;
        const bool theirsAs = ci.cont == QLatin1String("AS");
        const bool mineAs = ctx.myCont == QLatin1String("AS");
        return theirsAs != mineAs;
    };
    d.points = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                       const ContestContext& ctx) {
        if (!valid(ci, ok, ctx)) return 0;
        if (q.band == QLatin1String("160M")) return 3;
        if (q.band == QLatin1String("80M")) return 2;
        if (q.band == QLatin1String("10M")) return 2;
        return 1;
    };
    d.mults = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                      const ContestContext& ctx) {
        if (!valid(ci, ok, ctx)) return QStringList{};
        const QString p = wpxPrefix(q.call);
        return p.isEmpty()
                   ? QStringList{}
                   : QStringList{"P:" + p + "|" + q.band};
    };
    d.cabExch = {"rst", "exch"};
    d.fkeyRun = cw ? withOverrides(kBaseRun, {
                         {1, "CQ|cq aa {MYCALL} {MYCALL}"},
                     })
                   : withOverrides(kBaseRun, kVoiceRun);
    return d;
}

// -------------------------------------------------------------- WAE DX CW
ContestDef makeWaeCw() {
    ContestDef d;
    d.id = "DARC-WAEDC-CW";
    d.title = "WAE DX CW";
    d.cabrilloName = "DARC-WAEDC-CW";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = true;
    d.sentExchDefault = "";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true, ""},
        {ExchCol::SerialR, "RCV NR", 6, "", true, ""},
    };
    // Exactly one side European, and 160 m is not a WAE band. From the US
    // that means EU only; everything else logs at zero.
    const auto valid = [](const CtyInfo& ci, bool ok,
                          const ContestContext& ctx, const QString& band) {
        if (!ok || band == QLatin1String("160M")) return false;
        const bool theirsEu = ci.cont == QLatin1String("EU");
        const bool mineEu = ctx.myCont == QLatin1String("EU");
        return theirsEu != mineEu;
    };
    d.points = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                       const ContestContext& ctx) {
        return valid(ci, ok, ctx, q.band) ? 1 : 0;
    };
    d.mults = [valid](const CQsoValues& q, const CtyInfo& ci, bool ok,
                      const ContestContext& ctx) {
        if (!valid(ci, ok, ctx, q.band)) return QStringList{};
        return QStringList{"C:" + ci.country + "|" + q.band};
    };
    d.multWeight = [](const QString& key) {
        const QString b = bandOf(key);
        if (b == QLatin1String("80M")) return 4;
        if (b == QLatin1String("40M")) return 3;
        return 2;                   // 20/15/10
    };
    d.cabExch = {"rst", "serial"};
    d.hasQtc = true;
    d.fkeyRun = withOverrides(kBaseRun, {
        {1, "CQ|cq wae {MYCALL} {MYCALL} wae"},
        {3, "Exch|{SNT} {SENTNR}"},
        {4, "TU|tu {MYCALL} wae"},
        {6, "QTC?|qtc?"},
        {7, "QRV|qrv"},
        {8, "QSL|qsl"},
        {11, "Serial|{SENTNR}"},
    });
    return d;
}

// -------------------------------------------------- generic RST + serial
// The day-of fallback for a contest with no definition yet: correct
// dupes, a usable log, a parseable generic Cabrillo. Its proper
// definition gets written the week after.
ContestDef makeGeneric() {
    ContestDef d;
    d.id = "GENERIC-SERIAL";
    d.title = "Generic (RST + serial)";
    d.cabrilloName = "UNKNOWN";
    d.modeCategory = "CW";
    d.dupe = DupeScope::PerBand;
    d.hasRst = true;
    d.sentSerial = true;
    d.sentExchDefault = "";
    d.fields = {
        {ExchCol::RstR, "RCV", 4, "599", true, ""},
        {ExchCol::SerialR, "RCV NR", 6, "", false, ""},
    };
    d.points = one;
    d.mults = [](const CQsoValues& q, const CtyInfo& ci, bool ok,
                 const ContestContext&) {
        if (!ok) return QStringList{};
        return QStringList{"C:" + ci.country + "|" + q.band};
    };
    d.cabExch = {"rst", "serial"};
    d.fkeyRun = kBaseRun;
    return d;
}

} // namespace

const QList<const ContestDef*>& contestDefs() {
    static const ContestDef cqwwCw = makeCqWw(true);
    static const ContestDef cqwwSsb = makeCqWw(false);
    static const ContestDef wpxCw = makeWpx(true);
    static const ContestDef wpxSsb = makeWpx(false);
    static const ContestDef dxCw = makeArrlDx(true);
    static const ContestDef dxSsb = makeArrlDx(false);
    static const ContestDef ssCw = makeSs(true);
    static const ContestDef ssPh = makeSs(false);
    static const ContestDef a160 = makeArrl160();
    static const ContestDef a10 = makeArrl10();
    static const ContestDef c160 = makeCq160();
    static const ContestDef naqpCw = makeNaqp(true);
    static const ContestDef naqpSsb = makeNaqp(false);
    static const ContestDef sprint = makeSprint();
    static const ContestDef iaru = makeIaru();
    static const ContestDef aaCw = makeAllAsian(true);
    static const ContestDef aaSsb = makeAllAsian(false);
    static const ContestDef wae = makeWaeCw();
    static const ContestDef miqp = makeMiqp();
    static const ContestDef cwOpen = makeCwOpen();
    static const ContestDef cwt = makeCwt();
    static const ContestDef mst = makeMst();
    static const ContestDef generic = makeGeneric();
    static const QList<const ContestDef*> all = {
        &cqwwCw, &cqwwSsb, &wpxCw, &wpxSsb, &dxCw, &dxSsb,
        &ssCw, &ssPh, &a160, &a10, &c160, &naqpCw, &naqpSsb,
        &sprint, &iaru, &aaCw, &aaSsb, &wae, &miqp, &cwOpen, &cwt, &mst,
        &generic,
    };
    return all;
}

const ContestDef* contestDef(const QString& id) {
    for (const ContestDef* d : contestDefs())
        if (d->id == id) return d;
    return nullptr;
}

} // namespace ttc
