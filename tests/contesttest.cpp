// SPDX-License-Identifier: GPL-2.0-or-later
// Contest-engine test: callsign normalization, serial formatting, dupe
// scopes, per-contest scoring (CQ WW zero-point-own-country, WAE EU-only
// with weighted mults), macro expansion, ContestDb round-trip, and the
// Cabrillo build + parse-back self-check — including the reversed-
// exchange corruption the CW Open robot caught in the old system.
// No hardware, no network, no GUI.
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <cstdio>

#include <QBuffer>

#include "contest/Cabrillo.h"
#include "contest/CallHistory.h"
#include "contest/ContestDb.h"
#include "contest/ContestDef.h"
#include "contest/ContestEngine.h"
#include "util/CtyLookup.h"

using namespace ttc;

static int fails = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("ok    %s\n", what);                               \
        } else {                                                           \
            std::printf("FAIL  %s\n", what);                               \
            ++fails;                                                       \
        }                                                                  \
    } while (0)

static CQsoValues mkq(const QString& call, const QString& band,
                      const QString& exch1 = QString(),
                      const QString& serialR = QString()) {
    CQsoValues v;
    v.call = call;
    v.band = band;
    v.mode = "CW";
    v.rstS = "599";
    v.rstR = "599";
    v.exch1 = exch1;
    v.serialR = serialR;
    return v;
}

static void testNormalize() {
    CHECK(normalizeForCty("N8EM") == "N8EM", "normalize: plain call");
    CHECK(normalizeForCty("DL/N8EM") == "DL", "normalize: DL/N8EM -> DL");
    CHECK(normalizeForCty("N8EM/DL") == "DL", "normalize: N8EM/DL -> DL");
    CHECK(normalizeForCty("W1AW/4") == "W1AW",
          "normalize: bare area digit keeps the call");
    CHECK(normalizeForCty("N8EM/QRP") == "N8EM", "normalize: /QRP tail");
    CHECK(normalizeForCty("F/ON4UN/P") == "F",
          "normalize: portable tail then shortest");
    CHECK(normalizeForCty("VP2M/K9ZZ") == "VP2M",
          "normalize: shortest segment wins");
}

static void testLoggable() {
    CHECK(!loggableCall("TEST"), "loggable: no digit refused");
    CHECK(!loggableCall("W1"), "loggable: too short refused");
    CHECK(!loggableCall("123"), "loggable: no letter refused");
    CHECK(loggableCall("DL1TEST"), "loggable: DL1TEST accepted");
    CHECK(loggableCall("K9A"), "loggable: K9A accepted");
}

static void testSerials() {
    CHECK(formatSerial(1, true, 3) == "TT1", "serial: 1 -> TT1");
    CHECK(formatSerial(190, true, 3) == "1NT", "serial: 190 -> 1NT");
    CHECK(formatSerial(45, true, 3) == "T45", "serial: 45 -> T45");
    CHECK(formatSerial(9, false, 1) == "9", "serial: MST plain digits");
    CHECK(formatSerial(1024, true, 3) == "1T24", "serial: no truncation");
}

static void testDupes() {
    const ContestDef* cwt = contestDef("CW-OPS");
    QList<CQsoValues> log{mkq("N3JT", "20M")};
    CHECK(isDupe(*cwt, log, "N3JT", "20M", "CW"),
          "dupe: same band is a dupe (CWT)");
    CHECK(!isDupe(*cwt, log, "N3JT", "40M", "CW"),
          "dupe: other band is workable (CWT)");
    CHECK(!isDupe(*cwt, log, "N3JTX", "20M", "CW"),
          "dupe: different call is not a dupe");

    ContestDef whole = *cwt;
    whole.dupe = DupeScope::Contest;
    CHECK(isDupe(whole, log, "n3jt", "40M", "CW"),
          "dupe: whole-contest scope, case-insensitive");
}

static void testCtyZones(const CtyLookup& cty) {
    // Per-prefix CQ/ITU overrides must survive the load — this cty.dat
    // carries them on specific calls (K6ACV = zone 4/7, off the US
    // default 5/8). The area prefixes themselves are undifferentiated
    // here, which is exactly why the authoritative fill comes from QRZ.
    CtyInfo k6, us, dl;
    cty.info("K6ACV", k6);
    cty.info("W1XX", us);
    cty.info("DL1ABC", dl);
    CHECK(k6.cq == 4 && k6.itu == 7,
          "cty: per-prefix CQ/ITU override survives the load (K6ACV 4/7)");
    CHECK(us.cq == 5, "cty: US default zone 5 when no override");
    CHECK(dl.cq == 14, "cty: single-zone DX keeps its country zone");

    // US-state CQ zones: the accurate fill when QRZ omits its zone (most
    // US ops leave it blank). The 4/5 line is the Appalachians — the whole
    // Midwest is zone 4. These are the calls that caught real bugs.
    CHECK(CtyLookup::usStateCqZone("NM") == 4, "state: NM (N2IC) is CQ 4");
    CHECK(CtyLookup::usStateCqZone("ID") == 3, "state: ID (WC8T) is CQ 3");
    CHECK(CtyLookup::usStateCqZone("MI") == 4, "state: MI (WG8W, home) is CQ 4");
    CHECK(CtyLookup::usStateCqZone("OH") == 4, "state: OH is CQ 4, not 5");
    CHECK(CtyLookup::usStateCqZone("WI") == 4, "state: WI is CQ 4, not 5");
    CHECK(CtyLookup::usStateCqZone("CA") == 3, "state: CA is CQ 3");
    CHECK(CtyLookup::usStateCqZone("MD") == 5, "state: MD (seaboard) is CQ 5");
    CHECK(CtyLookup::usStateCqZone("GA") == 5, "state: GA (SE coast) is CQ 5");
    CHECK(CtyLookup::usStateCqZone("AK") == 1, "state: AK is CQ 1");
    CHECK(CtyLookup::usStateCqZone("ZZ") == 0, "state: unknown -> 0 (fall back)");
}

