// SPDX-License-Identifier: GPL-3.0-or-later
#include "rtty/RttyDeck.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "cw/CwMacros.h"
#include "log/QrzLookup.h"
#include "log/QsoContext.h"
#include "net/RotorLink.h"
#include "rtty/RttyEngine.h"
#include "ui/DeckStyle.h"
#include "util/Bearing.h"
#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

// Mark / space as two bars with the keying contrast between them: both
// tall and alternating = on frequency; one tall = off by a shift; both
// short = nothing there. Green while the squelch is open.
class RttyTuneWidget : public QWidget {
public:
    explicit RttyTuneWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(60, 56);
        setToolTip("Mark (left) and space (right) tone levels. Click the signal on\n"
                   "the waterfall; the AFC pulls in the last +-200 Hz. Both bars tall\n"
                   "and swapping as the station keys = tuned; green = the squelch is\n"
                   "open and text prints. Below: signal over noise, keying contrast,\n"
                   "and the mark tone with the AFC's trim.");
    }
    void set(float mark, float space, float quality, float snrDb, bool open,
             float afcHz, int markHz) {
        m_ = mark; s_ = space; q_ = quality; snr_ = snrDb; open_ = open;
        afc_ = afcHz; markHz_ = markHz;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0e141b"));
        const int w = width(), h = height() - 24;
        const int bw = std::max(6, (w - 18) / 2);
        const QColor on = open_ ? QColor("#82de8c") : QColor("#6aa5d8");
        const auto bar = [&](int x, float v) {
            const int bh = int(std::clamp(v, 0.0f, 1.0f) * (h - 4));
            p.fillRect(x, 2 + (h - 4 - bh), bw, bh, on);
            p.setPen(QColor("#2a3644"));
            p.drawRect(x, 2, bw, h - 4);
        };
        bar(6, m_);
        bar(12 + bw, s_);
        p.setPen(QColor("#8798a8"));
        QFont f = p.font();
        f.setPointSize(8);
        p.setFont(f);
        p.drawText(QRect(0, h, w, 12), Qt::AlignCenter,
                   QString("%1 dB · %2").arg(int(snr_)).arg(q_, 0, 'f', 2));
        // Where the pair sits: the nominal mark and the AFC's trim on it.
        p.drawText(QRect(0, h + 12, w, 12), Qt::AlignCenter,
                   QString("%1%2%3").arg(markHz_)
                       .arg(afc_ >= 0 ? "+" : "").arg(int(afc_)));
    }
private:
    float m_ = 0, s_ = 0, q_ = 0, snr_ = 0, afc_ = 0;
    int markHz_ = 2125;
    bool open_ = false;
};

namespace {
const char* kCap = "color:#8798a8; font-size:10px;";
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
// First-run macros: the RTTY QSO as it is actually sent on the air,
// in the CW window's plain-language fields.
const char* kDefaultFkeys[12] = {
    "CQ|CQ CQ CQ DE [my call] [my call] [my call] CQ CQ CQ DE [my call] [my call] [my call] PSE K",
    "HIS|[his call] [his call] DE [my call] [my call] K",
    "RST|[his call] DE [my call] [greeting] TNX FER CALL UR RST [rst sent] [rst sent] NAME [my name] [my name] QTH [my qth] [my qth] HW? [his call] DE [my call] K",
    "TU|[his call] DE [my call] TNX FER QSO [his name] 73 73 ES GL [his call] DE [my call] SK",
    "AGN|AGN AGN PSE K",
    "RY|RYRYRYRYRYRYRYRY",
    "QRZ|QRZ? DE [my call] [my call] K",
    "MY CALL|DE [my call] [my call] K",
    "QTH|QTH [my qth] [my qth]",
    "NAME|NAME [my name] [my name]",
    "73|73 73 [his name] DE [my call] SK",
    "",
};
QPushButton* toggleBtn(const char* text, const QString& tip, QWidget* parent) {
    auto* b = new QPushButton(QLatin1String(text), parent);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    b->setFixedHeight(20);
    return b;
}
}  // namespace

