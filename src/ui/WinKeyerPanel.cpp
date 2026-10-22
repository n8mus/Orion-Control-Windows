// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/WinKeyerPanel.h"
#include "cw/WinKeyer.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace ttc {

namespace {

// One dit at a given speed. PARIS standard, and the same 1200/WPM the
// K1EL manual uses in its own tail-delay worked examples (p8).
double ditMs(int wpm) { return 1200.0 / std::max(1, wpm); }

const char* kPanelStyle = R"(
    QDialog { background: #10151c; }
    QWidget#wkPage { background: #10151c; }
    QScrollArea { background: #10151c; border: none; }
    QScrollBar:vertical { background: #10151c; width: 10px; }
    QScrollBar::handle:vertical { background: #2a3644; border-radius: 5px;
                                  min-height: 24px; }
    QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
    QGroupBox { color: #8fa3b8; border: 1px solid #2a3644; border-radius: 3px;
                margin-top: 14px; font-size: 10px; font-weight: bold;
                padding-top: 8px; }
    QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }
    QPushButton { background: #1c2430; color: #c8d4e0; border: 1px solid #2a3644;
                  border-radius: 3px; font-size: 11px; padding: 3px 10px; }
    QPushButton:hover { background: #26303e; }
    QPushButton:disabled { color: #55616f; }
    QLabel { color: #c8d4e0; font-size: 11px; }
    QCheckBox { color: #c8d4e0; font-size: 11px; }
    QComboBox, QSpinBox { background: #1c2430; color: #c8d4e0;
                          border: 1px solid #2a3644; padding: 2px 6px;
                          font-size: 11px; }
    QSlider::groove:horizontal { height: 4px; background: #2a3644; border-radius: 2px; }
    QSlider::handle:horizontal { width: 12px; margin: -5px 0; border-radius: 6px;
                                 background: #6aa5d8; }
    QSlider::handle:horizontal:disabled { background: #47525f; }
)";

}  // namespace

// ── the shape preview ────────────────────────────────────────────────

CwShapeWidget::CwShapeWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(92);
}

void CwShapeWidget::setTiming(int wpm, int weight, int keyCompMs, int ratio,
                              int letterspace) {
    wpm_ = wpm; weight_ = weight; keyComp_ = keyCompMs;
    ratio_ = ratio; letterspace_ = letterspace;
    update();
}

void CwShapeWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(0x0b, 0x10, 0x16));

    const double dit = ditMs(wpm_);
    // Weighting is a proportion of the element (manual p8: weighted dit =
    // (weight/50) x dit); key compensation is a flat millisecond addition
    // on top (p13) and is deliberately NOT scaled by speed. Both are
    // taken back out of the following space, so total time is unchanged —
    // which is exactly why neither changes your WPM.
    const double wf   = weight_ / 50.0;
    const double dahF = 3.0 * (ratio_ / 50.0);
    const auto mark = [&](double units) { return units * dit * wf + keyComp_; };
    const auto gap  = [&](double units) {
        return std::max(0.0, units * dit * (2.0 - wf) - keyComp_);
    };
    // Extra letterspace, 0-15 in 2% steps, lands on the character gap.
    const double lsF = 1.0 + letterspace_ * 0.02;

    struct Seg { double ms; bool key; };
    const auto build = [&](bool adjusted) {
        QVector<Seg> s;
        const double m1 = adjusted ? mark(1) : dit;
        const double m3 = adjusted ? mark(dahF) : 3 * dit;
        const double g1 = adjusted ? gap(1) : dit;
        const double gc = (adjusted ? 3 * dit * lsF : 3 * dit);
        s << Seg{m1, true} << Seg{g1, false}       // R = dit dah dit
          << Seg{m3, true} << Seg{g1, false}
          << Seg{m1, true} << Seg{gc, false};
        return s;
    };
    const QVector<Seg> ghost = build(false);
    const QVector<Seg> live  = build(true);

    double total = 0;
    for (const Seg& s : live)  total += s.ms;
    for (double t = 0; const Seg& s : ghost) { t += s.ms; total = std::max(total, t); }
    if (total <= 0) return;

    const double pxPerMs = (width() - 20) / total;
    const auto drawTrace = [&](const QVector<Seg>& segs, int yTop, int h,
                               const QColor& col, bool filled) {
        double x = 10;
        for (const Seg& s : segs) {
            const double w = s.ms * pxPerMs;
            if (s.key) {
                const QRectF r(x, yTop, w, h);
                if (filled) p.fillRect(r, col);
                else { p.setPen(col); p.drawRect(r); }
            }
            x += w;
        }
    };
    // Ghost first (factory timing), live on top.
    drawTrace(ghost, 46, 22, QColor(0x35, 0x42, 0x52), true);
    drawTrace(live, 14, 24, QColor(0x6a, 0xa5, 0xd8), true);

    p.setPen(QColor(0x8f, 0xa3, 0xb8));
    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    p.drawText(10, 12, QString("R  at %1 WPM — your timing").arg(wpm_));
    p.drawText(10, 84, QString("same letter at the keyer's factory settings "
                               "(weight 50, comp 0, ratio 1:3)"));
}

// ── the panel ────────────────────────────────────────────────────────

WinKeyerPanel::WinKeyerPanel(WinKeyer* keyer, QWidget* parent)
    : QDialog(parent), wk_(keyer) {
    setWindowTitle("WinKeyer control");
    setStyleSheet(kPanelStyle);
    // The full parameter set is taller than any sane window, so the page
    // scrolls — same arrangement as Station setup, and the same two traps
    // it hit (00fd13b): a bare QWidget page misses "QDialog { background }"
    // so it must be named and painted, and inside a scroll area the wheel
    // lands on whatever control is under the pointer and silently edits
    // it. Here that would retune the keyer mid-scroll.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(scroll);
    page->setObjectName("wkPage");
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);

    fwLabel_ = new QLabel(this);
    fwLabel_->setStyleSheet("QLabel { color: #8fa3b8; }");
    const int rev = wk_ ? wk_->firmwareRev() : -1;
    fwLabel_->setText(
        rev > 0 ? QString("WinKeyer %1 · firmware rev %2 · host mode %3")
                      .arg(wk_->isWk3() ? "WK3" : "WK1/WK2").arg(rev)
                      .arg(wk_->wk2Mode() ? "WK2" : "WK1")
                : QString("keyer not open — controls will apply when it connects"));
    lay->addWidget(fwLabel_);

    // WK3 silicon maps three prosigns the older chips do not have. They
    // are typeable in the CW window's send line, so say so here where the
    // firmware is actually known.
    if (wk_ && wk_->isWk3()) {
        auto* extra = new QLabel(
            "WK3 prosigns, typeable in the CW window:  [ = AS   \\ = DN   "
            "] = KN   @ = AC", this);
        extra->setStyleSheet("QLabel { color: #6d7f92; font-size: 10px; }");
        lay->addWidget(extra);
    }

    shape_ = new CwShapeWidget(this);
    lay->addWidget(shape_);

    buildTimingGroup(lay);
    buildSendingGroup(lay);
    buildPttGroup(lay);
    buildPaddleGroup(lay);
    buildPresetRow(lay);
    lay->addStretch(1);

    refreshPreview();
    refreshPttTail();

    // Only a deliberately focused control takes the wheel; everything else
    // passes it to the page. Setup guards combos and spin boxes — this
    // panel is mostly SLIDERS, so they are guarded too, and a stray wheel
    // over "Weighting" while scrolling would otherwise retune the keyer
    // live with no indication which row moved.
    for (QWidget* w : findChildren<QWidget*>()) {
        if (qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w)
            || qobject_cast<QSlider*>(w)) {
            w->setFocusPolicy(Qt::StrongFocus);
            w->installEventFilter(this);
        }
    }

    // Natural size, capped to the screen; past that the scroll area works.
    if (QScreen* sc = QGuiApplication::primaryScreen()) {
        const QRect avail = sc->availableGeometry();
        const QSize want = page->sizeHint();
        resize(std::min(want.width() + 30, avail.width() - 40),
               std::min(want.height() + 20, int(avail.height() * 0.92)));
    }
}

bool WinKeyerPanel::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::Wheel) {
        auto* w = qobject_cast<QWidget*>(o);
        if (w && !w->hasFocus()) { e->ignore(); return true; }
    }
    return QDialog::eventFilter(o, e);
}

// Persist + adopt in one place: writing the key is what marks a parameter
// as owned by the console (WinKeyer::loadOwned only adopts keys present).
void WinKeyerPanel::setManaged(const QString& settingKey, int value) {
    QSettings().setValue("cw/wk/" + settingKey, value);
}

// One parameter = one self-contained widget: [name][slider][spin] with the
// explanation wrapped underneath. Built as a nested layout rather than
// grid cells because a word-wrapped QLabel does not report a useful height
// to QGridLayout and the caption ends up overlapping the row below it.
QSlider* WinKeyerPanel::addRow(QVBoxLayout* v, const QString& label,
                               const QString& hint, int lo, int hi, int cur,
                               const QString& settingKey,
                               std::function<void(int)> apply) {
    auto* row = new QWidget(this);
    auto* col = new QVBoxLayout(row);
    col->setContentsMargins(0, 0, 0, 4);
    col->setSpacing(1);
    auto* top = new QHBoxLayout();
    top->setSpacing(6);

    auto* name = new QLabel(label, row);
    name->setFixedWidth(96);
    auto* sl = new QSlider(Qt::Horizontal, row);
    sl->setRange(lo, hi);
    sl->setValue(cur);
    auto* spin = new QSpinBox(row);
    spin->setRange(lo, hi);
    spin->setValue(cur);
    spin->setFixedWidth(64);
    top->addWidget(name);
    top->addWidget(sl, 1);
    top->addWidget(spin);
    col->addLayout(top);

    // 40 ms trailing coalesce: the keyer runs at 1200 baud and a drag
    // would otherwise queue hundreds of writes behind the sending buffer.
    auto* tx = new QTimer(this);
    tx->setSingleShot(true);
    tx->setInterval(40);
    auto* pending = new int(cur);
    connect(tx, &QTimer::timeout, this, [this, pending, settingKey, apply] {
        apply(*pending);
        setManaged(settingKey, *pending);
        refreshPreview();
        refreshPttTail();
    });
    const auto changed = [this, sl, spin, tx, pending](int v) {
        if (sl->value() != v) { QSignalBlocker b(sl); sl->setValue(v); }
        if (spin->value() != v) { QSignalBlocker b(spin); spin->setValue(v); }
        *pending = v;
        if (!tx->isActive()) tx->start();
        refreshPreview();
    };
    connect(sl, &QSlider::valueChanged, this, changed);
    connect(spin, &QSpinBox::valueChanged, this, changed);

    if (!hint.isEmpty()) {
        auto* h = new QLabel(hint, row);
        h->setStyleSheet("QLabel { color: #6d7f92; font-size: 10px; }");
        h->setWordWrap(true);
        h->setContentsMargins(102, 0, 0, 0);   // line up under the slider
        col->addWidget(h);
    }
    v->addWidget(row);
    return sl;
}

void WinKeyerPanel::refreshPreview() {
    if (!shape_) return;
    shape_->setTiming(wpm_ ? wpm_->value() : 25,
                      weight_ ? weight_->value() : 50,
                      keyComp_ ? keyComp_->value() : 0,
                      ratio_ ? ratio_->value() : 50,
                      letterspace_ ? letterspace_->value() : 0);
}

// The tail the operator actually gets is not the number in the box: the
// manual's formula is three dit times PLUS tail x 10 ms (p8), which
// surprises people because it moves with speed.
void WinKeyerPanel::refreshPttTail() {
    if (!tailLabel_ || !pttTail_ || !wpm_) return;
    const double ms = 3.0 * ditMs(wpm_->value()) + pttTail_->value() * 10.0;
    tailLabel_->setText(
        QString("actual tail at %1 WPM = 3 dits + %2×10 ms = %3 ms")
            .arg(wpm_->value()).arg(pttTail_->value()).arg(qRound(ms)));
}

// The group that answers the actual complaint: the radio's own keyer
// sounds fuller than the WinKeyer. Every control here ships at "no
// adjustment" from the factory, which is why an untouched WinKeyer sounds
// mechanically exact. Ordered by how likely each is to be the fix.
void WinKeyerPanel::buildTimingGroup(QVBoxLayout* lay) {
    auto* box = new QGroupBox("SOUND / ELEMENT TIMING", this);
    auto* v = new QVBoxLayout(box);
    v->setSpacing(2);
    QSettings s;

    keyComp_ = addRow(v, "Key comp",
        "Adds this many ms to every dit and dah, taken back out of the "
        "spacing so speed is unchanged. K1EL: QSK rigs shorten elements, "
        "worst at high speed. Start here for choppiness.",
        0, 250, s.value("cw/wk/keyComp", 0).toInt(), "keyComp",
        [this](int val) { if (wk_) wk_->setKeyComp(val); });

    firstExt_ = addRow(v, "1st extension",
        "Stretches only the FIRST element after a silence, for rigs whose "
        "receive-to-transmit changeover clips it. K1EL: mainly audible "
        "above 25 WPM.",
        0, 250, s.value("cw/wk/firstExt", 0).toInt(), "firstExt",
        [this](int val) { if (wk_) wk_->setFirstExtension(val); });

    weight_ = addRow(v, "Weighting",
        "50 = no adjustment. Below is thinner, above is heavier. Tracks "
        "speed, so one value sounds right at every WPM.",
        10, 90, s.value("cw/wk/weight", 50).toInt(), "weight",
        [this](int val) { if (wk_) wk_->setWeighting(val); });

    ratio_ = addRow(v, "Dit/dah ratio",
        "50 = a textbook 1:3. 33 = 1:2, 66 = 1:4. K1EL's own warning: some "
        "ops use this to sound less machine-like, but a little goes a long "
        "way.",
        33, 66, s.value("cw/wk/ratio", 50).toInt(), "ratio",
        [this](int val) { if (wk_) wk_->setRatio(val); });

    letterspace_ = addRow(v, "Letterspace",
        "Extra gap between characters, 2% per step. The radio's own keyer "
        "measures a 4.2-unit character gap against the textbook 3 — much "
        "of why it sounds less crowded. Moves the keyer to WK2 host mode.",
        0, 15, s.value("cw/wk/letterspace", 0).toInt(), "letterspace",
        [this](int val) { if (wk_) wk_->setLetterspace(val); });

    lay->addWidget(box);
}

void WinKeyerPanel::buildSendingGroup(QVBoxLayout* lay) {
    auto* box = new QGroupBox("SENDING", this);
    auto* v = new QVBoxLayout(box);
    v->setSpacing(2);
    QSettings s;

    wpm_ = addRow(v, "Speed (WPM)", "",
        5, 99, s.value("cw/wpm", 25).toInt(), "wpm",
        [this](int val) {
            if (wk_) wk_->setSpeed(val);
            QSettings().setValue("cw/wpm", val);  // shared with the CW window
        });

    farns_ = addRow(v, "Farnsworth",
        "0 = off. Characters are sent at this speed while the spacing "
        "stays at the sending speed.",
        0, 99, s.value("cw/wk/farnsworth", 0).toInt(), "farnsworth",
        [this](int val) { if (wk_) wk_->setFarnsworth(val); });

    auto* stRow = new QHBoxLayout();
    auto* stName = new QLabel("Sidetone", this);
    stName->setFixedWidth(96);
    stRow->addWidget(stName);
    sidetone_ = new QComboBox(this);
    // WK1/WK2 sidetone is a table index, not a frequency (manual p7).
    const int freqs[] = {4000, 2000, 1333, 1000, 800, 666, 571, 500, 444, 400};
    for (int i = 0; i < 10; ++i)
        sidetone_->addItem(QString("%1 Hz").arg(freqs[i]), i + 1);
    sidetone_->setCurrentIndex(
        std::clamp(s.value("cw/wk/sidetone", 5).toInt(), 1, 10) - 1);
    connect(sidetone_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const int n = sidetone_->itemData(i).toInt();
        if (wk_) wk_->setSidetone(n);
        setManaged("sidetone", n);
    });
    stRow->addWidget(sidetone_, 1);
    v->addLayout(stRow);
    auto* stHint = new QLabel(
        "The keyer's own beeper, not the radio's monitor. WK1/WK2 host "
        "mode offers these ten fixed steps only.", this);
    stHint->setStyleSheet("QLabel { color: #6d7f92; font-size: 10px; }");
    stHint->setWordWrap(true);
    stHint->setContentsMargins(102, 0, 0, 0);
    v->addWidget(stHint);