static void testCqWw(const CtyLookup& cty) {
    const ContestDef* d = contestDef("CQ-WW-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    CHECK(cty.info("N8EM", me), "cty: resolves N8EM");
    ctx.myCountry = me.country;
    ctx.myCq = me.cq;
    CHECK(me.cont == "NA", "cty: continent parsed (N8EM -> NA)");

    // Own country 0, VE (same continent, NA) 2, DL (other continent) 3 —
    // and every one of them still claims zone+country mults.
    QList<CQsoValues> log;
    log << mkq("W1AW", "20M", "5") << mkq("VE3AT", "20M", "4")
        << mkq("DL2CC", "20M", "14");
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.points == 0 + 2 + 3, "cqww: 0/2/3 point ladder");
    CHECK(sb.mults == 6, "cqww: 3 zones + 3 countries on one band");
    CHECK(sb.total == qint64(5) * 6, "cqww: score = points x mults");

    // Same countries again on another band are fresh mults.
    log << mkq("DL8WPX", "40M", "14");
    const ScoreBreakdown sb2 = computeScore(*d, log, &cty, ctx);
    CHECK(sb2.mults == 8, "cqww: mults are per band");
}

static void testWae(const CtyLookup& cty) {
    const ContestDef* d = contestDef("DARC-WAEDC-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    cty.info("N8EM", me);
    ctx.myCountry = me.country;

    QList<CQsoValues> log;
    log << mkq("DL1ABC", "40M", "", "456")   // EU, 40 m: 1 pt, mult x3
        << mkq("G4XYZ", "80M", "", "112")    // EU, 80 m: 1 pt, mult x4
        << mkq("W1AW", "20M", "", "7")       // NA-NA: zero
        << mkq("DL2CC", "160M", "", "9")     // 160 m is not a WAE band
        << mkq("DL8WPX", "40M", "", "77");   // Germany 40 m again: no new mult
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.points == 3, "wae: EU-only scoring (W1AW, 160 m zero)");
    CHECK(sb.mults == 2, "wae: two country-band mults");
    CHECK(sb.weightedMults == 3 + 4, "wae: band weights 40x3 + 80x4");
    CHECK(sb.total == 3 * 7, "wae: total = pts x weighted mults");
}