RttyDeck::RttyDeck(RttyEngine* eng, LogbookIndex* idx, const CtyLookup* cty,
                   RotorLink* rotor, QrzLookup* qrz, QWidget* parent)
    : QWidget(parent), eng_(eng), idx_(idx), cty_(cty), rotor_(rotor), qrz_(qrz) {
    setObjectName("rttyDeck");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(kDeckStyle);
    {
        QSettings s;
        for (int i = 0; i < 11; ++i) {
            const QString k = QString("rtty/fkey/F%1").arg(i + 1);
            if (!s.contains(k)) s.setValue(k, QLatin1String(kDefaultFkeys[i]));
        }
    }
    buildUi();
    if (qrz_) {
        connect(qrz_, &QrzLookup::result, this,
                [this](const QString& call, bool ok, const QString& name,
                       const QString& qth, const QString& grid, const QString&) {
                    if (!ok) return;
                    const QString c = call.trimmed().toUpper();
                    if (!grid.trimmed().isEmpty()) qrzGrid_.insert(c, grid.trimmed().toUpper());
                    if (c != call_->text().trimmed().toUpper()) return;
                    if (ctx_) {
                        if (ctx_->name().isEmpty()) ctx_->setName(name);
                        if (ctx_->qth().isEmpty()) ctx_->setQth(qth);
                        if (ctx_->grid().isEmpty()) ctx_->setGrid(grid);
                    }
                    refreshInfo();
                });
    }
    if (rotor_)
        connect(rotor_, &RotorLink::azimuthChanged, this, [this](double) { refreshInfo(); });
    if (idx_)
        connect(idx_, &LogbookIndex::updated, this, [this] { refreshInfo(); });
    if (eng_) {
        connect(eng_, &RttyEngine::txProgress, this, [this](int n) {
            if (txOn_) setStatus(n > 0 ? QString("TX · %1 chars to go").arg(n)
                                       : QStringLiteral("TX"));
        });
    }
    tick_ = new QTimer(this);
    tick_->setInterval(50);
    connect(tick_, &QTimer::timeout, this, [this] {
        if (!eng_ || !tune_) return;
        const auto st = eng_->rxStatus();
        tune_->set(st.mark, st.space, st.quality, st.snrDb, st.open, st.afcHz, eng_->markHz());
    });
}