    lay->addWidget(box);
}

void WinKeyerPanel::buildPttGroup(QVBoxLayout* lay) {
    auto* box = new QGroupBox("PTT / AMPLIFIER KEYING", this);
    auto* v = new QVBoxLayout(box);
    v->setSpacing(2);
    QSettings s;

    const auto sendLeadTail = [this] {
        if (wk_ && pttLead_ && pttTail_)
            wk_->setPttLeadTail(pttLead_->value(), pttTail_->value());
    };
    pttLead_ = addRow(v, "Lead-in",
        "PTT closes this long before keying starts. Too short and the "
        "first element is clipped by the relay. Units of 10 ms.",
        0, 250, s.value("cw/wk/pttLead", 0).toInt(), "pttLead",
        [sendLeadTail](int) { sendLeadTail(); });
    pttTail_ = addRow(v, "Tail",
        "How long PTT is held after the last element. Units of 10 ms.",
        0, 250, s.value("cw/wk/pttTail", 0).toInt(), "pttTail",
        [sendLeadTail](int) { sendLeadTail(); });

    tailLabel_ = new QLabel(this);
    tailLabel_->setStyleSheet("QLabel { color: #6d7f92; font-size: 10px; }");
    tailLabel_->setContentsMargins(102, 0, 0, 0);
    v->addWidget(tailLabel_);
    lay->addWidget(box);
}

