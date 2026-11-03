// SPDX-License-Identifier: GPL-2.0-or-later
#include "cw/CwDeck.h"

#include <QAbstractButton>
#include <QDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>

#include "cw/CwMacros.h"
#include "cw/CwWindow.h"
#include "log/QrzLookup.h"
#include "log/QsoContext.h"
#include "net/RotorLink.h"
#include "ui/DeckStyle.h"
#include "util/Bearing.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

namespace {
const char* kCap = "color:#8798a8; font-size:10px;";
// The band map's worked-before ramp, as the LOG window paints it.
const char* statusColor(QChar st) {
    switch (st.toLatin1()) {
        case 'N': return "#ff5c46";
        case 'B': return "#ffbe3c";
        case 'W': return "#82de8c";
        case 'C': return "#96a2b2";
        default:  return "#ffffff";
    }
}
const QRegularExpression& callRe() {
    static const QRegularExpression re(
        "^[A-Z0-9]{1,3}[0-9][A-Z0-9]*[A-Z](/[A-Z0-9]+)?$|"
        "^[A-Z0-9]+/[A-Z0-9]{1,3}[0-9][A-Z0-9]*[A-Z]$");
    return re;
}
// A deck check box: a push button that wears the sidebar's lit look, bound
// both ways to the CW window's own QCheckBox so the two never disagree.
QPushButton* bindToggle(const char* text, QAbstractButton* src,
                        const QString& tip, QWidget* parent) {
    auto* b = new QPushButton(QLatin1String(text), parent);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    b->setFixedHeight(20);
    if (src) {
        b->setChecked(src->isChecked());
        b->setEnabled(src->isEnabled());
        QObject::connect(src, &QAbstractButton::toggled, b,
                         &QAbstractButton::setChecked);
        QObject::connect(b, &QAbstractButton::toggled, src,
                         &QAbstractButton::setChecked);
    } else {
        b->setEnabled(false);
    }
    return b;
}
} // namespace

CwDeck::CwDeck(CwWindow* cw, LogbookIndex* idx, const CtyLookup* cty,
               RotorLink* rotor, QrzLookup* qrz, QWidget* parent)
    : QWidget(parent), cw_(cw), idx_(idx), cty_(cty), rotor_(rotor),
      qrz_(qrz) {
    setObjectName("cwDeck");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(kDeckStyle);
    buildUi();
    if (qrz_) {
        connect(qrz_, &QrzLookup::result, this,
                [this](const QString& call, bool ok, const QString& name,
                       const QString& qth, const QString& grid,
                       const QString&) {
                    if (!ok) return;
                    const QString c = call.trimmed().toUpper();
                    if (!grid.trimmed().isEmpty())
                        qrzGrid_.insert(c, grid.trimmed().toUpper());
                    if (c != call_->text().trimmed().toUpper()) return;
                    // The callbook's name/QTH/grid into the shared record
                    // (the NAME box is HIS word and is never touched), so
                    // [his qth] keys and the heading sharpens.
                    if (ctx_) {
                        if (ctx_->name().isEmpty()) ctx_->setName(name);
                        if (ctx_->qth().isEmpty()) ctx_->setQth(qth);
                        if (ctx_->grid().isEmpty()) ctx_->setGrid(grid);
                    }
                    refreshInfo();
                });
    }
    if (rotor_)
        connect(rotor_, &RotorLink::azimuthChanged, this,
                [this](double) { refreshInfo(); });
    if (idx_)
        connect(idx_, &LogbookIndex::updated, this, [this] { refreshInfo(); });
}