void RttyDeck::buildUi() {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);

    // ---- RTTY READ (left third) ------------------------------------------
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
        auto* rrow = new QHBoxLayout;
        rrow->setSpacing(4);
        read_ = new QPlainTextEdit(this);
        read_->setReadOnly(true);
        read_->setMaximumBlockCount(300);
        read_->setObjectName("rttyRead");
        read_->setStyleSheet("QPlainTextEdit#rttyRead { background:#2b5c8a;"
                             " color:#ffffff; border:1px solid #3d7ab6;"
                             " selection-background-color:#1c2430; }");
        read_->setMinimumWidth(120);
        read_->setToolTip("Decoded RTTY from the radio's audio. Double-click a "
                          "call to work it · right-click: text size");
        sizeMenu(read_, "read");
        connect(clr, &QPushButton::clicked, read_, &QPlainTextEdit::clear);
        read_->viewport()->installEventFilter(this);
        rrow->addWidget(read_, 1);
        tune_ = new RttyTuneWidget(this);
        tune_->setFixedWidth(64);
        rrow->addWidget(tune_);
        box->addLayout(rrow, 1);
        lay->addWidget(readPane_, 3);
        addPopout("read", "RTTY READ", readPane_, trow, [this](QWidget* w) {
            static_cast<QHBoxLayout*>(layout())->insertWidget(0, w, 3);
        });
    }

    // ---- middle third: the QSO -------------------------------------------
    {
        auto* mid = new QVBoxLayout;
        mid->setSpacing(3);
        auto* r1 = new QHBoxLayout;
        r1->setSpacing(4);
        QFont big;
        big.setPointSize(16);
        big.setBold(true);
        const auto cap = [this](const char* t) {
            auto* l = new QLabel(QLatin1String(t), this);
            l->setStyleSheet(kCap);
            l->setObjectName("cap");
            return l;
        };
        r1->addWidget(cap("CALL"));
        call_ = new QLineEdit(this);
        call_->setFont(big);
        call_->setMaxLength(16);
        call_->setMinimumWidth(118);
        call_->setMaximumWidth(150);
        call_->setToolTip("The station you're working — what [his call] sends.\n"
                          "Enter moves to RST R · ←/→ (empty) walk the spots.");
        call_->installEventFilter(this);
        r1->addWidget(call_);
        r1->addWidget(cap("NAME"));
        name_ = new QLineEdit(this);
        name_->setFont(big);
        name_->setMaxLength(14);
        name_->setMinimumWidth(56);
        name_->setToolTip("The name he GIVES you — what [his name] sends. Enter logs.");
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
        rst("S", &rstS_, "RST you SENT — what [rst sent] sends. Enter logs.");
        rst("R", &rstR_, "RST he GAVE you. Enter logs.");
        auto* logBtn = new QPushButton("LOG IT", this);
        logBtn->setFocusPolicy(Qt::NoFocus);
        logBtn->setStyleSheet("QPushButton { background:#2b5c8a; color:#ffffff;"
                              " border:1px solid #5db2f0; font-weight:bold;"
                              " padding:2px 8px; }");
        logBtn->setToolTip("Log the QSO as RTTY: call, both reports, name, dial, UTC now.");
        connect(logBtn, &QPushButton::clicked, this, [this] { emit logRequested(); });
        r1->addWidget(logBtn);
        for (QLineEdit* e : {call_, name_, rstS_, rstR_}) e->setFixedHeight(30);
        mid->addLayout(r1);

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
                emit rotorAimed(lpAz_, -1.0, call_->text().trimmed().toUpper() + " LP");
            }
        });
        mid->addLayout(r2);

        auto* fg = new QGridLayout;
        fg->setSpacing(3);
        for (int i = 0; i < 12; ++i) {
            fk_[i] = new QPushButton(this);
            fk_[i]->setFocusPolicy(Qt::NoFocus);
            fk_[i]->setFixedHeight(22);
            fk_[i]->setMinimumWidth(40);
            fk_[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            QFont f = fk_[i]->font();
            f.setPointSize(std::max(7, f.pointSize() - 1));
            fk_[i]->setFont(f);
            connect(fk_[i], &QPushButton::clicked, this, [this, i] { keyFkey(i); });
            fk_[i]->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(fk_[i], &QPushButton::customContextMenuRequested, this,
                    [this, i](const QPoint&) { if (i != 11) editFkey(i); });
            fg->addWidget(fk_[i], i / 6, i % 6);
            fg->setColumnStretch(i % 6, 1);
        }
        mid->addLayout(fg);
        applyFkeyLabels();

        // Row 5: TX · NARROW · BAUD · SHIFT · REV USOS DIDL SQL · LVL
        auto* r5 = new QHBoxLayout;
        r5->setSpacing(4);
        txBtn_ = new QPushButton("TX", this);
        txBtn_->setCheckable(true);
        txBtn_->setFocusPolicy(Qt::NoFocus);
        txBtn_->setFixedWidth(44);
        txBtn_->setStyleSheet("QPushButton:checked { background:#8a2b2b; border-color:#f05d5d; }");
        txBtn_->setToolTip("Hold the key up between lines (a rag-chew). Off = each\n"
                           "F-key or Enter is one over: key, send, un-key.\n"
                           "Click again or Esc to drop. MOX / space do the same.");
        connect(txBtn_, &QPushButton::toggled, this, [this](bool on) {
            if (txBtn_->signalsBlocked()) return;
            emit txHoldChanged(on);
        });
        r5->addWidget(txBtn_);
        narrowBtn_ = toggleBtn("NARROW", "500 Hz filter centred on the tone pair (the\n"
                                         "radio's PBT moves it; off = the filter you had)", this);
        connect(narrowBtn_, &QPushButton::toggled, this, [this](bool on) {
            if (!narrowBtn_->signalsBlocked()) emit narrowRequested(on);
        });
        r5->addWidget(narrowBtn_);
        r5->addWidget(cap("BAUD"));
        const char* kCombo =
            "QComboBox { background:#1c2430; color:#e8eef5; border:1px solid #2a3644;"
            " border-radius:3px; padding:1px 4px; }"
            "QComboBox QAbstractItemView { background:#1c2430; color:#e8eef5;"
            " selection-background-color:#2b5c8a; }";
        baud_ = new QComboBox(this);
        baud_->addItems({"45.45", "50", "75"});
        baud_->setFocusPolicy(Qt::NoFocus);
        baud_->setStyleSheet(kCombo);
        baud_->setToolTip("Signalling rate: 45.45 Bd is amateur RTTY; 50 / 75 for the rare others");
        r5->addWidget(baud_);
        r5->addWidget(cap("SHIFT"));
        shift_ = new QComboBox(this);
        shift_->addItems({"170", "200", "425", "850"});
        shift_->setFocusPolicy(Qt::NoFocus);
        shift_->setStyleSheet(kCombo);
        shift_->setToolTip("Mark-space shift in Hz: 170 is amateur RTTY");
        r5->addWidget(shift_);
        revBtn_ = toggleBtn("REV", "Swap mark and space (a station on the other sideband)", this);
        usosBtn_ = toggleBtn("USOS", "Unshift on space: a space returns to letters (most stations)", this);
        diddleBtn_ = toggleBtn("DIDL", "Diddle: send LTRS while keyed with nothing to say", this);
        sqlBtn_ = toggleBtn("SQL", "Squelch: print only while the tones key cleanly (off = print everything)", this);
        afcBtn_ = toggleBtn("AFC", "Pull the tone pair onto the signal, +-200 Hz (off = the tones stay put)", this);
        for (QPushButton* b : {revBtn_, usosBtn_, diddleBtn_, sqlBtn_, afcBtn_}) r5->addWidget(b);
        r5->addWidget(cap("LVL"));
        level_ = new QSpinBox(this);
        level_->setRange(-30, 0);
        level_->setSuffix(" dB");
        level_->setFixedWidth(68);
        level_->setFocusPolicy(Qt::ClickFocus);
        level_->setToolTip("AFSK tone level into the SignaLink (dBFS peak). Set it so\n"
                           "the radio's ALC barely moves — over-driven AFSK splatters.");
        r5->addWidget(level_);
        r5->addStretch(1);
        mid->addLayout(r5);
        pullSettings();
        connect(baud_, &QComboBox::currentIndexChanged, this, [this] { pushSettings(); });
        connect(shift_, &QComboBox::currentIndexChanged, this, [this] { pushSettings(); });
        for (QPushButton* b : {revBtn_, usosBtn_, diddleBtn_, sqlBtn_, afcBtn_})
            connect(b, &QPushButton::toggled, this, [this] { pushSettings(); });
        connect(level_, &QSpinBox::valueChanged, this, [this] { pushSettings(); });

        auto* r6 = new QHBoxLayout;
        status_ = new QLabel(this);
        status_->setStyleSheet(kCap);
        status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        r6->addWidget(status_, 1);
        auto* stop = new QPushButton("STOP", this);
        stop->setFocusPolicy(Qt::NoFocus);
        stop->setToolTip("Drop the transmission now (Esc)");
        connect(stop, &QPushButton::clicked, this, [this] { stopEverything(); });
        r6->addWidget(stop);
        mid->addLayout(r6);
        mid->addStretch(1);
        lay->addLayout(mid, 4);
    }

    // ---- RTTY TYPE (right third) -------------------------------------------
    {
        typePane_ = new QWidget(this);
        auto* box = new QVBoxLayout(typePane_);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(2);
        auto* thdr = new QHBoxLayout;
        auto* t = new QLabel("RTTY TYPE — Enter sends the line", this);
        t->setStyleSheet(kCap);
        thdr->addWidget(t);
        thdr->addStretch(1);
        box->addLayout(thdr);
        type_ = new QPlainTextEdit(this);
        type_->setObjectName("rttyType");
        type_->setStyleSheet("QPlainTextEdit#rttyType { background:#2b5c8a;"
                             " color:#ffffff; border:1px solid #3d7ab6;"
                             " selection-background-color:#1c2430; }");
        type_->setMaximumBlockCount(100);
        type_->setMinimumWidth(120);
        type_->setToolTip("Type; Enter keys the radio and sends the line (fields like\n"
                          "[his call] expand), history stays above. With TX held the\n"
                          "key stays up between lines. Esc stops · right-click: text size");
        sizeMenu(type_, "type");
        type_->installEventFilter(this);
        box->addWidget(type_, 1);
        auto* brow = new QHBoxLayout;
        sent_ = new QLabel(this);
        sent_->setStyleSheet("color:#a4cf82; font-size:10px;");
        sent_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        brow->addWidget(sent_, 1);
        box->addLayout(brow);
        lay->addWidget(typePane_, 3);
        addPopout("type", "RTTY TYPE", typePane_, thdr, [this](QWidget* w) {
            static_cast<QHBoxLayout*>(layout())->addWidget(w, 3);
        });
    }

    const auto sc = [this](const QKeySequence& k, auto fn) {
        auto* s = new QShortcut(k, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, fn);
        shortcuts_ << s;
    };
    for (int i = 0; i < 12; ++i) sc(QKeySequence(Qt::Key_F1 + i), [this, i] { keyFkey(i); });
    for (QShortcut* s : shortcuts_) s->setEnabled(false);

    connect(call_, &QLineEdit::textEdited, this, [this](const QString& t) {
        QString up = t.toUpper();
        up.remove(QLatin1Char(' '));
        if (up != t) {
            const int pos = call_->cursorPosition();
            call_->setText(up);
            call_->setCursorPosition(std::min(pos, int(up.size())));
        }
        callFromSpot_ = false;
        refreshInfo();
    });
    connect(call_, &QLineEdit::returnPressed, this, [this] {
        requestQrz(call_->text());
        rstR_->setFocus();
        rstR_->selectAll();
    });
    connect(call_, &QLineEdit::editingFinished, this, [this] { requestQrz(call_->text()); });
    for (QLineEdit* e : {name_, rstS_, rstR_})
        connect(e, &QLineEdit::returnPressed, this, [this] { emit logRequested(); });

    for (QLabel* l : findChildren<QLabel*>()) {
        if (l->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) continue;
        if (l->objectName() == QLatin1String("cap")) continue;
        auto sp = l->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Maximum);
        l->setSizePolicy(sp);
        l->setMinimumWidth(0);
    }
    refreshInfo();
}