// One-way settings: WK3's "Get Values" admin command always returns 0, so
// there is no way to read the owner's paddle setup back off the keyer.
// Off by default, and the checkbox says exactly why.
void WinKeyerPanel::buildPaddleGroup(QVBoxLayout* lay) {
    // "&&" — a lone & is a Qt mnemonic and renders as an underscore.
    paddleBox_ = new QGroupBox("PADDLE && MODE REGISTER", this);
    auto* v = new QVBoxLayout(paddleBox_);
    QSettings s;

    manageMode_ = new QCheckBox("Let the console manage these", this);
    manageMode_->setChecked(s.value("cw/wk/manageMode", false).toBool());
    v->addWidget(manageMode_);
    auto* warn = new QLabel(
        "The keyer cannot report these back — its \"get values\" command "
        "always answers zero. Leave this off and your paddle keeps "
        "whatever you set on the keyer itself. Turn it on and the console "
        "writes them at every connect, with no way to recover the "
        "originals. Nothing above this box is affected.", this);
    warn->setStyleSheet("QLabel { color: #c8a45c; font-size: 10px; }");
    warn->setWordWrap(true);
    v->addWidget(warn);

    auto* g = new QGridLayout();
    g->setColumnStretch(1, 1);
    g->addWidget(new QLabel("Paddle mode", this), 0, 0);
    keyMode_ = new QComboBox(this);
    keyMode_->addItem("Iambic B", 0);       // mode-register bits 5:4
    keyMode_->addItem("Iambic A", 1);
    keyMode_->addItem("Ultimatic", 2);
    keyMode_->addItem("Bug", 3);
    g->addWidget(keyMode_, 0, 1);
    swap_      = new QCheckBox("Swap dit/dah paddles", this);
    autospace_ = new QCheckBox("Autospace", this);
    ctSpace_   = new QCheckBox("Contest word spacing (6 dits, not 7)", this);
    g->addWidget(swap_, 1, 0, 1, 2);
    g->addWidget(autospace_, 2, 0, 1, 2);
    g->addWidget(ctSpace_, 3, 0, 1, 2);
    v->addLayout(g);

    switchpoint_ = addRow(v, "Switchpoint",
        "When the keyer starts looking for the next paddle press, as a "
        "percentage of a dit. 50 = one dit time.",
        10, 90, s.value("cw/wk/switchpoint", 50).toInt(), "switchpoint",
        [this](int val) { if (wk_) wk_->setSwitchpoint(val); });

    // Mode-register bits per manual Table 12 (p11).
    const int bits = s.value("cw/wk/modeReg", 0).toInt();
    keyMode_->setCurrentIndex((bits >> 4) & 0x03);
    swap_->setChecked(bits & 0x08);
    autospace_->setChecked(bits & 0x02);
    ctSpace_->setChecked(bits & 0x01);

    const auto pushMode = [this] {
        if (!manageMode_->isChecked()) return;
        int b = 0;
        b |= (keyMode_->currentData().toInt() & 0x03) << 4;
        if (swap_->isChecked())      b |= 0x08;
        if (autospace_->isChecked()) b |= 0x02;
        if (ctSpace_->isChecked())   b |= 0x01;
        if (wk_) wk_->setModeRegister(b);
        setManaged("modeReg", b);
    };
    connect(keyMode_, &QComboBox::currentIndexChanged, this, pushMode);
    for (QCheckBox* c : {swap_, autospace_, ctSpace_})
        connect(c, &QCheckBox::toggled, this, pushMode);

    const auto gate = [this, pushMode](bool on) {
        QSettings().setValue("cw/wk/manageMode", on);
        keyMode_->setEnabled(on);
        swap_->setEnabled(on);
        autospace_->setEnabled(on);
        ctSpace_->setEnabled(on);
        if (on) pushMode();
    };
    connect(manageMode_, &QCheckBox::toggled, this, gate);
    gate(manageMode_->isChecked());

    lay->addWidget(paddleBox_);
}