void CwDeck::buildUi() {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);

    // ---- CW READ (left third) --------------------------------------------
    {
        readPane_ = new QWidget(this);
        auto* box = new QVBoxLayout(readPane_);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        auto* trow = new QHBoxLayout;
        trow->addStretch(1);
        auto* clr = new QPushButton("clear", this);
        clr->setFlat(true);
        clr->setFocusPolicy(Qt::NoFocus);
        clr->setStyleSheet("color:#8798a8; font-size:10px; border:0;");
        clr->setCursor(Qt::PointingHandCursor);
        trow->addWidget(clr);
        box->addLayout(trow);
        read_ = new QPlainTextEdit(this);
        read_->setReadOnly(true);
        read_->setMaximumBlockCount(200);
        read_->setObjectName("cwRead");
        read_->setStyleSheet("QPlainTextEdit#cwRead { background:#2b5c8a;"
                             " color:#ffffff; border:1px solid #3d7ab6;"
                             " selection-background-color:#1c2430; }");
        read_->setMinimumWidth(120);
        read_->setToolTip("Decoded CW. Double-click a call to work it ·"
                          " right-click: text size");
        sizeMenu(read_, "read");
        connect(clr, &QPushButton::clicked, read_, &QPlainTextEdit::clear);
        read_->viewport()->installEventFilter(this);
        box->addWidget(read_, 1);
        lay->addWidget(readPane_, 3);
        addPopout("read", "CW READ", readPane_, trow,
                  [this](QWidget* w) {
                      static_cast<QHBoxLayout*>(layout())->insertWidget(0, w, 3);
                  });
    }

    // ---- middle third: the QSO ---------------------------------------------
    {
        auto* mid = new QVBoxLayout;
        mid->setSpacing(3);

        // Row 1: CALL · NAME · S · R · 0-BEAT · LOG IT. Big type: the
        // call must read from across the desk while hand-sending.
        auto* r1 = new QHBoxLayout;
        r1->setSpacing(4);
        QFont big;
        big.setPointSize(16);
        big.setBold(true);
        const auto cap = [this](const char* t) {
            auto* l = new QLabel(QLatin1String(t), this);
            l->setStyleSheet(kCap);
            l->setObjectName("cap");          // keeps its text width
            return l;
        };
        r1->addWidget(cap("CALL"));
        call_ = new QLineEdit(this);
        call_->setFont(big);
        call_->setMaxLength(16);
        call_->setMinimumWidth(118);
        call_->setMaximumWidth(150);
        call_->setToolTip("The station you're working — what [his call] keys.\n"
                          "Enter moves to RST R · ←/→ (empty) walk the spots ·"
                          " Space (empty) nothing.\nA call from the band map"
                          " lands here on click or arrow.");
        call_->installEventFilter(this);
        r1->addWidget(call_);
        r1->addWidget(cap("NAME"));
        name_ = new QLineEdit(this);
        name_->setFont(big);
        name_->setMaxLength(14);
        name_->setMinimumWidth(56);
        name_->setToolTip("The name he GIVES you — what [his name] keys.\n"
                          "Enter logs.");
        name_->installEventFilter(this);
        r1->addWidget(name_, 1);
        const auto rst = [&](const char* t, QLineEdit** e, const char* tip) {
            r1->addWidget(cap(t));
            *e = new QLineEdit(this);
            (*e)->setFont(big);
            (*e)->setMaxLength(5);
            (*e)->setFixedWidth(52);
            (*e)->setText("599");
            (*e)->setToolTip(QLatin1String(tip));
            (*e)->installEventFilter(this);
            r1->addWidget(*e);
        };
        rst("S", &rstS_, "RST you SENT — what [rst sent] keys. Enter logs.");
        rst("R", &rstR_, "RST he GAVE you. Enter logs.");
        auto* zb = new QPushButton("0-BEAT", this);
        zb->setFocusPolicy(Qt::NoFocus);
        zb->setToolTip("Zero-beat the strongest signal in the passband (Z)");
        connect(zb, &QPushButton::clicked, this,
                [this] { emit zeroBeatRequested(); });
        r1->addWidget(zb);
        auto* logBtn = new QPushButton("LOG IT", this);
        logBtn->setFocusPolicy(Qt::NoFocus);
        logBtn->setStyleSheet("QPushButton { background:#2b5c8a; color:#ffffff;"
                              " border:1px solid #5db2f0; font-weight:bold;"
                              " padding:2px 8px; }");
        logBtn->setToolTip("Log the QSO to the station log: call, both reports,"
                           " name,\ndial, mode and UTC now. Name/QTH/grid fill"
                           " from the callbook after.\nEnter in S, R or NAME"
                           " does the same.");
        connect(logBtn, &QPushButton::clicked, this,
                [this] { emit logRequested(); });
        r1->addWidget(logBtn);
        for (QLineEdit* e : {call_, name_, rstS_, rstR_})
            e->setFixedHeight(30);
        mid->addLayout(r1);

        // Row 2: country · worked-before, then Spot / SP / LP.
        auto* r2 = new QHBoxLayout;
        r2->setSpacing(4);
        info_ = new QLabel(this);
        info_->setStyleSheet("color:#c8d4e0; font-size:11px;");
        info_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        r2->addWidget(info_, 1);
        auto* spot = new QPushButton("Spot", this);
        spot->setFocusPolicy(Qt::NoFocus);
        spot->setToolTip("Send this call to the cluster as a DX spot at the dial");
        connect(spot, &QPushButton::clicked, this, [this] {
            const QString c = call_->text().trimmed().toUpper();
            if (!c.isEmpty()) emit spotDxRequested(c, rigHz_);
        });
        r2->addWidget(spot);
        spBtn_ = new QPushButton("SP", this);
        lpBtn_ = new QPushButton("LP", this);
        spBtn_->setToolTip("Turn the rotor to the short path");
        lpBtn_->setToolTip("Turn the rotor to the long path");
        for (QPushButton* b : {spBtn_, lpBtn_}) {
            b->setFocusPolicy(Qt::NoFocus);
            b->setFixedWidth(34);
            r2->addWidget(b);
        }
        connect(spBtn_, &QPushButton::clicked, this, [this] {
            if (rotor_ && spAz_ >= 0) {
                rotor_->turnTo(spAz_);
                emit rotorAimed(spAz_, distKm_, call_->text().trimmed().toUpper());
            }
        });
        connect(lpBtn_, &QPushButton::clicked, this, [this] {
            if (rotor_ && lpAz_ >= 0) {
                rotor_->turnTo(lpAz_);
                emit rotorAimed(lpAz_, -1.0,
                                call_->text().trimmed().toUpper() + " LP");
            }
        });
        mid->addLayout(r2);

        // Rows 3-4: F1-F12, two rows of six, the CW window's own macros.
        auto* fg = new QGridLayout;
        fg->setSpacing(3);
        for (int i = 0; i < 12; ++i) {
            fk_[i] = new QPushButton(this);
            fk_[i]->setFocusPolicy(Qt::NoFocus);
            fk_[i]->setFixedHeight(22);
            fk_[i]->setMinimumWidth(40);
            fk_[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            {
                QFont f = fk_[i]->font();
                f.setPointSize(std::max(7, f.pointSize() - 1));
                fk_[i]->setFont(f);
            }
            connect(fk_[i], &QPushButton::clicked, this,
                    [this, i] { keyFkey(i); });
            fk_[i]->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(fk_[i], &QPushButton::customContextMenuRequested, this,
                    [this, i](const QPoint&) {
                        if (i == 11) return;
                        cw_->editFkey(i);
                        applyFkeyLabels();
                    });
            fg->addWidget(fk_[i], i / 6, i % 6);
            fg->setColumnStretch(i % 6, 1);
        }
        mid->addLayout(fg);
        applyFkeyLabels();

        // Row 5: WPM · TUNE · STOP · · · FLD SOM DEEP RADIO NR
        auto* r5 = new QHBoxLayout;
        r5->setSpacing(4);
        r5->addWidget(cap("WPM"));
        wpm_ = new QSpinBox(this);
        wpm_->setRange(5, 99);
        wpm_->setValue(cw_->speedWpm());
        wpm_->setFixedWidth(52);
        wpm_->setFocusPolicy(Qt::ClickFocus);
        wpm_->setToolTip("Keying speed (↑/↓ anywhere in the deck; the "
                         "WinKeyer pot moves it too)");
        connect(wpm_, &QSpinBox::valueChanged, this,
                [this](int v) { cw_->setSpeedWpm(v); });
        connect(cw_, &CwWindow::speedChanged, this, [this](int v) {
            const QSignalBlocker b(wpm_);
            wpm_->setValue(v);
        });
        r5->addWidget(wpm_);
        tuneBtn_ = new QPushButton("TUNE", this);
        tuneBtn_->setCheckable(true);
        tuneBtn_->setFocusPolicy(Qt::NoFocus);
        tuneBtn_->setToolTip("Steady key-down for tuning (click again to stop)");
        connect(tuneBtn_, &QPushButton::toggled, this, [this](bool on) {
            cw_->setTune(on);
            setStatus(on ? "TUNE — key down" : QString());
        });
        r5->addWidget(tuneBtn_);
        auto* stop = new QPushButton("STOP", this);
        stop->setFocusPolicy(Qt::NoFocus);
        stop->setToolTip("Dump the keying buffer now (Esc)");
        connect(stop, &QPushButton::clicked, this, [this] { stopEverything(); });
        r5->addWidget(stop);
        r5->addStretch(1);
        r5->addWidget(bindToggle("FLD", cw_->toggle(CwWindow::Toggle::Engine),
                                 "fldigi decode engine (off = the original "
                                 "console decoder)", this));
        r5->addWidget(bindToggle("SOM", cw_->toggle(CwWindow::Toggle::Som),
                                 "Fuzzy whole-character matching", this));
        r5->addWidget(bindToggle("DEEP", cw_->toggle(CwWindow::Toggle::Deep),
                                 "Weak-signal narrow filter (slower)", this));
        r5->addWidget(bindToggle("RADIO", cw_->toggle(CwWindow::Toggle::RadioSrc),
                                 "Decode the RADIO's audio (SignaLink) "
                                 "instead of the SDR", this));
        r5->addWidget(bindToggle("NR", cw_->toggle(CwWindow::Toggle::Nr),
                                 "RNNoise ahead of the decoder (RADIO source)",
                                 this));
        mid->addLayout(r5);

        // Rows 6-7: the radio's own CW settings over CAT, mirrors of the
        // CW window's sliders — a drag here moves that slider (which
        // owns the throttled CAT send); that slider moving moves this.
        const auto rigRow = [this](int row) {
            rigRow_[row] = new QWidget(this);
            auto* h = new QHBoxLayout(rigRow_[row]);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(4);
            return h;
        };
        const auto mirror = [this](QHBoxLayout* h, const char* capText,
                                   CwWindow::RigCtl which, QSlider** out,
                                   QLabel** val) {
            auto* c = new QLabel(QLatin1String(capText), this);
            c->setStyleSheet(kCap);
            c->setFixedWidth(34);
            h->addWidget(c);
            auto* s = new QSlider(Qt::Horizontal, this);
            s->setFocusPolicy(Qt::NoFocus);
            QSlider* src = cw_->rigSlider(which);
            if (src) {
                s->setRange(src->minimum(), src->maximum());
                s->setSingleStep(src->singleStep());
                s->setPageStep(src->pageStep());
                s->setValue(src->value());
                s->setToolTip(src->toolTip());
                c->setToolTip(src->toolTip());
                connect(s, &QSlider::valueChanged, src, &QSlider::setValue);
                connect(src, &QSlider::valueChanged, this, [s](int v) {
                    const QSignalBlocker b(s);
                    s->setValue(v);
                });
            }
            h->addWidget(s, 1);
            auto* v = new QLabel("--", this);
            v->setFixedWidth(34);
            v->setStyleSheet(kCap);
            h->addWidget(v);
            connect(s, &QSlider::valueChanged, v,
                    [v](int nv) { v->setText(QString::number(nv)); });
            *out = s;
            *val = v;
        };
        auto* r6 = rigRow(0);
        mirror(r6, "VOL", CwWindow::RigCtl::Vol, &rigVol_, &rigVolVal_);
        mirror(r6, "PITCH", CwWindow::RigCtl::Pitch, &rigPitch_, &rigPitchVal_);
        mid->addWidget(rigRow_[0]);
        auto* r7 = rigRow(1);
        mirror(r7, "QSK", CwWindow::RigCtl::Qsk, &rigQsk_, &rigQskVal_);
        mirror(r7, "RISE", CwWindow::RigCtl::Rise, &rigRise_, &rigRiseVal_);
        mid->addWidget(rigRow_[1]);
        setRigCwAvailable(cw_->rigCwAvailable());
        connect(cw_, &CwWindow::rigCwAvailableChanged, this,
                &CwDeck::setRigCwAvailable);

        // Row 8: the reader's wpm · Hz, the WinKeyer box, the rig keyer.
        auto* r8 = new QHBoxLayout;
        r8->setSpacing(6);
        rxInfo_ = new QLabel(this);
        rxInfo_->setTextFormat(Qt::RichText);
        rxInfo_->setStyleSheet("color:#6aa5d8; font-size:11px;");
        // FIXED at the widest text it ever shows: the Hz readout changes
        // length as the tone wanders, and a label that re-negotiates its
        // width bounces the whole row (operator, 2026-11-02).
        rxInfo_->setFixedWidth(
            rxInfo_->fontMetrics().horizontalAdvance("88 WPM · 8888 Hz") + 12);
        rxInfo_->setObjectName("cap");       // the shrink loop leaves it
        r8->addWidget(rxInfo_);
        status_ = new QLabel(this);
        status_->setStyleSheet(kCap);
        status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        r8->addWidget(status_, 1);
        auto* wk = new QPushButton("WinKeyer…", this);
        wk->setFocusPolicy(Qt::NoFocus);
        wk->setToolTip("WinKeyer control box: weighting, key compensation,\n"
                       "ratio, PTT lead/tail, Farnsworth, presets");
        connect(wk, &QPushButton::clicked, this,
                [this] { emit winKeyerPanelRequested(); });
        r8->addWidget(wk);
        rigKeyer_ = new QLabel(cw_->rigKeyerText(), this);
        rigKeyer_->setStyleSheet(kCap);
        rigKeyer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rigKeyer_->setToolTip("The RADIO's built-in keyer, read-only");
        connect(cw_, &CwWindow::rigKeyerTextChanged, rigKeyer_, &QLabel::setText);
        r8->addWidget(rigKeyer_, 1);
        mid->addLayout(r8);
        mid->addStretch(1);
        lay->addLayout(mid, 4);        // the entry row needs the width
    }

    // ---- CW TYPE (right third) -------------------------------------------
    {
        typePane_ = new QWidget(this);
        auto* box = new QVBoxLayout(typePane_);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        auto* thdr = new QHBoxLayout;
        auto* t = new QLabel("CW TYPE — Enter sends", this);
        t->setStyleSheet(kCap);
        thdr->addWidget(t);
        thdr->addStretch(1);
        box->addLayout(thdr);
        type_ = new QPlainTextEdit(this);
        type_->setObjectName("cwType");
        type_->setStyleSheet("QPlainTextEdit#cwType { background:#2b5c8a;"
                             " color:#ffffff; border:1px solid #3d7ab6;"
                             " selection-background-color:#1c2430; }");
        type_->setMaximumBlockCount(100);
        type_->setMinimumWidth(120);
        type_->setToolTip("Type CW; Enter keys the line and keeps it as "
                          "history above.\nLIVE: each keystroke goes out as "
                          "typed (Backspace unsends).\nWORD: each word goes "
                          "out at the space bar.\nEsc stops keying · "
                          "right-click: text size");
        sizeMenu(type_, "type");
        type_->installEventFilter(this);
        connect(type_, &QPlainTextEdit::textChanged, this,
                [this] { onTypeChanged(); });
        box->addWidget(type_, 1);
        auto* brow = new QHBoxLayout;
        brow->setSpacing(4);
        brow->addWidget(bindToggle("LIVE", cw_->toggle(CwWindow::Toggle::Live),
                                   "Stream each keystroke to the keyer as "
                                   "typed", this));
        brow->addWidget(bindToggle("WORD", cw_->toggle(CwWindow::Toggle::Word),
                                   "Each word goes out when the space bar "
                                   "lands", this));
        sent_ = new QLabel(this);
        sent_->setStyleSheet("color:#a4cf82; font-size:10px;");
        sent_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        brow->addWidget(sent_, 1);
        box->addLayout(brow);
        lay->addWidget(typePane_, 3);
        addPopout("type", "CW TYPE", typePane_, thdr,
                  [this](QWidget* w) {
                      static_cast<QHBoxLayout*>(layout())->addWidget(w, 3);
                  });
    }

    // The keys, while the deck is visible in the active window, wherever
    // focus sits (F1 must key from the panadapter too).
    const auto sc = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, fn);
        shortcuts_ << s;
    };
    for (int i = 0; i < 12; ++i)
        sc(QKeySequence(Qt::Key_F1 + i), [this, i] { keyFkey(i); });
    sc(QKeySequence(Qt::Key_PageUp), [this] { nudgeSpeed(+1); });
    sc(QKeySequence(Qt::Key_PageDown), [this] { nudgeSpeed(-1); });
    for (QShortcut* s : shortcuts_) s->setEnabled(false);

    // The call box: uppercase, no spaces, the shared record, the arrows.
    connect(call_, &QLineEdit::textEdited, this, [this](const QString& t) {
        QString up = t.toUpper();
        up.remove(QLatin1Char(' '));
        if (up != t) {
            const int pos = call_->cursorPosition();
            call_->setText(up);
            call_->setCursorPosition(std::min(pos, int(up.size())));
        }
        callFromSpot_ = false;
        onCallEdited();
    });
    connect(call_, &QLineEdit::returnPressed, this, [this] {
        requestQrz(call_->text());
        rstR_->setFocus();
        rstR_->selectAll();
    });
    connect(call_, &QLineEdit::editingFinished, this,
            [this] { requestQrz(call_->text()); });
    for (QLineEdit* e : {name_, rstS_, rstR_})
        connect(e, &QLineEdit::returnPressed, this,
                [this] { emit logRequested(); });

    // Prose labels never widen the window (the deck sits under the pan;
    // the width budget is the main window's floor).
    for (QLabel* l : findChildren<QLabel*>()) {
        if (l->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) continue;
        if (l->objectName() == QLatin1String("cap")) continue;
        auto sp = l->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Maximum);
        l->setSizePolicy(sp);
        l->setMinimumWidth(0);
    }
    refreshInfo();
    setRxWpm(0);
}