// ---- settings mirrors ------------------------------------------------------

void RttyDeck::pullSettings() {
    if (!eng_) return;
    settingsEcho_ = true;
    const auto& s = eng_->settings();
    baud_->setCurrentIndex(s.baud > 60 ? 2 : s.baud > 48 ? 1 : 0);
    const int shifts[] = {170, 200, 425, 850};
    int si = 0;
    for (int i = 0; i < 4; ++i) if (shifts[i] == s.shiftHz) si = i;
    shift_->setCurrentIndex(si);
    revBtn_->setChecked(s.reverse);
    usosBtn_->setChecked(s.usos);
    diddleBtn_->setChecked(s.diddle);
    sqlBtn_->setChecked(s.squelch > 0.0);
    afcBtn_->setChecked(s.afc);
    level_->setValue(s.txLevelDb);
    settingsEcho_ = false;
}

void RttyDeck::pushSettings() {
    if (!eng_ || settingsEcho_) return;
    RttyEngine::Settings s = eng_->settings();
    const double bauds[] = {45.45, 50.0, 75.0};
    s.baud = bauds[std::clamp(baud_->currentIndex(), 0, 2)];
    const int shifts[] = {170, 200, 425, 850};
    s.shiftHz = shifts[std::clamp(shift_->currentIndex(), 0, 3)];
    s.reverse = revBtn_->isChecked();
    s.usos = usosBtn_->isChecked();
    s.diddle = diddleBtn_->isChecked();
    s.squelch = sqlBtn_->isChecked() ? 0.5 : 0.0;
    s.afc = afcBtn_->isChecked();
    s.txLevelDb = level_->value();
    eng_->apply(s);
    setStatus(QString("%1 Bd · %2 Hz%3").arg(s.baud).arg(s.shiftHz)
                  .arg(s.reverse ? " · REV" : ""));
}