void WinKeyerPanel::buildPresetRow(QVBoxLayout* lay) {
    auto* row = new QHBoxLayout();
    presets_ = new QComboBox(this);
    presets_->setMinimumWidth(160);
    refreshPresetList();
    auto* recall = new QPushButton("Recall", this);
    auto* save   = new QPushButton("Save as…", this);
    auto* del    = new QPushButton("Delete", this);
    auto* fact   = new QPushButton("K1EL factory defaults", this);
    row->addWidget(new QLabel("Preset", this));
    row->addWidget(presets_, 1);
    row->addWidget(recall);
    row->addWidget(save);
    row->addWidget(del);
    row->addStretch(1);
    row->addWidget(fact);
    lay->addLayout(row);

    connect(recall, &QPushButton::clicked, this, [this] {
        if (!presets_->currentText().isEmpty())
            applyPreset(presets_->currentText());
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString n = QInputDialog::getText(this, "Save keyer preset",
                                                "Name (e.g. ragchew, contest):")
                              .trimmed();
        if (!n.isEmpty()) { savePreset(n); refreshPresetList();
                            presets_->setCurrentText(n); }
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const QString n = presets_->currentText();
        if (n.isEmpty()) return;
        QSettings s;
        s.remove("winkeyer/preset/" + n);
        QStringList all = s.value("winkeyer/presets").toStringList();
        all.removeAll(n);
        s.setValue("winkeyer/presets", all);
        refreshPresetList();
    });
    connect(fact, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Restore factory defaults",
                "Set every control back to the K1EL factory values "
                "(weight 50, key comp 0, first extension 0, ratio 1:3, "
                "Farnsworth off, sidetone 800 Hz)?") == QMessageBox::Yes)
            restoreFactoryDefaults();
    });
}