static void testMacros() {
    const ContestDef* wae = contestDef("DARC-WAEDC-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    const QString f3 =
        expandMacro("{SNT} {SENTNR}", *wae, ctx, "DL8WPX", "", 1);
    CHECK(f3 == "5NN TT1", "macro: WAE exchange 5NN TT1");
    const ContestDef* cwt = contestDef("CW-OPS");
    CHECK(expandMacro("{EXCH}", *cwt, ctx, "", "Jon MI", 0) == "Jon MI",
          "macro: CWT sent exchange");
    CHECK(expandMacro("cq wae {MYCALL} {MYCALL} wae", *wae, ctx, "", "", 1)
              == "cq wae N8EM N8EM wae",
          "macro: my call substitution");
}

static void testDb(const QString& dir) {
    ContestDb db;
    CHECK(db.open(dir + "/contest.sqlite"), "db: opens");

    ContestRow c;
    c.defId = "DARC-WAEDC-CW";
    c.title = "WAE DX CW 2026";
    c.startUtc = QDateTime::fromString("2026-08-08 00:00:00",
                                       "yyyy-MM-dd HH:mm:ss");
    c.startUtc.setTimeZone(QTimeZone::utc());
    c.sentExch = "";
    const qint64 cid = db.createContest(c);
    CHECK(cid > 0, "db: contest created");

    ContestQso q;
    q.contestId = cid;
    q.tsUtc = c.startUtc.addSecs(1800);
    q.freqHz = 7024300;
    q.v = mkq("DL1ABC", "40M", "", "456");
    q.v.serialS = 1;
    CHECK(db.addQso(q) > 0, "db: qso added");
    CHECK(db.setNextSerial(cid, 2), "db: serial persisted");
    CHECK(db.contest(cid).nextSerial == 2, "db: serial reads back");

    const QList<ContestQso> rows = db.qsos(cid);
    CHECK(rows.size() == 1 && rows[0].v.call == "DL1ABC"
              && rows[0].v.serialR == "456" && rows[0].v.serialS == 1
              && rows[0].tsUtc.toString("HHmm") == "0030",
          "db: qso round-trips");
    CHECK(db.contests().size() == 1
              && db.contests()[0].defId == "DARC-WAEDC-CW",
          "db: contest list");
}

static void testCabrillo(const CtyLookup& cty) {
    // CW Open: serial-then-name BOTH sides — the exact shape the old
    // system got wrong (received side went out name-then-serial).
    const ContestDef* d = contestDef("CW-OPEN");
    ContestRow c;
    c.defId = d->id;
    c.sentExch = "Jon";
    ContestQso q;
    q.tsUtc = QDateTime::fromString("2026-09-05 12:23:00",
                                    "yyyy-MM-dd HH:mm:ss");
    q.tsUtc.setTimeZone(QTimeZone::utc());
    q.freqHz = 7029000;
    q.v = mkq("N2IC", "40M", "STEVE", "52");
    q.v.serialS = 1;
    QList<ContestQso> qsos{q};

    CabrilloStation st;
    st.call = "N8EM";
    st.name = "Jon Greenwood";
    st.gridLocator = "EN83AL";
    st.location = "MI";
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";

    const QString text = Cabrillo::build(*d, c, qsos, {}, st, &cty, ctx);
    CHECK(text.startsWith("START-OF-LOG: 3.0\r\n"), "cab: header + CRLF");
    CHECK(text.contains("CONTEST: CW-OPEN"), "cab: contest tag");
    CHECK(text.contains("END-OF-LOG:"), "cab: end tag");

    QString qline;
    for (const QString& l : text.split("\r\n"))
        if (l.startsWith("QSO: ")) qline = l;
    CHECK(qline.contains(" 7029 CW 2026-09-05 1223 "), "cab: freq/mode/ts");
    // serial then name, both sides, in order.
    const int sMine = qline.indexOf("001 JON");
    const int sHis = qline.indexOf("052 STEVE");
    CHECK(sMine > 0 && sHis > sMine,
          "cab: serial-name order on BOTH sides (the CW Open bug)");

    QString err;
    const bool selfOk = Cabrillo::selfCheck(text, *d, c, qsos, {}, &err);
    if (!selfOk) std::printf("      self-check said: %s\n", qPrintable(err));
    CHECK(selfOk, "cab: self-check passes");

    // Corrupt the file the way the old bug did — swap the received side
    // to name-then-serial — and the self-check must refuse it.
    QString bad = text;
    bad.replace("052 STEVE", "STEVE 052");
    CHECK(!Cabrillo::selfCheck(bad, *d, c, qsos, {}, &err),
          "cab: reversed exchange is caught");

    // Cut-number copy normalizes on export: TT1 in the box, 001 on file.
    ContestQso q2 = q;
    q2.v.call = "K6RB";
    q2.v.serialR = "TT7";
    QList<ContestQso> qsos2{q, q2};
    const QString t2 = Cabrillo::build(*d, c, qsos2, {}, st, &cty, ctx);
    CHECK(t2.contains("K6RB          007"), "cab: cut numbers uncut on file");
    CHECK(Cabrillo::selfCheck(t2, *d, c, qsos2, {}, nullptr),
          "cab: self-check with uncut serials");
}

static void testSentExchInCab(const CtyLookup& cty) {
    // All Asian's EN-on-every-line lesson: the sent exchange (age) MUST
    // ride on the sent side. With it present, both sides carry age.
    const ContestDef* d = contestDef("AADX-SSB");
    ContestRow c;
    c.defId = d->id;
    c.sentExch = "57";               // the operator's age
    ContestQso q;
    q.tsUtc = QDateTime::fromString("2026-09-06 14:18:00",
                                    "yyyy-MM-dd HH:mm:ss");
    q.tsUtc.setTimeZone(QTimeZone::utc());
    q.freqHz = 21302000;
    q.v = mkq("JI2MED", "15M", "45");
    q.v.mode = "SSB";
    q.v.rstS = q.v.rstR = "59";      // phone report
    QList<ContestQso> qsos{q};
    CabrilloStation st;
    st.call = "N8EM";
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    const QString text = Cabrillo::build(*d, c, qsos, {}, st, &cty, ctx);
    QString qline;
    for (const QString& l : text.split("\r\n"))
        if (l.startsWith("QSO: ")) qline = l;
    const int sent = qline.indexOf("59 57");   // my RST + my age
    const int call = qline.indexOf("JI2MED");
    const int rcvd = qline.indexOf("59 45");    // his RST + his age
    CHECK(sent > 0 && call > sent && rcvd > call,
          "aadx cab: sent RST+age, then call, then rcvd RST+age");
    // The EMAIL: header the sponsors require.
    st.email = "jon@example.com";
    const QString t2 = Cabrillo::build(*d, c, qsos, {}, st, &cty, ctx);
    CHECK(t2.contains("EMAIL: jon@example.com"),
          "aadx cab: EMAIL header present when the station has one");
}

static void testQtc(const QString& dir, const CtyLookup& cty) {
    ContestDb db;
    CHECK(db.open(dir + "/qtc.sqlite"), "qtc: db opens");
    ContestRow c;
    c.defId = "DARC-WAEDC-CW";
    c.title = "WAE QTC test";
    c.startUtc = QDateTime::fromString("2026-08-08 00:00:00",
                                       "yyyy-MM-dd HH:mm:ss");
    c.startUtc.setTimeZone(QTimeZone::utc());
    const qint64 cid = db.createContest(c);

    // Twelve EU QSOs, oldest first; DL8WPX is the receiving station.
    QList<qint64> ids;
    for (int i = 0; i < 12; ++i) {
        ContestQso q;
        q.contestId = cid;
        q.tsUtc = c.startUtc.addSecs(300 * (i + 1));
        q.freqHz = 7024000;
        q.v = mkq(i == 2 ? "DL8WPX" : QString("DL%1AA").arg(i), "40M", "",
                  QString::number(100 + i));
        q.v.serialS = i + 1;
        ids << db.addQso(q);
    }
    const QList<ContestQso> qsos = db.qsos(cid);

    // Allocation: 10 oldest, never the receiving station itself.
    QList<qint64> alloc = allocateQtc(qsos, db.qtcReportedQsoIds(cid),
                                      "DL8WPX", 0);
    CHECK(alloc.size() == 10, "qtc: block caps at 10");
    CHECK(!alloc.contains(ids[2]),
          "qtc: never reports a QSO back to the station it was made with");
    CHECK(alloc.first() == ids[0], "qtc: oldest first");

    // Cumulative per-station limit: 7 already sent leaves room for 3.
    CHECK(allocateQtc(qsos, {}, "DL8WPX", 7).size() == 3,
          "qtc: per-station limit is cumulative");
    CHECK(allocateQtc(qsos, {}, "DL8WPX", 10).isEmpty(),
          "qtc: station at 10 gets nothing");

    // Confirm the block; those QSOs are spent forever.
    CHECK(db.addQtcBlock(cid, "DL8WPX", db.nextQtcBlock(cid), alloc,
                         7028000, "CW"),
          "qtc: block confirms");
    CHECK(db.qtcCount(cid) == 10 && db.qtcSentTo(cid, "DL8WPX") == 10,
          "qtc: counters track");
    const QList<qint64> again = allocateQtc(
        qsos, db.qtcReportedQsoIds(cid), "G4XYZ", 0);
    CHECK(again.size() == 2,
          "qtc: reported QSOs never allocate again (2 left of 12)");

    // The frozen-QSO rules, enforced by the database and the API.
    CHECK(!db.deleteQso(alloc.first()),
          "qtc: reported QSO cannot be deleted (FK RESTRICT)");
    ContestQso ed = qsos[0];
    ed.v.serialR = "999";
    CHECK(!db.updateQso(ed), "qtc: reported QSO cannot be edited");
    CHECK(db.nextQtcBlock(cid) == 2, "qtc: block numbering advances");

    // Cabrillo: interleaved, receiver first, verified by parse-back.
    CabrilloStation st;
    st.call = "N8EM";
    st.gridLocator = "EN83AL";
    st.location = "MI";
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    cty.info("N8EM", me);
    ctx.myCountry = me.country;
    const ContestDef* d = contestDef("DARC-WAEDC-CW");
    const ContestRow row = db.contest(cid);
    const QList<ContestDb::QtcRow> qtcs = db.qtcRows(cid);
    const QString text =
        Cabrillo::build(*d, row, qsos, qtcs, st, &cty, ctx);
    CHECK(text.count("QTC: ") == 10, "qtc cab: ten QTC lines");
    CHECK(text.contains("DL8WPX        1/10"),
          "qtc cab: receiver first, then series block/count");
    QString err;
    const bool ok = Cabrillo::selfCheck(text, *d, row, qsos, qtcs, &err);
    if (!ok) std::printf("      qtc self-check said: %s\n", qPrintable(err));
    CHECK(ok, "qtc cab: parse-back verifies");
    QString bad = text;
    // Swap receiver and sender on one QTC line — the classic drift.
    bad.replace("DL8WPX        1/10      N8EM",
                "N8EM          1/10      DL8WPX");
    CHECK(!Cabrillo::selfCheck(bad, *d, row, qsos, qtcs, &err),
          "qtc cab: swapped receiver/sender is caught");
    // Score: (12 QSO pts + 10 QTC) × weighted mults.
    QList<CQsoValues> vals;
    for (const ContestQso& q : qsos) vals << q.v;
    const ScoreBreakdown sb =
        computeScore(*d, vals, &cty, ctx, db.qtcCount(cid));
    CHECK(sb.qtcPoints == 10 && sb.total == (sb.points + 10)
              * sb.weightedMults,
          "qtc: score adds QTC points before the multiplier");
}

static void testWpx(const CtyLookup& cty) {
    CHECK(wpxPrefix("N8EM") == "N8", "wpx: N8EM -> N8");
    CHECK(wpxPrefix("WA3ABC") == "WA3", "wpx: WA3ABC -> WA3");
    CHECK(wpxPrefix("4X4AA") == "4X4", "wpx: 4X4AA -> 4X4");
    CHECK(wpxPrefix("DL/N8EM") == "DL0",
          "wpx: designator without a digit gains a 0");
    CHECK(wpxPrefix("N8EM/DL") == "DL0", "wpx: designator on either side");
    CHECK(wpxPrefix("W1AW/4") == "W4",
          "wpx: bare-digit designator swaps the home number");
    CHECK(wpxPrefix("KH6/N8EM") == "KH6", "wpx: KH6/N8EM -> KH6");
    CHECK(wpxPrefix("N8EM/QRP") == "N8", "wpx: /QRP tail ignored");

    const ContestDef* d = contestDef("CQ-WPX-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    cty.info("N8EM", me);
    ctx.myCountry = me.country;
    QList<CQsoValues> log;
    log << mkq("DL2CC", "20M", "", "1")     // other cont, high: 3
        << mkq("DL8WPX", "40M", "", "2")    // other cont, low: 6
        << mkq("VE3AT", "40M", "", "3")     // NA-NA, low: 4
        << mkq("W1AW", "20M", "", "4")      // same country: 1
        << mkq("N8XYZ", "40M", "", "5");    // same country low: still 1
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.points == 3 + 6 + 4 + 1 + 1, "wpx: point ladder incl. low bands");
    CHECK(sb.mults == 5, "wpx: five distinct prefixes");
    // Same prefix on another band adds nothing.
    log << mkq("W1AW", "40M", "", "6");
    CHECK(computeScore(*d, log, &cty, ctx).mults == 5,
          "wpx: prefixes count once for the whole contest");
}

static void testSs(const CtyLookup& cty) {
    const ContestDef* d = contestDef("ARRL-SS-CW");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    QList<CQsoValues> log;
    CQsoValues q = mkq("W1AW", "20M");
    q.serialR = "123";
    q.exch1 = "B";       // prec
    q.exch2 = "55";      // check
    q.exch3 = "CT";      // section
    log << q;
    CHECK(isDupe(*d, log, "W1AW", "40M", "CW"),
          "ss: whole-contest dupe — other band still a dupe");
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.points == 2 && sb.mults == 1, "ss: 2 points, section mult");
    CQsoValues q2 = q;
    q2.call = "K1ZZ";
    q2.exch3 = "CT";
    log << q2;
    CHECK(computeScore(*d, log, &cty, ctx).mults == 1,
          "ss: sections count once, ever");

    // Cabrillo: the four received values ride the exch token in order.
    ContestRow c;
    c.defId = d->id;
    c.sentExch = "A 71 MI";
    ContestQso cq;
    cq.tsUtc = QDateTime::fromString("2026-11-01 21:02:00",
                                     "yyyy-MM-dd HH:mm:ss");
    cq.tsUtc.setTimeZone(QTimeZone::utc());
    cq.freqHz = 21042000;
    cq.v = q;
    cq.v.serialS = 17;
    QList<ContestQso> qsos{cq};
    CabrilloStation st;
    st.call = "N8EM";
    const QString text = Cabrillo::build(*d, c, qsos, {}, st, &cty, ctx);
    CHECK(text.contains("017 A 71 MI") && text.contains("123 B 55 CT"),
          "ss cab: serial + prec/ck/sec both sides, in order");
    CHECK(Cabrillo::selfCheck(text, *d, c, qsos, {}, nullptr),
          "ss cab: parse-back verifies");
}