// ---- context -----------------------------------------------------------------

void RttyDeck::setContext(QsoContext* ctx) {
    ctx_ = ctx;
    if (!ctx_) return;
    struct Link { QLineEdit* e; void (QsoContext::*set)(const QString&); };
    const Link links[] = {
        {call_, &QsoContext::setCall}, {name_, &QsoContext::setHandle},
        {rstS_, &QsoContext::setRstSent}, {rstR_, &QsoContext::setRstRcvd},
    };
    for (const Link& l : links)
        connect(l.e, &QLineEdit::textChanged, this, [this, l](const QString& t) {
            if (ctxEcho_) return;
            ctxEcho_ = true;
            (ctx_->*l.set)(t);
            ctxEcho_ = false;
        });
    connect(ctx_, &QsoContext::changed, this, [this] {
        if (ctxEcho_) return;
        ctxEcho_ = true;
        const auto put = [](QLineEdit* e, const QString& v) { if (e->text() != v) e->setText(v); };
        put(call_, ctx_->call());
        put(name_, ctx_->handle());
        put(rstS_, ctx_->rstSent());
        put(rstR_, ctx_->rstRcvd());
        ctxEcho_ = false;
        refreshInfo();
    });
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
    refreshInfo();
}

void RttyDeck::setRig(qint64 hz, const QString& adifMode) {
    const bool bandMoved = LogbookIndex::bandForHz(hz) != LogbookIndex::bandForHz(rigHz_);
    rigHz_ = hz;
    if (!adifMode.isEmpty()) rigMode_ = adifMode;
    if (bandMoved) refreshInfo();
}