// ---- context ---------------------------------------------------------------

void CwDeck::setContext(QsoContext* ctx) {
    ctx_ = ctx;
    if (!ctx_) return;
    struct Link { QLineEdit* e; void (QsoContext::*set)(const QString&); };
    const Link links[] = {
        {call_, &QsoContext::setCall},
        {name_, &QsoContext::setHandle},
        {rstS_, &QsoContext::setRstSent},
        {rstR_, &QsoContext::setRstRcvd},
    };
    for (const Link& l : links)
        connect(l.e, &QLineEdit::textChanged, this,
                [this, l](const QString& t) {
                    if (ctxEcho_) return;
                    ctxEcho_ = true;
                    (ctx_->*l.set)(t);
                    ctxEcho_ = false;
                });
    connect(ctx_, &QsoContext::changed, this, [this] {
        if (ctxEcho_) return;
        ctxEcho_ = true;
        const auto put = [](QLineEdit* e, const QString& v) {
            if (e->text() != v) e->setText(v);
        };
        put(call_, ctx_->call());
        put(name_, ctx_->handle());
        put(rstS_, ctx_->rstSent());
        put(rstR_, ctx_->rstRcvd());
        ctxEcho_ = false;
        onCallEdited();
    });
    // What is on screen is the record.
    ctxEcho_ = true;
    if (!ctx_->call().isEmpty()) call_->setText(ctx_->call());
    if (!ctx_->handle().isEmpty()) name_->setText(ctx_->handle());
    if (!ctx_->rstSent().isEmpty()) rstS_->setText(ctx_->rstSent());
    if (!ctx_->rstRcvd().isEmpty()) rstR_->setText(ctx_->rstRcvd());
    ctxEcho_ = false;
    ctx_->setCall(call_->text());
    ctx_->setHandle(name_->text());
    ctx_->setRstSent(rstS_->text());
    ctx_->setRstRcvd(rstR_->text());
    onCallEdited();
}