static void testRoster(const CtyLookup& cty) {
    CHECK(contestDefs().size() == 23, "roster: 23 definitions registered");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    ctx.myItu = 8;
    CtyInfo me;
    cty.info("N8EM", me);
    ctx.myCountry = me.country;

    // ARRL DX from the W side: DX 3 points, W/VE zero, country per band.
    {
        const ContestDef* d = contestDef("ARRL-DX-CW");
        QList<CQsoValues> log;
        log << mkq("DL2CC", "20M", "KW") << mkq("VE3AT", "20M", "100");
        const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
        CHECK(sb.points == 3 && sb.mults == 1,
              "arrl-dx: VE is zero points and no mult from the W side");
    }
    // NAQP: everything a point; only NA states multiply.
    {
        const ContestDef* d = contestDef("NAQP-CW");
        QList<CQsoValues> log;
        CQsoValues a = mkq("VE3AT", "20M", "BOB");
        a.exch2 = "ON";
        CQsoValues b = mkq("DL2CC", "20M", "HANS");  // no state, still 1 pt
        log << a << b;
        const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
        CHECK(sb.points == 2 && sb.mults == 1,
              "naqp: DX pays a point but no mult");
    }
    // Sprint: state mults count once across bands.
    {
        const ContestDef* d = contestDef("NA-SPRINT-CW");
        QList<CQsoValues> log;
        CQsoValues a = mkq("N6RO", "20M", "KEN");
        a.exch2 = "CA";
        a.serialR = "1";
        CQsoValues b = a;
        b.call = "K6XX";
        b.band = "40M";
        b.serialR = "2";
        log << a << b;
        CHECK(computeScore(*d, log, &cty, ctx).mults == 1,
              "sprint: CA counts once, not per band");
    }
    // IARU: zone ladder 1 / 3 / 5, HQ strings pay 1.
    {
        const ContestDef* d = contestDef("IARU-HF");
        QList<CQsoValues> log;
        log << mkq("W9ZZZ", "20M", "8")     // my zone: 1
            << mkq("VE3AT", "20M", "4")     // same continent: 3
            << mkq("DL2CC", "20M", "28")    // other continent: 5
            << mkq("DA0HQ", "20M", "DARC"); // HQ: 1
        CHECK(computeScore(*d, log, &cty, ctx).points == 1 + 3 + 5 + 1,
              "iaru: zone-distance point ladder + HQ");
    }
    // CQ 160: 2 / 5 / 10 ladder; W/VE state mult, DX country mult.
    {
        const ContestDef* d = contestDef("CQ-160-CW");
        QList<CQsoValues> log;
        log << mkq("W1AW", "160M", "CT") << mkq("XE2X", "160M", "")
            << mkq("DL2CC", "160M", "");
        const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
        CHECK(sb.points == 2 + 5 + 10, "cq160: point ladder");
        CHECK(sb.mults == 3, "cq160: state + two countries");
    }
    // All Asian from the NA side: Asia only, age exchange, hard-band
    // bonus points, Asian WPX prefixes per band.
    {
        const ContestDef* d = contestDef("AADX-SSB");
        QList<CQsoValues> log;
        CQsoValues ja = mkq("JA1ABC", "20M", "48");
        ja.mode = "SSB";
        CQsoValues ja10 = mkq("JA1XYZ", "10M", "22");
        ja10.mode = "SSB";
        CQsoValues hl80 = mkq("HL4CJG", "80M", "31");
        hl80.mode = "SSB";
        CQsoValues dl = mkq("DL2CC", "20M", "50");   // not Asia: zero
        dl.mode = "SSB";
        log << ja << ja10 << hl80 << dl;
        const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
        CHECK(sb.points == 1 + 2 + 2 + 0,
              "aadx: Asia-only with the hard-band bonus (10m/80m pay 2)");
        CHECK(sb.mults == 3, "aadx: JA1/JA1/HL4 prefixes per band = 3");
        CQsoValues ja15 = mkq("JA1DEF", "15M", "60");
        ja15.mode = "SSB";
        log << ja15;
        CHECK(computeScore(*d, log, &cty, ctx).mults == 4,
              "aadx: same prefix on another band is a fresh mult");
    }
    // ARRL 10: CW pays double, dupes are per band+mode.
    {
        const ContestDef* d = contestDef("ARRL-10");
        QList<CQsoValues> log;
        CQsoValues cw = mkq("W1AW", "10M", "CT");
        CQsoValues ph = cw;
        ph.mode = "SSB";
        log << cw << ph;
        CHECK(computeScore(*d, log, &cty, ctx).points == 4 + 2,
              "arrl10: 4 CW / 2 PH points");
        CHECK(!isDupe(*d, log, "K1ZZ", "10M", "CW")
                  && isDupe(*d, log, "W1AW", "10M", "SSB"),
              "arrl10: dupes are per mode");
    }
    // Every definition survives a Cabrillo round-trip with a generic QSO.
    {
        for (const ContestDef* d : contestDefs()) {
            ContestRow c;
            c.defId = d->id;
            c.sentExch = "TEST EX";
            ContestQso q;
            q.tsUtc = QDateTime::fromString("2026-01-01 00:00:00",
                                            "yyyy-MM-dd HH:mm:ss");
            q.tsUtc.setTimeZone(QTimeZone::utc());
            q.freqHz = 14030000;
            q.v = mkq("DL1ABC", "20M", "EX1", "5");
            q.v.exch2 = "EX2";
            q.v.exch3 = "EX3";
            q.v.serialS = 1;
            q.v.mode = d->modeCategory == QLatin1String("SSB") ? "SSB"
                                                               : "CW";
            QList<ContestQso> qsos{q};
            CabrilloStation st;
            st.call = "N8EM";
            const QString text =
                Cabrillo::build(*d, c, qsos, {}, st, &cty, ctx);
            QString err;
            if (!Cabrillo::selfCheck(text, *d, c, qsos, {}, &err)) {
                std::printf("FAIL  roster cab round-trip: %s — %s\n",
                            qPrintable(d->id), qPrintable(err));
                ++fails;
            }
        }
        CHECK(true, "roster: every definition round-trips its Cabrillo");
    }
}

