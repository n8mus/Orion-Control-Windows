// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <algorithm>
#include <QRegularExpression>
#include <QString>
#include <QVector>

namespace ttc {

// Callsigns in a speech-to-text transcript. Whisper writes a spoken call
// three ways: as one word ("G8SEZ"), letter by letter ("G-8-S-E-Z",
// "K 1 A B C") or in phonetics ("Kilo One Alpha Bravo Charlie"). Each
// word maps to its letter/digit where it can; runs of such single
// characters join; whatever joins (or already was one word) counts if it
// has a callsign's shape: prefix, a digit, a 1-4 letter suffix, optional
// /P-style tail. Number runs ("five nine", "1, 2, 3") never pass — no
// letters. Offsets are into the original text, for highlighting.
struct CallHit {
    QString call;
    int start = 0, len = 0;
};

inline bool looksLikeCall(const QString& c) {
    static const QRegularExpression re(
        "^(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])[0-9][A-Z]{1,4}(?:/[A-Z0-9]{1,3})?$");
    return re.match(c).hasMatch();
}

// One word -> its single character (phonetic, digit word, lone
// letter/digit), or a null QChar.
inline QChar spokenChar(const QString& w) {
    static const QHash<QString, QChar> map = [] {
        QHash<QString, QChar> m;
        const char* nato[] = {"ALPHA", "BRAVO", "CHARLIE", "DELTA", "ECHO", "FOXTROT",
                              "GOLF", "HOTEL", "INDIA", "JULIET", "KILO", "LIMA",
                              "MIKE", "NOVEMBER", "OSCAR", "PAPA", "QUEBEC", "ROMEO",
                              "SIERRA", "TANGO", "UNIFORM", "VICTOR", "WHISKEY",
                              "XRAY", "YANKEE", "ZULU"};
        for (int i = 0; i < 26; ++i) m.insert(nato[i], QChar('A' + i));
        m.insert("ALFA", 'A'); m.insert("JULIETT", 'J'); m.insert("WHISKY", 'W');
        m.insert("X-RAY", 'X');
        const char* dig[] = {"ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX",
                             "SEVEN", "EIGHT", "NINE"};
        for (int i = 0; i < 10; ++i) m.insert(dig[i], QChar('0' + i));
        m.insert("NINER", '9');
        return m;
    }();
    const QString u = w.toUpper();
    if (u.size() == 1 && u[0].isLetterOrNumber()) return u[0];
    return map.value(u, QChar());
}

inline QVector<CallHit> extractCalls(const QString& text) {
    QVector<CallHit> out;
    // Words: letters/digits/slash plus inner hyphens (X-RAY, G-8-S-E-Z).
    static const QRegularExpression word("[A-Za-z0-9/]+(?:-[A-Za-z0-9/]+)*");
    struct Tok { QString s; int start, end; };
    QVector<Tok> toks;
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        toks.push_back({m.captured(), int(m.capturedStart()), int(m.capturedEnd())});
    }
    const auto add = [&](const QString& c, int s, int e) {
        if (!looksLikeCall(c)) return;
        for (const CallHit& h : out)
            if (h.start == s) return;
        out.push_back({c, s, e - s});
    };
    // A run of single characters, possibly spread over several words —
    // each with the text span it came from. Calls are found inside it
    // greedily, longest first: "Kilo One Alpha Bravo Charlie five nine"
    // is K1ABC + 59 (no call), not K1ABC59.
    struct Ch { QChar c; int start, end; };
    QVector<Ch> run;
    const auto flush = [&] {
        int i = 0;
        while (i + 3 <= run.size()) {
            int best = 0;
            for (int len = std::min(10, int(run.size()) - i); len >= 3; --len) {
                QString c;
                for (int k = 0; k < len; ++k) c += run[i + k].c;
                if (looksLikeCall(c)) { best = len; break; }
            }
            if (best == 0) { ++i; continue; }
            QString c;
            for (int k = 0; k < best; ++k) c += run[i + k].c;
            add(c, run[i].start, run[i + best - 1].end);
            i += best;
        }
        run.clear();
    };
    for (const Tok& t : toks) {
        const QString u = t.s.toUpper();
        if (u.contains('-') && u != "X-RAY") {
            // "G-8-S-E-Z": every part one character -> part of the run.
            QVector<Ch> parts;
            bool all = true;
            for (const QString& p : u.split('-')) {
                const QChar c = spokenChar(p);
                if (c.isNull()) { all = false; break; }
                parts.push_back({c, t.start, t.end});
            }
            if (all) { run += parts; continue; }
        }
        const QChar c = spokenChar(u);
        if (!c.isNull()) { run.push_back({c, t.start, t.end}); continue; }
        flush();
        add(u, t.start, t.end);                   // one-word call ("W1AW")
    }
    flush();
    return out;
}

} // namespace ttc