void CwDeck::setRig(qint64 hz, const QString& adifMode) {
    const bool bandMoved = LogbookIndex::bandForHz(hz)
                        != LogbookIndex::bandForHz(rigHz_);
    rigHz_ = hz;
    if (!adifMode.isEmpty()) rigMode_ = adifMode;
    if (bandMoved) refreshInfo();
}

void CwDeck::nudgeSpeed(int delta) {
    wpm_->setValue(wpm_->value() + delta);
    setStatus(QString("%1 wpm").arg(wpm_->value()));
}

void CwDeck::prefillCall(const QString& call, qint64 hz) {
    Q_UNUSED(hz);
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return;
    const bool fresh = c != call_->text().trimmed().toUpper();
    call_->setText(c);                     // textChanged -> the record
    if (fresh) {
        name_->clear();
        if (ctx_) { ctx_->setName(QString()); ctx_->setQth(QString());
                    ctx_->setGrid(QString()); }
    }
    callFromSpot_ = true;
    onCallEdited();
    if (fresh) requestQrz(c);
    if (isVisible()) { call_->setFocus(); call_->deselect(); }
}

void CwDeck::setRigCwAvailable(bool on) {
    for (QWidget* w : rigRow_) if (w) w->setEnabled(on);
}

void CwDeck::clearForNext() {
    callFromSpot_ = false;
    call_->clear();
    name_->clear();
    rstS_->setText("599");
    rstR_->setText("599");
    if (ctx_) { ctx_->setRstSent("599"); ctx_->setRstRcvd("599"); }
    onCallEdited();
    if (isVisible()) call_->setFocus();
}