void WinKeyerPanel::refreshPresetList() {
    if (!presets_) return;
    const QString cur = presets_->currentText();
    presets_->clear();
    presets_->addItems(QSettings().value("winkeyer/presets").toStringList());
    if (!cur.isEmpty()) presets_->setCurrentText(cur);
}

void WinKeyerPanel::savePreset(const QString& name) {
    QSettings s;
    const QString k = "winkeyer/preset/" + name + "/";
    s.setValue(k + "wpm",         wpm_->value());
    s.setValue(k + "weight",      weight_->value());
    s.setValue(k + "keyComp",     keyComp_->value());
    s.setValue(k + "firstExt",    firstExt_->value());
    s.setValue(k + "ratio",       ratio_->value());
    s.setValue(k + "letterspace", letterspace_->value());
    s.setValue(k + "farnsworth",  farns_->value());
    s.setValue(k + "pttLead",     pttLead_->value());
    s.setValue(k + "pttTail",     pttTail_->value());
    s.setValue(k + "switchpoint", switchpoint_->value());
    s.setValue(k + "sidetone",    sidetone_->currentData().toInt());
    QStringList all = s.value("winkeyer/presets").toStringList();
    if (!all.contains(name)) { all << name; s.setValue("winkeyer/presets", all); }
}

void WinKeyerPanel::applyPreset(const QString& name) {
    QSettings s;
    const QString k = "winkeyer/preset/" + name + "/";
    if (!s.contains(k + "weight")) return;
    // Setting the sliders drives the same wiring a drag would, so each
    // value is written to the keyer and persisted exactly once.
    wpm_->setValue(s.value(k + "wpm", wpm_->value()).toInt());
    weight_->setValue(s.value(k + "weight", 50).toInt());
    keyComp_->setValue(s.value(k + "keyComp", 0).toInt());
    firstExt_->setValue(s.value(k + "firstExt", 0).toInt());
    ratio_->setValue(s.value(k + "ratio", 50).toInt());
    letterspace_->setValue(s.value(k + "letterspace", 0).toInt());
    farns_->setValue(s.value(k + "farnsworth", 0).toInt());
    pttLead_->setValue(s.value(k + "pttLead", 0).toInt());
    pttTail_->setValue(s.value(k + "pttTail", 0).toInt());
    switchpoint_->setValue(s.value(k + "switchpoint", 50).toInt());
    const int st = s.value(k + "sidetone", 5).toInt();
    sidetone_->setCurrentIndex(std::clamp(st, 1, 10) - 1);
}

// The documented factory set, manual p18.
void WinKeyerPanel::restoreFactoryDefaults() {
    weight_->setValue(50);
    keyComp_->setValue(0);
    firstExt_->setValue(0);
    ratio_->setValue(50);
    letterspace_->setValue(0);
    farns_->setValue(0);
    pttLead_->setValue(0);
    pttTail_->setValue(0);
    switchpoint_->setValue(50);
    sidetone_->setCurrentIndex(4);          // 800 Hz
}

} // namespace ttc