static void testVoice() {
    CHECK(vkSlot("{VK1}") == 1 && vkSlot("{vk4}") == 4,
          "voice: VK tokens recognized, case-insensitive");
    CHECK(vkSlot("{VK5}") == 0 && vkSlot("5NN MI") == 0
              && vkSlot("cq {VK1}") == 0,
          "voice: out-of-range and CW text are not VK macros");
    const ContestDef* ssb = contestDef("CQ-WW-SSB");
    CHECK(vkSlot(ssb->fkeyRun.value(1).section('|', 1)) == 1
              && vkSlot(ssb->fkeyRun.value(3).section('|', 1)) == 2,
          "voice: CQ WW SSB F1/F3 play VK slots");
    CHECK(ssb->fkeyRun.value(2).section('|', 1).isEmpty()
              && ssb->fkeyRun.value(9).isEmpty(),
          "voice: his-call and CW text keys are blank on phone");
    const ContestDef* cw = contestDef("CQ-WW-CW");
    CHECK(vkSlot(cw->fkeyRun.value(1).section('|', 1)) == 0,
          "voice: the CW twin still keys text");
}

static void testDupeScoring(const CtyLookup& cty) {
    // Six Enters on JI2MED = one QSO's points, not six (live-found).
    const ContestDef* d = contestDef("AADX-SSB");
    ContestContext ctx;
    ctx.myCall = "N8EM";
    ctx.myCont = "NA";
    CtyInfo me;
    cty.info("N8EM", me);
    ctx.myCountry = me.country;
    QList<CQsoValues> log;
    for (int i = 0; i < 6; ++i) {
        CQsoValues q = mkq("JI2MED", "20M", "45");
        q.mode = "SSB";
        log << q;
    }
    const ScoreBreakdown sb = computeScore(*d, log, &cty, ctx);
    CHECK(sb.qsos == 6 && sb.points == 1 && sb.mults == 1,
          "dupes: six identical rows score once");
    // The same station on ANOTHER band is a fresh QSO, not a dupe.
    CQsoValues q15 = mkq("JI2MED", "15M", "45");
    q15.mode = "SSB";
    log << q15;
    CHECK(computeScore(*d, log, &cty, ctx).points == 2,
          "dupes: per-band scope frees the other band");
}

