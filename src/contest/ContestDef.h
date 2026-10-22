// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace ttc {

struct CtyInfo;

// How far a duplicate reaches. "Once per band" is the contest norm; CWT
// and friends allow the same station again on the same band in a later
// session, which a new contest instance models — scope stays PerBand.
enum class DupeScope { Never, Contest, PerBand, PerBandMode };

// Which contest.db column a received-exchange entry field fills. The
// schema stays fixed (serial_r, exch1..exch3) and each contest names
// what its columns mean — Sweepstakes needs all three besides the
// serial (precedence, check, section).
enum class ExchCol { RstR, SerialR, Exch1, Exch2, Exch3 };

struct ExchFieldDef {
    ExchCol col;
    QString label;               // above the entry box ("RCV NR", "NAME")
    int     widthCh = 8;         // entry width hint, characters
    QString preset;              // "599" — cleared fields refill with this
    bool    required = true;     // Enter refuses to log while empty
    // Which call-history column prefills this field ("name", "exch1",
    // "sect", "state", "ck", "grid"); empty = no prefill.
    QString historyCol;
    // Sanity-check the typed value against cty.dat: "cqz" or "ituz"
    // flags the field when the number doesn't match the call's expected
    // zone (a station CAN be off-default, so it's a warning, not a
    // block). Empty = no check.
    QString verify;
};

// The values one QSO carries, engine-facing. Sent serial is numeric (the
// engine formats it); everything received is text exactly as copied — a
// contest exchange is evidence, not data to normalize.
struct CQsoValues {
    QString call;
    QString band, mode;          // "40M", "CW"
    QString rstS, rstR;
    int     serialS = 0;
    QString serialR;
    QString exch1, exch2, exch3;
};

// My side, precomputed once per contest open.
struct ContestContext {
    QString myCall;
    QString myCont;              // "NA"
    QString myCountry;           // cty.dat name, e.g. "United States"
    int     myCq = 0;
    int     myItu = 0;           // IARU HF scores by ITU zone distance
};

// One contest's rules. A table entry, not a subclass: every hook is a
// plain function so a definition reads top-to-bottom like the rules
// paragraph it encodes, and adding a contest is one entry in
// contestDefs() plus tests.
struct ContestDef {
    QString id;                  // stable key, stored in contest.db rows
    QString title;               // picker text
    QString cabrilloName;        // CONTEST: header (WWROF registry tag)
    QString modeCategory;        // "CW" | "SSB" | "RTTY" | "MIXED"
    DupeScope dupe = DupeScope::PerBand;
    bool    hasRst = true;       // CWT/MST/NAQP have no RST anywhere
    bool    sentSerial = false;  // engine tracks + formats a running serial
    QString sentExchDefault;     // seed for the instance's sent exchange
    QList<ExchFieldDef> fields;  // received side, in tab order

    // Points for one QSO. cty may have missed (ctyOk false) — score what
    // can be scored and never refuse the log.
    std::function<int(const CQsoValues&, const CtyInfo&, bool ctyOk,
                      const ContestContext&)> points;

    // Multiplier keys this QSO can claim (empty list = contributes none).
    // Keys are already scoped: a per-band mult embeds the band in the key
    // ("DL|40M"), a whole-contest mult doesn't ("PA3"). First QSO to
    // produce an unseen key owns the mult — recomputed from row 1 on any
    // change, never incrementally patched.
    std::function<QStringList(const CQsoValues&, const CtyInfo&, bool ctyOk,
                              const ContestContext&)> mults;

    // Weight of one mult key (WAE: 80m×4, 40m×3, high bands ×2). Keyed on
    // the band embedded in the key; 1 everywhere else.
    std::function<int(const QString& multKey)> multWeight;

    // Cabrillo exchange column layout — ONE ordered token list applied to
    // BOTH sides ("rst", "serial", "exch1", "exch2"). The sent/received
    // sides can therefore never drift apart, which is the CW Open bug
    // (received side emitted name-then-serial) made unrepresentable.
    QStringList cabExch;

    // F-key macro defaults, 1-based key -> "Label|text". Missing keys are
    // blank. S&P falls back to Run when empty.
    QHash<int, QString> fkeyRun, fkeySp;

    // Serial formatting on the air (Cabrillo always gets plain digits).
    bool cutNumbers = true;      // 0->T, 9->N
    int  serialPad = 3;          // 1 -> TT1 at pad 3

    // WAE: QTC traffic exists (sending side; each reported QSO is one
    // point, limits enforced by the QTC engine).
    bool hasQtc = false;
};

// The registry. Order is the picker order.
const QList<const ContestDef*>& contestDefs();
const ContestDef* contestDef(const QString& id);

} // namespace ttc
