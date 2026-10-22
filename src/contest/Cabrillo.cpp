// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/Cabrillo.h"

#include <QHash>
#include <algorithm>

#include "contest/ContestEngine.h"
#include "util/CtyLookup.h"

namespace ttc {

namespace {
// Column layout of one QSO: line. Fixed slices so selfCheck can parse
// positionally — a multi-word exchange ("Jon MI") stays one field.
//   QSO: FFFFF MO yyyy-MM-dd HHmm MYCALL........ SENTEXCH...... HISCALL....... RCVDEXCH......
constexpr int kCallW = 13;
constexpr int kExchW = 14;

QString pad(const QString& s, int w) {
    QString t = s;
    if (t.size() < w) t += QString(w - t.size(), ' ');
    return t;
}

// "TT1" -> "001": the air uses cut numbers, the robot wants digits. Only
// the unambiguous letters translate; anything still non-numeric is
// emitted as copied — the log checker judges evidence, we don't invent.
QString uncut(const QString& s) {
    QString t = s.trimmed().toUpper();
    t.replace('T', '0');
    t.replace('O', '0');
    t.replace('N', '9');
    bool numeric = !t.isEmpty();
    for (QChar c : t)
        if (!c.isDigit()) { numeric = false; break; }
    if (!numeric) return s.trimmed().toUpper();
    while (t.size() < 3) t.prepend('0');
    return t;
}

QString modeToken(const QString& mode) {
    if (mode == QLatin1String("CW")) return "CW";
    if (mode == QLatin1String("SSB") || mode == QLatin1String("USB")
        || mode == QLatin1String("LSB")) return "PH";
    if (mode == QLatin1String("RTTY")) return "RY";
    return "DG";
}

void header(QString& out, const QString& tag, const QString& val) {
    out += tag + ": " + val + "\r\n";
}
} // namespace

QString Cabrillo::freqField(qint64 hz) {
    return QString::number(hz / 1000).rightJustified(5);
}

QStringList Cabrillo::exchTokens(const ContestDef& def,
                                 const ContestRow& contest,
                                 const ContestQso& q, bool sentSide) {
    QStringList out;
    for (const QString& tok : def.cabExch) {
        if (tok == QLatin1String("rst")) {
            // Empty-field fallback follows the QSO's mode: 59 on phone.
            const char* dflt =
                q.v.mode == QLatin1String("SSB") ? "59" : "599";
            out << (sentSide ? (q.v.rstS.isEmpty() ? dflt : q.v.rstS)
                             : (q.v.rstR.isEmpty() ? dflt : q.v.rstR));
        } else if (tok == QLatin1String("serial")) {
            out << (sentSide
                        ? QString::number(q.v.serialS).rightJustified(3, '0')
                        : uncut(q.v.serialR));
        } else if (tok == QLatin1String("exch")) {
            if (sentSide) {
                out << contest.sentExch.trimmed().toUpper();
            } else {
                QStringList parts;
                for (const QString& e :
                     {q.v.exch1, q.v.exch2, q.v.exch3})
                    if (!e.trimmed().isEmpty()) parts << e.trimmed();
                out << parts.join(' ').toUpper();
            }
        }
    }
    return out;
}

QString Cabrillo::build(const ContestDef& def, const ContestRow& contest,
                        const QList<ContestQso>& qsos,
                        const QList<ContestDb::QtcRow>& qtcs,
                        const CabrilloStation& st, const CtyLookup* cty,
                        const ContestContext& ctx) {
    // Claimed score is recomputed here, never taken from the screen.
    QList<CQsoValues> vals;
    for (const ContestQso& q : qsos) vals << q.v;
    const ScoreBreakdown sb =
        computeScore(def, vals, cty, ctx, int(qtcs.size()));

    QString out;
    header(out, "START-OF-LOG", "3.0");
    header(out, "CONTEST", def.cabrilloName);
    header(out, "CALLSIGN", st.call.toUpper());
    if (!st.location.isEmpty()) header(out, "LOCATION", st.location);
    header(out, "CATEGORY-OPERATOR", contest.catOp);
    header(out, "CATEGORY-ASSISTED", contest.catAssisted);
    header(out, "CATEGORY-BAND", contest.catBand);
    header(out, "CATEGORY-POWER", contest.catPower);
    header(out, "CATEGORY-MODE", contest.catMode);
    header(out, "CATEGORY-TRANSMITTER", contest.catTx);
    header(out, "CATEGORY-STATION", contest.catStation);
    if (!contest.catOverlay.isEmpty())
        header(out, "CATEGORY-OVERLAY", contest.catOverlay);
    header(out, "CLAIMED-SCORE", QString::number(sb.total));
    header(out, "OPERATORS", st.call.toUpper());
    if (!st.gridLocator.isEmpty())
        header(out, "GRID-LOCATOR", st.gridLocator.toUpper());
    if (!st.club.isEmpty()) header(out, "CLUB", st.club);
    if (!st.name.isEmpty()) header(out, "NAME", st.name);
    if (!st.address.isEmpty()) header(out, "ADDRESS", st.address);
    if (!st.email.isEmpty()) header(out, "EMAIL", st.email);
    header(out, "CREATED-BY", "tentec-console");
    header(out, "SOAPBOX", contest.soapbox);

    // QSO and QTC lines interleave chronologically (a QTC's timestamp is
    // its confirm time), the order the DARC robot expects.
    struct Line {
        QDateTime ts;
        QString text;
    };
    QList<Line> lines;
    for (const ContestQso& q : qsos) {
        const QDateTime ts = q.tsUtc.toUTC();
        QString t = "QSO: " + freqField(q.freqHz) + ' '
             + pad(modeToken(q.v.mode), 2) + ' '
             + ts.toString("yyyy-MM-dd") + ' ' + ts.toString("HHmm") + ' '
             + pad(st.call.toUpper(), kCallW) + ' '
             + pad(exchTokens(def, contest, q, true).join(' '), kExchW) + ' '
             + pad(q.v.call.toUpper(), kCallW) + ' '
             + pad(exchTokens(def, contest, q, false).join(' '), kExchW);
        lines.push_back({ts, t});
    }
    if (!qtcs.isEmpty()) {
        QHash<qint64, const ContestQso*> byId;
        for (const ContestQso& q : qsos) byId.insert(q.id, &q);
        QHash<int, int> blockCount;          // block -> items in it
        for (const ContestDb::QtcRow& r : qtcs) blockCount[r.block]++;
        for (const ContestDb::QtcRow& r : qtcs) {
            const ContestQso* q = byId.value(r.qsoId);
            if (!q) continue;                // selfCheck will scream
            const QDateTime ts = r.tsUtc.toUTC();
            // Receiver's call FIRST, then series, then us, then the
            // reported QSO's time / call / serial-as-received.
            QString t = "QTC: " + freqField(r.freqHz) + ' '
                 + pad(modeToken(r.mode), 2) + ' '
                 + ts.toString("yyyy-MM-dd") + ' ' + ts.toString("HHmm")
                 + ' ' + pad(r.toCall.toUpper(), kCallW) + ' '
                 + pad(QString("%1/%2").arg(r.block)
                           .arg(blockCount[r.block]), 9) + ' '
                 + pad(st.call.toUpper(), kCallW) + ' '
                 + q->tsUtc.toUTC().toString("HHmm") + ' '
                 + pad(q->v.call.toUpper(), kCallW) + ' '
                 + uncut(q->v.serialR);
            lines.push_back({ts, t});
        }
        std::stable_sort(lines.begin(), lines.end(),
                         [](const Line& a, const Line& b) {
                             return a.ts < b.ts;
                         });
    }
    for (const Line& l : lines) {
        QString t = l.text;
        while (t.endsWith(' ')) t.chop(1);
        out += t + "\r\n";
    }
    header(out, "END-OF-LOG", "");
    return out;
}

bool Cabrillo::selfCheck(const QString& text, const ContestDef& def,
                         const ContestRow& contest,
                         const QList<ContestQso>& qsos,
                         const QList<ContestDb::QtcRow>& qtcs,
                         QString* err) {
    // Slice each QSO: line by the fixed columns build() used and compare
    // every field to what the DATABASE row says it should be. Catches a
    // side swap, a wrong token order, a timestamp bug — the class of
    // mistake a sponsor's robot finds weeks too late.
    QList<QString> lines, qtcLines;
    for (const QString& l : text.split("\r\n")) {
        if (l.startsWith("QSO: ")) lines << l;
        if (l.startsWith("QTC: ")) qtcLines << l;
    }
    if (lines.size() != qsos.size()) {
        if (err)
            *err = QString("QSO line count %1 != database %2")
                       .arg(lines.size()).arg(qsos.size());
        return false;
    }
    if (qtcLines.size() != qtcs.size()) {
        if (err)
            *err = QString("QTC line count %1 != database %2")
                       .arg(qtcLines.size()).arg(qtcs.size());
        return false;
    }
    // Every QTC line: receiver first, then series, then us, then the
    // reported QSO's time/call/serial — cross-checked against BOTH the
    // qtc row and the QSO it snapshots.
    {
        QHash<qint64, const ContestQso*> byId;
        for (const ContestQso& q : qsos) byId.insert(q.id, &q);
        for (int i = 0; i < qtcs.size(); ++i) {
            const ContestDb::QtcRow& r = qtcs[i];
            const ContestQso* q = byId.value(r.qsoId);
            const QString& l = qtcLines[i];
            if (!q) {
                if (err)
                    *err = QString("QTC %1 references a missing QSO id %2")
                               .arg(i + 1).arg(r.qsoId);
                return false;
            }
            int p = 5 + 5 + 1 + 2 + 1 + 10 + 1 + 4 + 1;  // up to receiver
            const QString recv = l.mid(p, kCallW).trimmed();
            p += kCallW + 1;
            const QString series = l.mid(p, 9).trimmed();
            p += 9 + 1;
            const QString sender = l.mid(p, kCallW).trimmed();
            p += kCallW + 1;
            const QString qtime = l.mid(p, 4).trimmed();
            p += 4 + 1;
            const QString qcall = l.mid(p, kCallW).trimmed();
            p += kCallW + 1;
            const QString qser = l.mid(p).trimmed();
            QString bad;
            if (recv != r.toCall.toUpper()) bad = "receiver call";
            else if (!series.startsWith(QString::number(r.block) + "/"))
                bad = "series";
            else if (sender.isEmpty()) bad = "sender call";
            else if (qtime != q->tsUtc.toUTC().toString("HHmm"))
                bad = "reported time";
            else if (qcall != q->v.call.toUpper()) bad = "reported call";
            else if (qser != uncut(q->v.serialR)) bad = "reported serial";
            if (!bad.isEmpty()) {
                if (err)
                    *err = QString("QTC %1 (%2): %3 mismatch\n%4")
                               .arg(i + 1).arg(r.toCall, bad, l);
                return false;
            }
        }
    }
    for (int i = 0; i < qsos.size(); ++i) {
        const ContestQso& q = qsos[i];
        const QString& l = lines[i];
        int p = 5;
        const auto slice = [&](int w) {
            const QString s = l.mid(p, w).trimmed();
            p += w + 1;
            return s;
        };
        const QString freq = slice(5), mo = slice(2), date = slice(10),
                      time = slice(4), my = slice(kCallW),
                      sent = slice(kExchW), his = slice(kCallW),
                      rcvd = l.mid(p).trimmed();   // last field, unpadded
        const QDateTime ts = q.tsUtc.toUTC();
        const QString wantSent =
            exchTokens(def, contest, q, true).join(' ').simplified();
        const QString wantRcvd =
            exchTokens(def, contest, q, false).join(' ').simplified();
        QString bad;
        if (freq != freqField(q.freqHz).trimmed()) bad = "freq";
        else if (mo != modeToken(q.v.mode)) bad = "mode";
        else if (date != ts.toString("yyyy-MM-dd")) bad = "date";
        else if (time != ts.toString("HHmm")) bad = "time";
        else if (his != q.v.call.toUpper()) bad = "his call";
        else if (my.isEmpty()) bad = "my call";
        else if (sent.simplified() != wantSent) bad = "sent exchange";
        else if (rcvd.simplified() != wantRcvd) bad = "received exchange";
        if (!bad.isEmpty()) {
            if (err)
                *err = QString("QSO %1 (%2): %3 mismatch\n%4")
                           .arg(i + 1).arg(q.v.call, bad, l);
            return false;
        }
    }
    if (!text.contains("END-OF-LOG:")) {
        if (err) *err = "missing END-OF-LOG";
        return false;
    }
    return true;
}

} // namespace ttc