static void testNearMiss() {
    CHECK(nearMissCall("DL2CC", "DL2CE"), "nearmiss: one substitution");
    CHECK(nearMissCall("DL2CC", "DL2C"), "nearmiss: one deletion");
    CHECK(nearMissCall("DL2CC", "DDL2CC"), "nearmiss: one insertion");
    CHECK(!nearMissCall("DL2CC", "DL2CC"), "nearmiss: identical is not");
    CHECK(!nearMissCall("DL2CC", "DL3CE"), "nearmiss: two edits is not");
    CHECK(!nearMissCall("DL2CC", "OK1RR"), "nearmiss: different call");
    CHECK(nearMissCall("N8EM", "N8EN"), "nearmiss: last-letter bust");
}

static void testOpTime() {
    QList<QDateTime> ev;
    QDateTime t = QDateTime::fromString("2026-08-08 00:00:00",
                                        "yyyy-MM-dd HH:mm:ss");
    t.setTimeZone(QTimeZone::utc());
    // 30 min of QSOs, a 59-minute lull (still operating), 10 more
    // minutes, then a 2-hour break, then 5 minutes.
    ev << t << t.addSecs(1800) << t.addSecs(1800 + 3540)
       << t.addSecs(1800 + 3540 + 600)
       << t.addSecs(1800 + 3540 + 600 + 7200)
       << t.addSecs(1800 + 3540 + 600 + 7200 + 300);
    CHECK(opTimeSecs(ev) == 1800 + 3540 + 600 + 300,
          "optime: sub-hour lulls count, a 2-hour break does not");
    CHECK(opTimeSecs({}) == 0 && opTimeSecs({t}) == 0,
          "optime: empty and single-event logs are zero");
}