void RttyDeck::prefillCall(const QString& call, qint64 hz) {
    Q_UNUSED(hz);
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return;
    const bool fresh = c != call_->text().trimmed().toUpper();
    call_->setText(c);
    if (fresh) {
        name_->clear();
        if (ctx_) { ctx_->setName(QString()); ctx_->setQth(QString()); ctx_->setGrid(QString()); }
    }
    callFromSpot_ = true;
    refreshInfo();
    if (fresh) requestQrz(c);
    if (isVisible()) { call_->setFocus(); call_->deselect(); }
}

void RttyDeck::showTx(bool on) {
    txOn_ = on;
    if (!on) {
        const QSignalBlocker b(txBtn_);
        txBtn_->setChecked(false);
    }
    setStatus(on ? QStringLiteral("TX") : QStringLiteral("RX"));
}

void RttyDeck::showNarrow(bool on) {
    const QSignalBlocker b(narrowBtn_);
    narrowBtn_->setChecked(on);
}

void RttyDeck::clearForNext() {
    callFromSpot_ = false;
    call_->clear();
    name_->clear();
    rstS_->setText("599");
    rstR_->setText("599");
    if (ctx_) { ctx_->setRstSent("599"); ctx_->setRstRcvd("599"); }
    refreshInfo();
    if (isVisible()) call_->setFocus();
}

void RttyDeck::appendRead(const QString& text) {
    read_->moveCursor(QTextCursor::End);
    read_->insertPlainText(text);
    read_->moveCursor(QTextCursor::End);
}

// ---- sending -------------------------------------------------------------------

QString RttyDeck::expand(const QString& text, QString* missing) const {
    QSettings s;
    CwMacroFields f;
    f.myCall = s.value("station/callsign").toString().toUpper();
    f.myName = s.value("station/name").toString();
    f.myQth = s.value("station/qth").toString();
    f.myGrid = s.value("station/grid").toString();
    f.hisCall = call_->text().trimmed().toUpper();
    if (ctx_) {
        f.hisHandle = ctx_->handle();
        f.hisName = ctx_->handle().isEmpty() ? ctx_->name() : ctx_->handle();
        f.hisQth = ctx_->qth();
        f.hisGrid = ctx_->grid();
        f.rstSent = ctx_->rstSent();
        f.rstRcvd = ctx_->rstRcvd();
    }
    f.greeting = cwGreetingFor(f.hisGrid);
    return expandCwMacro(text, f, missing);
}

void RttyDeck::sendText(const QString& text) {
    QString missing;
    const QString out = expand(text, &missing);
    if (!missing.isEmpty()) {
        setStatus(QString("nothing in %1 — fill it first").arg(missing));
        if (missing == QLatin1String("his call")) call_->setFocus();
        else if (missing == QLatin1String("his name")) name_->setFocus();
        return;
    }
    if (out.trimmed().isEmpty()) return;
    sent_->setText(out.right(60));
    // A line starts and ends on a fresh line at the far end.
    emit sendRequested("\r\n" + out.toUpper() + "\r\n", txBtn_->isChecked());
}