// ---- reader --------------------------------------------------------------

void CwDeck::appendRead(const QString& text) {
    read_->moveCursor(QTextCursor::End);
    read_->insertPlainText(text);
    read_->moveCursor(QTextCursor::End);
}

void CwDeck::setRxWpm(int wpm) {
    rxWpm_ = wpm;
    QString hz = QStringLiteral("— Hz");
    if (rxPitch_ > 0) {
        const int target = QSettings().value("cw/pitchHz", 550).toInt();
        const bool on = std::abs(rxPitch_ - target) <= 10.0;
        hz = QString("<span style='color:%1'>%2 Hz</span>")
                 .arg(on ? "#8fd48f" : "#e0b060").arg(qRound(rxPitch_));
    }
    rxInfo_->setText((rxWpm_ > 0 ? QString("%1 WPM").arg(rxWpm_)
                                 : QStringLiteral("— WPM"))
                     + "&nbsp;·&nbsp;" + hz);
}

void CwDeck::setRxPitch(double hz) {
    static double shown = -1.0;
    if (hz < 0) shown = -1.0;
    else if (shown < 0 || std::abs(hz - shown) > 1.5) shown = hz;
    rxPitch_ = shown;
    setRxWpm(rxWpm_);
}

static void showSl(QSlider* s, QLabel* v, int val) {
    if (!s) return;
    const QSignalBlocker b(s);
    s->setValue(val);
    if (v) v->setText(QString::number(val));
}
void CwDeck::showRigSidetoneVol(int pct)  { showSl(rigVol_, rigVolVal_, pct); }
void CwDeck::showRigSidetonePitch(int hz) { showSl(rigPitch_, rigPitchVal_, hz); }
void CwDeck::showRigQskDelay(int val)     { showSl(rigQsk_, rigQskVal_, val); }
void CwDeck::showRigAttackDecay(int ms)   { showSl(rigRise_, rigRiseVal_, ms); }

