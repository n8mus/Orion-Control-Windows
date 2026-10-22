// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include "contest/ContestDb.h"
#include "contest/ContestDef.h"

#include <QDateTime>

namespace ttc {

class CtyLookup;

// Pure contest arithmetic — no database, no widgets, so contesttest can
// hammer it without Qt GUI or fixtures.

// Strip portable decorations so cty.dat answers for the COUNTRY being
// operated from: N8EM/DL and DL/N8EM both resolve as DL; a bare area
// digit (W1AW/4) keeps the call side because cty cannot resolve "4".
QString normalizeForCty(const QString& call);

// A call the log would accept: 3+ chars, at least one letter AND one
// digit. Everything else gets a fill request instead of a log action —
// upstream's silent save_contact drop, made visible.
bool loggableCall(const QString& call);

// "001" -> "TT1" on the air (cut zeros/nines, zero-padded). Cabrillo
// always receives plain digits — this is keying-side formatting only.
QString formatSerial(int n, bool cut, int pad);

// One edit apart (substitution, insertion or deletion) — the busted-
// spot test: the same station skimmed at nearly the same frequency
// under two calls one keystroke apart means one of them is wrong.
bool nearMissCall(const QString& a, const QString& b);

// A macro whose whole text is "{VK1}".."{VK4}" plays that voice-keyer
// slot instead of keying CW (the SSB contests' F-key defaults). Returns
// the 1-based slot, 0 for a normal CW macro.
int vkSlot(const QString& macroText);

// The WPX prefix: letters+digits through the call's last leading digit
// (N8EM -> N8, WA3ABC -> WA3, 4X4AA -> 4X4). Portable designators take
// over: DL/N8EM -> DL0 (no digit gains a 0), W1AW/4 -> W4 (a bare
// digit replaces the home number). Per the CQ WPX rules text.
QString wpxPrefix(const QString& call);

// One row of a computed score.
struct BandCount {
    int qsos = 0, points = 0, mults = 0, weighted = 0;
};

struct ScoreBreakdown {
    int qsos = 0;                       // rows in the log
    int points = 0;                     // sum of per-QSO points
    int qtcPoints = 0;                  // WAE: one per QTC line sent
    int mults = 0;                      // distinct mult keys
    int weightedMults = 0;              // with the def's per-key weight
    qint64 total = 0;                   // (pts+qtc) × wtMults (min 1×)
    QHash<QString, BandCount> perBand;  // "40M" -> counts
    QSet<QString> multKeys;             // for "is this spot a new mult?"
};

// Recompute everything from row 1. A few thousand QSOs is microseconds;
// incremental patching is where mult bugs live (a stuck dupe flag once
// zeroed a whole log's points on the old system).
ScoreBreakdown computeScore(const ContestDef& def,
                            const QList<CQsoValues>& qsos,
                            const CtyLookup* cty,
                            const ContestContext& ctx,
                            int qtcPoints = 0);

// ---- WAE QTC allocation --------------------------------------------------
// The oldest unreported QSOs, never one made with the receiving station,
// capped at min(10, 10 − alreadySentTo, eligible). Cumulative per
// station across the whole contest; each QSO is reportable once, ever.
QList<qint64> allocateQtc(const QList<ContestQso>& qsos,
                          const QSet<qint64>& reportedIds,
                          const QString& toCall, int alreadySentTo);

// On-air seconds under the WAE rest rule: a gap of >= 60 minutes with
// no QSO and no QTC is a break; everything else counts as operating.
// events = QSO + QTC-confirm timestamps, any order.
int opTimeSecs(QList<QDateTime> events);

// Would this call be a dupe against the worked list, under the def's
// scope? band/mode are the CURRENT rig state.
bool isDupe(const ContestDef& def, const QList<CQsoValues>& qsos,
            const QString& call, const QString& band, const QString& mode);

// Expand {MYCALL} {HISCALL} {SNT} {SENTNR} {EXCH} {NAME} in a macro.
// Unknown tokens key literally — an operator typo should be audible on
// the sidetone, not silently swallowed.
QString expandMacro(QString text, const ContestDef& def,
                    const ContestContext& ctx, const QString& hisCall,
                    const QString& sentExch, int sentSerial);

// ---- ESM: what should Enter do right now? --------------------------------
//
// The operator's corrected machine, ported from the 27 WAE tests of the
// old system. The rules that must survive any refactor:
//  - S&P is a STRICT three-beat: my call → my exchange (once) → log.
//    The log is NEVER bundled with the exchange; the silent field wipe
//    after beat three is the only "it logged" signal.
//  - Enter logs from ANY field once the call and exchange are complete.
//  - A call the log would refuse never offers the log action — Enter
//    asks for a fill (his call + exchange) instead.
//  - Both sent-flags reset the moment the call text changes.

enum class EsmAct {
    KeyCq,        // F1
    KeyHisCall,   // F2
    KeyExch,      // F3   (sets exchSent)
    KeyMyCall,    // F5   (sets myCallSent)
    KeyTu,        // F4   (run-mode closer; QRZ slot is disabled by spec)
    Log,          // commit the QSO, silently
    FocusExch,    // move the cursor to the first exchange field
};

struct EsmInput {
    bool esmOn = true;
    bool run = true;           // false = S&P
    bool callEmpty = true;
    bool callLoggable = false; // loggableCall() on the entry text
    bool exchComplete = false; // every required received field filled
    bool myCallSent = false;   // S&P beat one done (this QSO)
    bool exchSent = false;     // exchange keyed (this QSO)
};

QList<EsmAct> esmPlan(const EsmInput& in);

} // namespace ttc