void RttyDeck::keyFkey(int idx0) {
    if (idx0 == 11) { wipe(); return; }
    const QString spec = QSettings().value(QString("rtty/fkey/F%1").arg(idx0 + 1)).toString();
    const QString text = spec.section('|', 1);
    if (text.trimmed().isEmpty()) return;
    sendText(text);
}

void RttyDeck::editFkey(int idx0) {
    const QString key = QString("rtty/fkey/F%1").arg(idx0 + 1);
    const QString spec = QSettings().value(key).toString();
    QDialog d(this);
    d.setWindowTitle(QString("F%1 — RTTY macro").arg(idx0 + 1));
    auto* form = new QFormLayout(&d);
    auto* label = new QLineEdit(spec.section('|', 0, 0), &d);
    auto* text = new QLineEdit(spec.section('|', 1), &d);
    text->setMinimumWidth(480);
    form->addRow("Label", label);
    form->addRow("Sends", text);
    QString fields;
    for (const CwFieldInfo& fi : cwMacroFields())
        fields += QString("[%1] %2\n").arg(fi.token, fi.what);
    auto* help = new QLabel(fields.trimmed(), &d);
    help->setStyleSheet(kCap);
    form->addRow(help);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(bb);
    if (d.exec() != QDialog::Accepted) return;
    QSettings().setValue(key, label->text().trimmed() + "|" + text->text());
    applyFkeyLabels();
}

void RttyDeck::applyFkeyLabels() {
    QSettings s;
    for (int i = 0; i < 12; ++i) {
        if (i == 11) {
            fk_[i]->setText("F12 WIPE");
            fk_[i]->setToolTip("Clear the entry for the next contact");
            continue;
        }
        const QString spec = s.value(QString("rtty/fkey/F%1").arg(i + 1)).toString();
        const QString label = spec.section('|', 0, 0).trimmed();
        fk_[i]->setText(QString("F%1 %2").arg(i + 1).arg(label.isEmpty() ? QStringLiteral("—") : label));
        fk_[i]->setToolTip(QString("F%1  %2\nsends:  %3\n\nclick or press F%1 · right-click to edit")
                               .arg(i + 1).arg(label, spec.section('|', 1)));
        fk_[i]->setEnabled(!spec.section('|', 1).trimmed().isEmpty());
    }
}

void RttyDeck::wipe() {
    clearForNext();
    setStatus("wiped");
}

void RttyDeck::stopEverything() {
    {
        const QSignalBlocker b(txBtn_);
        txBtn_->setChecked(false);
    }
    emit abortRequested();
    setStatus("stopped");
}

void RttyDeck::sendTypedLine() {
    const QString line = type_->document()->lastBlock().text();
    QTextCursor c = type_->textCursor();
    c.movePosition(QTextCursor::End);
    type_->setTextCursor(c);
    if (!line.isEmpty()) type_->insertPlainText("\n");
    if (!line.trimmed().isEmpty()) sendText(line);
}

// ---- info line ----------------------------------------------------------------

void RttyDeck::requestQrz(const QString& call) {
    const QString c = call.trimmed().toUpper();
    if (!qrz_ || c.size() < 3) return;
    if (QSettings().value("up/qrzweb/user").toString().trimmed().isEmpty()) return;
    if (qrzGrid_.contains(c)) return;
    qrz_->lookup(c);
}

void RttyDeck::refreshInfo() {
    const QString call = call_->text().trimmed().toUpper();
    const QString band = LogbookIndex::bandForHz(rigHz_);
    const QChar st = (idx_ && !call.isEmpty()) ? idx_->status(call, band) : QChar('?');
    const bool shaped = call.isEmpty() || callRe().match(call).hasMatch();
    call_->setStyleSheet(QString("QLineEdit { color: %1; }")
                             .arg(shaped ? QLatin1String(statusColor(st)) : QLatin1String("#e0b45d")));
    QString line;
    spAz_ = lpAz_ = distKm_ = -1.0;
    double myLat = 0, myLon = 0, dxLat = 0, dxLon = 0;
    const bool haveMe = CtyLookup::gridToLatLon(QSettings().value("station/grid", "").toString(), myLat, myLon);
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
    spBtn_->setToolTip(spAz_ >= 0 ? QString("Short path %1°").arg(int(spAz_ + 0.5)) : "Turn the rotor to the short path");
    lpBtn_->setToolTip(lpAz_ >= 0 ? QString("Long path %1°").arg(int(lpAz_ + 0.5)) : "Turn the rotor to the long path");
}