// ---- sending ---------------------------------------------------------------

void CwDeck::keyFkey(int idx0) {
    if (idx0 == 11) { wipe(); return; }
    const QString spec = QSettings()
        .value(QString("cw/fkey/F%1").arg(idx0 + 1)).toString();
    const QString text = spec.section('|', 1);
    if (text.trimmed().isEmpty()) return;
    const QString missing = cw_->sendTyped(text);
    if (!missing.isEmpty()) {
        setStatus(QString("nothing in %1 — fill it first").arg(missing));
        if (missing == QLatin1String("his call")) call_->setFocus();
        else if (missing == QLatin1String("his name")) name_->setFocus();
        return;
    }
    sent_->setText("→ " + spec.section('|', 0, 0).trimmed());
    setStatus(QString("F%1").arg(idx0 + 1));
}

void CwDeck::applyFkeyLabels() {
    QSettings s;
    for (int i = 0; i < 12; ++i) {
        if (i == 11) {
            fk_[i]->setText("F12 WIPE");
            fk_[i]->setToolTip("Clear the entry for the next contact");
            continue;
        }
        const QString spec = s.value(QString("cw/fkey/F%1").arg(i + 1)).toString();
        const QString label = spec.section('|', 0, 0).trimmed();
        fk_[i]->setText(QString("F%1 %2").arg(i + 1)
                            .arg(label.isEmpty() ? QStringLiteral("—") : label));
        fk_[i]->setToolTip(QString("F%1  %2\nsends:  %3\n\nclick or press "
                                   "F%1 · right-click to edit")
                               .arg(i + 1).arg(label, spec.section('|', 1)));
        fk_[i]->setEnabled(!spec.section('|', 1).trimmed().isEmpty());
    }
}

void CwDeck::wipe() {
    clearForNext();
    setStatus("wiped");
}

void CwDeck::stopEverything() {
    cw_->stopKeying();
    tuneBtn_->setChecked(false);
    // Drop the line being typed (it may have gone out half way).
    QTextCursor c = type_->textCursor();
    c.movePosition(QTextCursor::End);
    c.select(QTextCursor::LineUnderCursor);
    c.removeSelectedText();
    typeLine_.clear();
    typeSent_ = 0;
    setStatus("stopped");
}

QString CwDeck::currentTypeLine() const {
    return type_->document()->lastBlock().text();
}

void CwDeck::sendTypedLine() {
    const QString line = currentTypeLine();
    const bool live = cw_->toggle(CwWindow::Toggle::Live)->isChecked();
    const bool word = cw_->toggle(CwWindow::Toggle::Word)->isChecked();
    QString rest = word ? line.mid(typeSent_) : line;
    if (!live && !rest.trimmed().isEmpty()) {
        const QString missing = cw_->sendTyped(rest);
        if (!missing.isEmpty()) {
            setStatus(QString("nothing in %1 — fill it first").arg(missing));
            return;
        }
        sent_->setText(rest.right(40));
    }
    // The line stays as history; a fresh line opens below.
    QTextCursor c = type_->textCursor();
    c.movePosition(QTextCursor::End);
    type_->setTextCursor(c);
    if (!line.isEmpty()) type_->insertPlainText("\n");
    typeLine_.clear();
    typeSent_ = 0;
}