static bool plansAre(const QList<EsmAct>& got,
                     const QList<EsmAct>& want) {
    return got == want;
}

static void testEsm() {
    using A = EsmAct;
    EsmInput in;

    // ---- Run mode -------------------------------------------------------
    in.run = true;
    CHECK(plansAre(esmPlan(in), {A::KeyCq}), "esm run: empty call -> CQ");

    in.callEmpty = false;               // partial like "DL8" — not loggable
    CHECK(plansAre(esmPlan(in), {A::KeyHisCall, A::KeyExch}),
          "esm run: unloggable call never offers log, asks the fill");

    in.callLoggable = true;
    CHECK(plansAre(esmPlan(in),
                   {A::KeyHisCall, A::KeyExch, A::FocusExch}),
          "esm run: answering Enter keys call+exch and moves the cursor");

    in.exchSent = true;                 // answered; his numbers not yet in
    CHECK(plansAre(esmPlan(in), {}),
          "esm run: answered + incomplete exchange -> Enter waits");

    in.exchComplete = true;
    CHECK(plansAre(esmPlan(in), {A::KeyTu, A::Log}),
          "esm run: closer is TU + log (QRZ slot disabled -> F4)");

    // Exchange typed BEFORE answering: still answer first, never skip.
    in.exchSent = false;
    CHECK(plansAre(esmPlan(in),
                   {A::KeyHisCall, A::KeyExch, A::FocusExch}),
          "esm run: exchange-first ordering still answers first");

    // ---- S&P: the strict three-beat ------------------------------------
    in = EsmInput();
    in.run = false;
    in.callEmpty = false;
    in.callLoggable = true;
    CHECK(plansAre(esmPlan(in), {A::KeyMyCall, A::FocusExch}),
          "esm s&p beat 1: my call, cursor to exchange");

    in.myCallSent = true;
    CHECK(plansAre(esmPlan(in), {A::KeyExch}),
          "esm s&p beat 2: my exchange, once, NOT bundled with log");

    in.exchSent = true;
    CHECK(plansAre(esmPlan(in), {}),
          "esm s&p beat 3 refused while his exchange is missing");

    in.exchComplete = true;
    CHECK(plansAre(esmPlan(in), {A::Log}),
          "esm s&p beat 3: log, silent, nothing keyed");

    // Serial-first order (his numbers before beat 2) — both orders work.
    in = EsmInput();
    in.run = false;
    in.callEmpty = false;
    in.callLoggable = true;
    in.exchComplete = true;
    in.myCallSent = true;
    CHECK(plansAre(esmPlan(in), {A::KeyExch}),
          "esm s&p: serial-first order still keys the exchange once");

    // No CQ from S&P, ever.
    in = EsmInput();
    in.run = false;
    CHECK(plansAre(esmPlan(in), {}), "esm s&p: empty call does nothing");

    // Unloggable call blocks beat 3 but not beats 1-2.
    in.callEmpty = false;
    in.callLoggable = false;
    in.myCallSent = true;
    in.exchSent = true;
    in.exchComplete = true;
    CHECK(plansAre(esmPlan(in), {}),
          "esm s&p: unloggable call never logs");

    // ESM off = plain Enter-logs.
    in = EsmInput();
    in.esmOn = false;
    in.callEmpty = false;
    in.callLoggable = true;
    in.exchComplete = true;
    CHECK(plansAre(esmPlan(in), {A::Log}), "esm off: plain log");
    in.exchComplete = false;
    CHECK(plansAre(esmPlan(in), {}), "esm off: incomplete does nothing");
}