void RttyDeck::setStatus(const QString& s) { status_->setText(s); }

// ---- events ---------------------------------------------------------------------

void RttyDeck::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(true);
    for (auto it = panes_.begin(); it != panes_.end(); ++it) if (it->fly) it->fly->show();
    applyFkeyLabels();
    pullSettings();
    if (tick_) tick_->start();
    call_->setFocus();
}

void RttyDeck::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    for (QShortcut* s : shortcuts_) s->setEnabled(false);
    for (auto it = panes_.begin(); it != panes_.end(); ++it) if (it->fly) it->fly->hide();
    if (tick_) tick_->stop();
}

bool RttyDeck::eventFilter(QObject* obj, QEvent* ev) {
    if (ev->type() == QEvent::Close) {
        for (auto it = panes_.begin(); it != panes_.end(); ++it)
            if (it->fly == obj) {
                QSettings().setValue("rttydeck/float/" + it.key() + "/geom", it->fly->saveGeometry());
                unfloatPane(it.key(), false);
                return true;
            }
    }
    if (ev->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) { ev->accept(); return true; }
    }
    if (ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Escape) { stopEverything(); return true; }
        if (obj == type_) {
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                sendTypedLine();
                return true;
            }
            QTextCursor c = type_->textCursor();
            if (c.block() != type_->document()->lastBlock()) {
                c.movePosition(QTextCursor::End);
                type_->setTextCursor(c);
            }
        }
        if (obj == call_ && (ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)
            && (call_->text().isEmpty() || callFromSpot_)) {
            emit walkSpots(ke->key() == Qt::Key_Right ? +1 : -1);
            return true;
        }
    }
    if (obj == read_->viewport() && ev->type() == QEvent::MouseButtonDblClick) {
        const QTextCursor c = read_->cursorForPosition(static_cast<QMouseEvent*>(ev)->pos());
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

// ---- pop-outs + text size ------------------------------------------------------

void RttyDeck::sizeMenu(QPlainTextEdit* pane, const QString& key) {
    const QString skey = "rttydeck/" + key + "Pt";
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
                m.addAction("Text bigger", pane, [skey, apply] { apply(QSettings().value(skey, 13).toInt() + 1); });
                m.addAction("Text smaller", pane, [skey, apply] { apply(QSettings().value(skey, 13).toInt() - 1); });
                m.exec(pane->mapToGlobal(p));
            });
}

void RttyDeck::addPopout(const QString& key, const QString& title, QWidget* pane,
                         QHBoxLayout* headerRow, std::function<void(QWidget*)> reinsert) {
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

void RttyDeck::floatPane(const QString& key) {
    PaneSlot& s = panes_[key];
    if (!s.pane) return;
    if (s.fly) { s.fly->show(); s.fly->raise(); return; }
    auto* d = new QDialog(window());
    d->setObjectName("contestPane");
    d->setAttribute(Qt::WA_StyledBackground, true);
    d->setStyleSheet(kDeckStyle);
    d->setWindowTitle(s.title + " — RTTY");
    auto* v = new QVBoxLayout(d);
    v->setContentsMargins(6, 6, 6, 6);
    v->addWidget(s.pane);
    const QByteArray g = QSettings().value("rttydeck/float/" + key + "/geom").toByteArray();
    if (!g.isEmpty()) d->restoreGeometry(g);
    else d->resize(430, 300);
    d->installEventFilter(this);
    s.fly = d;
    d->show();
}

void RttyDeck::unfloatPane(const QString& key, bool saveGeom) {
    PaneSlot& s = panes_[key];
    if (!s.fly) return;
    if (saveGeom) QSettings().setValue("rttydeck/float/" + key + "/geom", s.fly->saveGeometry());
    s.reinsert(s.pane);
    s.pane->show();
    s.fly->deleteLater();
    s.fly = nullptr;
}

}  // namespace ttc