void CwDeck::onTypeChanged() {
    const QString now = currentTypeLine();
    const bool live = cw_->toggle(CwWindow::Toggle::Live)->isChecked();
    const bool word = cw_->toggle(CwWindow::Toggle::Word)->isChecked();
    if (live) {
        if (now.startsWith(typeLine_))
            cw_->streamChars(now.mid(typeLine_.size()));
        else if (typeLine_.startsWith(now))
            cw_->unsendChars(typeLine_.size() - now.size());
        if (now.size() > typeLine_.size())
            sent_->setText((sent_->text() + now.mid(typeLine_.size())).right(40));
    } else if (word) {
        if (now.size() > typeLine_.size() && now.endsWith(' ')
            && now.size() > typeSent_) {
            const QString out = now.mid(typeSent_);
            if (!out.trimmed().isEmpty()) {
                cw_->sendTyped(out);
                sent_->setText((sent_->text() + out).right(40));
            }
            typeSent_ = now.size();
        }
        if (now.size() < typeSent_) typeSent_ = now.size();
    }
    typeLine_ = now;
}

// ---- info line -------------------------------------------------------------

void CwDeck::onCallEdited() {
    refreshInfo();
}

void CwDeck::requestQrz(const QString& call) {
    const QString c = call.trimmed().toUpper();
    if (!qrz_ || c.size() < 3) return;
    if (QSettings().value("up/qrzweb/user").toString().trimmed().isEmpty())
        return;
    if (qrzGrid_.contains(c)) return;
    qrz_->lookup(c);
}

void CwDeck::refreshInfo() {
    const QString call = call_->text().trimmed().toUpper();
    const QString band = LogbookIndex::bandForHz(rigHz_);
    // Call colour: the band map's worked-before ramp.
    const QChar st = (idx_ && !call.isEmpty()) ? idx_->status(call, band)
                                               : QChar('?');
    const bool shaped = call.isEmpty() || callRe().match(call).hasMatch();
    call_->setStyleSheet(QString("QLineEdit { color: %1; }")
                             .arg(shaped ? QLatin1String(statusColor(st))
                                         : QLatin1String("#e0b45d")));
    QString line;
    spAz_ = lpAz_ = distKm_ = -1.0;
    double myLat = 0, myLon = 0, dxLat = 0, dxLon = 0;
    const bool haveMe = CtyLookup::gridToLatLon(
        QSettings().value("station/grid", "").toString(), myLat, myLon);
    bool haveDx = false;
    if (cty_ && !call.isEmpty()) {
        CtyInfo ci;
        if (cty_->info(call, ci)) line = ci.country;
        QString grid = ctx_ ? ctx_->grid() : QString();
        if (grid.isEmpty()) grid = qrzGrid_.value(call);
        if (CtyLookup::gridToLatLon(grid, dxLat, dxLon)) haveDx = true;
        else haveDx = cty_->lookup(call, dxLat, dxLon);
    }
    if (!call.isEmpty()) {
        switch (st.toLatin1()) {
            case 'W': line += " · wkd b4 " + band; break;
            case 'C': line += " · cfm " + band; break;
            case 'B': line += " · new band"; break;
            case 'N': line += " · NEW"; break;
            default: break;
        }
    }
    if (haveMe && haveDx) {
        spAz_ = bearing::initialDeg(myLat, myLon, dxLat, dxLon);
        lpAz_ = spAz_ >= 180.0 ? spAz_ - 180.0 : spAz_ + 180.0;
        distKm_ = bearing::distanceKm(myLat, myLon, dxLat, dxLon);
    }
    info_->setText(line);
    const bool can = rotor_ && rotor_->connected() && spAz_ >= 0;
    spBtn_->setEnabled(can);
    lpBtn_->setEnabled(can);
    spBtn_->setToolTip(spAz_ >= 0 ? QString("Short path %1°").arg(int(spAz_ + 0.5))
                                  : "Turn the rotor to the short path");
    lpBtn_->setToolTip(lpAz_ >= 0 ? QString("Long path %1°").arg(int(lpAz_ + 0.5))
                                  : "Turn the rotor to the long path");
}

void CwDeck::setStatus(const QString& s) { status_->setText(s); }

// ---- events ------------------------------------------------------------------

void CwDeck::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(true);
    for (auto it = panes_.begin(); it != panes_.end(); ++it)
        if (it->fly) it->fly->show();
    applyFkeyLabels();
    call_->setFocus();
}

void CwDeck::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(false);
    for (auto it = panes_.begin(); it != panes_.end(); ++it)
        if (it->fly) it->fly->hide();
}