static void testHistoryParser() {
    // The corrected-header CWops shape: number lands in Exch1.
    QByteArray good =
        "!!Order!!,Call,Name,Exch1,UserText\n"
        "# comment\n"
        "N3JT,Jim,1,VA\n"
        "K6RB,Rob,3,CA\n";
    QBuffer b(&good);
    b.open(QIODevice::ReadOnly);
    QString err;
    const auto rows = parseCallHistory(b, &err);
    CHECK(rows.size() == 2 && rows[0].call == "N3JT"
              && rows[0].name == "Jim" && rows[0].exch1 == "1"
              && rows[0].userText == "VA",
          "history: corrected CWops header parses (number in Exch1)");

    // The published file's actual defect: two columns named Misc.
    QByteArray dup =
        "!!Order!!,Call,Name,Misc,State,Misc\n"
        "N3JT,Jim,1,VA,x\n";
    QBuffer d(&dup);
    d.open(QIODevice::ReadOnly);
    const auto none = parseCallHistory(d, &err);
    CHECK(none.isEmpty() && err.contains("duplicate column"),
          "history: duplicate header column is REFUSED, with the reason");

    QByteArray noCall = "Name,Exch1\nJim,1\n";
    QBuffer n(&noCall);
    n.open(QIODevice::ReadOnly);
    CHECK(parseCallHistory(n, &err).isEmpty() && err.contains("Call"),
          "history: missing Call column refused");
}

static void testScp() {
    const QSet<QString> scp = {"DL8WPX", "DL8WAA", "DL8WX", "N3JT",
                               "W8DL8", "K6RB"};
    const QStringList m = scpMatches("DL8", scp);
    CHECK(m.size() == 4, "scp: prefix + substring matches found");
    CHECK(m[0] == "DL8WAA" && m[1] == "DL8WPX" && m[2] == "DL8WX",
          "scp: prefix matches first, alphabetical");
    CHECK(m[3] == "W8DL8", "scp: substring matches after");
    CHECK(scpMatches("D", scp).isEmpty(), "scp: one char is too little");
}

static void testHistoryDb(const QString& dir) {
    ContestDb db;
    CHECK(db.open(dir + "/hist.sqlite"), "historydb: opens");
    QList<HistoryRow> rows;
    HistoryRow r;
    r.call = "n3jt";
    r.name = "Jim";
    r.exch1 = "1";
    rows << r;
    r.call = "N3JT";                    // upsert replaces, not duplicates
    r.name = "JIM";
    rows << r;
    CHECK(db.importCallHistory(rows) == 2, "historydb: import lands");
    CHECK(db.historyCount() == 1, "historydb: upsert by call");
    CHECK(db.historyFor("N3JT").name == "JIM", "historydb: lookup");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;

    CtyLookup cty;
    bool ctyOk = false;
    for (const char* p : {"resources/cty.dat", "../resources/cty.dat",
                          "../../resources/cty.dat"})
        if (cty.load(QString::fromLatin1(p))) { ctyOk = true; break; }
    if (!ctyOk) {
        std::printf("FAIL  cannot find resources/cty.dat (run from repo "
                    "or build dir)\n");
        return 1;
    }

    testNormalize();
    testLoggable();
    testSerials();
    testDupes();
    testCtyZones(cty);
    testCqWw(cty);
    testWae(cty);
    testMacros();
    testEsm();
    testHistoryParser();
    testScp();
    testDb(tmp.path());
    testHistoryDb(tmp.path());
    testCabrillo(cty);
    testSentExchInCab(cty);
    testQtc(tmp.path(), cty);
    testOpTime();
    testWpx(cty);
    testSs(cty);
    testRoster(cty);
    testVoice();
    testNearMiss();
    testDupeScoring(cty);

    std::printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