bool CwDeck::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::Close) {
        for (auto it = panes_.begin(); it != panes_.end(); ++it)
            if (it->fly == obj) {
                QSettings().setValue("cwdeck/float/" + it.key() + "/geom",
                                     it->fly->saveGeometry());
                unfloatPane(it.key(), false);
                return true;
            }
    }
    // Plain Esc must reach the boxes (an app-level Escape would eat it).
    if (ev->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) { ev->accept(); return true; }
    }
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) { stopEverything(); return true; }
        // ↑/↓ = keying speed from ANY box, the type pane included
        // (operator, 2026-11-02): the hands never leave the keyboard.
        if ((ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
            && ke->modifiers() == Qt::NoModifier) {
            nudgeSpeed(ke->key() == Qt::Key_Up ? +1 : -1);
            return true;
        }
        if (obj == type_) {
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                sendTypedLine();
                return true;
            }
            // Earlier lines are history: typing always lands on the last.
            QTextCursor c = type_->textCursor();
            if (c.block() != type_->document()->lastBlock()) {
                c.movePosition(QTextCursor::End);
                type_->setTextCursor(c);
            }
        }
        if (obj == call_ && (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)
            && (call_->text().isEmpty() || callFromSpot_)) {
            // The arrows walk the spots while the box is empty OR still
            // holding an untouched landing — ←/→ ROLL through the band.
            emit walkSpots(ke->key() == Qt::Key_Right ? +1 : -1);
            return true;
        }
    }
    if (obj == read_->viewport() && ev->type() == QEvent::MouseButtonDblClick) {
        const QTextCursor c = read_->cursorForPosition(
            static_cast<QMouseEvent*>(ev)->pos());
        const QString line = c.block().text();
        int i = std::min(c.positionInBlock(), int(line.size()) - 1);
        if (line.isEmpty() || i < 0) return true;
        int a = i, b = i;
        while (a > 0 && !line[a - 1].isSpace()) --a;
        while (b < int(line.size()) && !line[b].isSpace()) ++b;
        QString tok = line.mid(a, b - a).toUpper();
        tok.remove(QRegularExpression(QStringLiteral("[^A-Z0-9/]")));
        if (callRe().match(tok).hasMatch()) prefillCall(tok, rigHz_);
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

// ---- pop-outs + text size ------------------------------------------------

void CwDeck::sizeMenu(QPlainTextEdit* pane, const QString& key) {
    const QString skey = "cwdeck/" + key + "Pt";
    const auto apply = [pane, skey](int pt) {
        pt = std::clamp(pt, 8, 28);
        QFont mf("DejaVu Sans Mono");
        mf.setPointSize(pt);
        pane->setFont(mf);
        QSettings().setValue(skey, pt);
    };
    apply(QSettings().value(skey, 13).toInt());
    pane->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pane, &QPlainTextEdit::customContextMenuRequested, this,
            [pane, skey, apply](const QPoint& p) {
                QMenu m(pane);
                m.addAction("Clear", pane, &QPlainTextEdit::clear);
                m.addAction("Text bigger", pane, [skey, apply] {
                    apply(QSettings().value(skey, 13).toInt() + 1);
                });
                m.addAction("Text smaller", pane, [skey, apply] {
                    apply(QSettings().value(skey, 13).toInt() - 1);
                });
                m.exec(pane->mapToGlobal(p));
            });
}

void CwDeck::addPopout(const QString& key, const QString& title,
                       QWidget* pane, QHBoxLayout* headerRow,
                       std::function<void(QWidget*)> reinsert) {
    PaneSlot s;
    s.pane = pane;
    s.title = title;
    s.reinsert = std::move(reinsert);
    panes_.insert(key, s);
    auto* pop = new QPushButton("⧉", pane);
    pop->setFlat(true);
    pop->setFocusPolicy(Qt::NoFocus);
    pop->setCursor(Qt::PointingHandCursor);
    pop->setStyleSheet("color:#8798a8; font-size:11px; border:0;");
    pop->setToolTip("Pop out as its own window; closing it returns it");
    connect(pop, &QPushButton::clicked, this, [this, key] { floatPane(key); });
    headerRow->addWidget(pop);
}

void CwDeck::floatPane(const QString& key) {
    PaneSlot& s = panes_[key];
    if (!s.pane) return;
    if (s.fly) { s.fly->show(); s.fly->raise(); return; }
    auto* d = new QDialog(window());
    d->setObjectName("contestPane");
    d->setAttribute(Qt::WA_StyledBackground, true);
    d->setStyleSheet(kDeckStyle);
    d->setWindowTitle(s.title + " — CW");
    auto* v = new QVBoxLayout(d);
    v->setContentsMargins(6, 6, 6, 6);
    v->addWidget(s.pane);
    const QByteArray g =
        QSettings().value("cwdeck/float/" + key + "/geom").toByteArray();
    if (!g.isEmpty()) d->restoreGeometry(g);
    else d->resize(430, 300);
    d->installEventFilter(this);
    s.fly = d;
    d->show();
}

void CwDeck::unfloatPane(const QString& key, bool saveGeom) {
    PaneSlot& s = panes_[key];
    if (!s.fly) return;
    if (saveGeom)
        QSettings().setValue("cwdeck/float/" + key + "/geom",
                             s.fly->saveGeometry());
    s.reinsert(s.pane);
    s.pane->show();
    s.fly->deleteLater();
    s.fly = nullptr;
}

} // namespace ttc
