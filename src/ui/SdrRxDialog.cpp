// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/SdrRxDialog.h"
#include "ui/VoiceEqDialog.h"   // VoiceEqCurve

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ttc {

namespace {
// The console's dialog scheme; group headings carry their own colour
// (unstyled they take the desktop theme's — unreadable on Cinnamon).
const char* kStyle =
    "QDialog { background: #141b24; }"
    "QLabel { color: #8fa3b8; font-size: 12px; }"
    "QCheckBox { color: #c8d4e0; }"
    "QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #4a5e76;"
    " background: #1c2430; border-radius: 2px; }"
    "QCheckBox::indicator:hover { border-color: #6a86a6; }"
    "QCheckBox::indicator:checked { background: #e8b04a; border-color: #e8b04a; }"
    "QCheckBox::indicator:disabled { border-color: #2a3644; }"
    "QCheckBox::indicator:checked:disabled { background: #7a6230; border-color: #7a6230; }"
    "QGroupBox { color: #e8b04a; font-weight: bold; border: 1px solid #2a3644;"
    " margin-top: 12px; padding-top: 6px; }"
    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
    "QComboBox, QPushButton { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #2a3644; padding: 3px 8px; }"
    "QDoubleSpinBox { background: #1c2430; color: #c8d4e0; }"
    "QPushButton:hover, QComboBox:hover { border-color: #3d5068; }"
    "QComboBox::drop-down { border: none; width: 18px; }"
    "QComboBox::down-arrow { image: url(:/combo-down.png); width: 10px; height: 6px; }"
    "QComboBox::down-arrow:disabled { image: url(:/combo-down-off.png); }"
    "QDoubleSpinBox:disabled, QComboBox:disabled { color: #6f7f92; background: #18202a; }"
    "QPushButton:disabled { color: #66768a; border-color: #232c38; }"
    "QComboBox QAbstractItemView { background: #1c2430; color: #c8d4e0;"
    " border: 1px solid #3d5068; outline: 0;"
    " selection-background-color: #2b5c8a; selection-color: #ffffff; }";

// Listening shapes, tune-by-ear starting points (slots: 0 HP, 1 LS,
// 2-5 peaks, 6 HS, 7 LP). Each starts from flat, so a slot not named
// here is off.
struct RxEqPreset {
    const char* name;
    struct B { int slot; float hz, db, q; int slope; };
    std::vector<B> bands;
};
const std::vector<RxEqPreset>& rxEqPresets() {
    static const std::vector<RxEqPreset> p = {
        {"Flat", {}},
        // Thin the mud, lift the consonants: SSB in a pile of QRM.
        {"Voice clarity", {{0, 150, 0, 0.707f, 24}, {2, 300, -3, 1.0f, 0},
                           {4, 2200, 4, 1.0f, 0}}},
        // Long sessions: warm lows, the top hiss taken down.
        {"Mellow (less hiss)", {{1, 200, 2, 0.707f, 0}, {6, 2500, -6, 0.707f, 0}}},
        // Band noise above the voice cut hard.
        {"Hiss cut 2.6 kHz", {{0, 120, 0, 0.707f, 24}, {7, 2600, 0, 0.707f, 48}}},
        // CW: a soft audio window around the usual pitches.
        {"CW window", {{0, 300, 0, 0.707f, 24}, {7, 1000, 0, 0.707f, 24}}},
    };
    return p;
}
}  // namespace

// The receive meter: level into the gate (pre-volume — what the gate and
// compressor thresholds are set against, both marked), compressor gain
// reduction, and the output to the speakers with the final limiter's
// ceiling. Peaks hold and fall ~20 dB/s, as the LIVE mic meter does.
class SdrRxMeter : public QWidget {
public:
    explicit SdrRxMeter(QWidget* parent) : QWidget(parent) { setMinimumHeight(78); }
    QSize sizeHint() const override { return {440, 78}; }
    void setLevels(const SdrRx::Levels& l, bool gateOn, float gateThr,
                   bool compOn, float compThr) {
        const auto fall = [](float& shown, float v) { shown = std::max(v, shown - 1.f); };
        fall(in_, l.inDb);
        fall(out_, l.outDb);
        comp_ = std::min(l.compGrDb, std::min(0.f, comp_ + 1.f));   // GR falls back up
        lim_ = l.limGrDb;
        gateOpen_ = l.gateOpen;
        gateOn_ = gateOn; gateThr_ = gateThr; compOn_ = compOn; compThr_ = compThr;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(11, 16, 22));
        QFont f = p.font();
        f.setPixelSize(10);
        p.setFont(f);
        const QRectF bars = QRectF(rect()).adjusted(52, 4, -60, -4);
        const double h = (bars.height() - 8) / 3;
        const auto xOf = [&](float db) {          // -60 .. 0 dBFS
            return bars.left() + bars.width() * (std::clamp(db, -60.f, 0.f) + 60.f) / 60.f;
        };
        const auto row = [&](int i, const QString& name, float db, QColor c,
                             const QString& val) {
            const QRectF r(bars.left(), bars.top() + i * (h + 4), bars.width(), h);
            p.fillRect(r, QColor(24, 32, 42));
            p.fillRect(QRectF(r.left(), r.top(), xOf(db) - r.left(), r.height()), c);
            p.setPen(QColor(143, 163, 184));
            p.drawText(QRectF(0, r.top(), 48, r.height()), Qt::AlignRight | Qt::AlignVCenter, name);
            p.setPen(QColor(200, 212, 224));
            p.drawText(QRectF(r.right() + 4, r.top(), 56, r.height()),
                       Qt::AlignLeft | Qt::AlignVCenter, val);
            return r;
        };
        const QRectF ri = row(0, "In", in_, gateOn_ && !gateOpen_ ? QColor(90, 100, 110)
                                                           : QColor(80, 200, 120),
                              QString("%1 dB").arg(in_, 0, 'f', 0));
        const auto mark = [&](const QRectF& r, float db, QColor c) {
            p.setPen(QPen(c, 2));
            p.drawLine(QPointF(xOf(db), r.top() - 1), QPointF(xOf(db), r.bottom() + 1));
        };
        if (gateOn_) mark(ri, gateThr_, QColor(232, 176, 74));    // amber: gate
        if (compOn_) mark(ri, compThr_, QColor(90, 160, 240));    // blue: comp
        // GR drawn from the right: 0..-20 dB over the bar.
        const QRectF rc(bars.left(), bars.top() + (h + 4), bars.width(), h);
        p.fillRect(rc, QColor(24, 32, 42));
        const double w = bars.width() * std::clamp(-comp_, 0.f, 20.f) / 20.f;
        p.fillRect(QRectF(rc.right() - w, rc.top(), w, rc.height()), QColor(90, 160, 240));
        p.setPen(QColor(143, 163, 184));
        p.drawText(QRectF(0, rc.top(), 48, rc.height()), Qt::AlignRight | Qt::AlignVCenter, "Comp");
        p.setPen(QColor(200, 212, 224));
        p.drawText(QRectF(rc.right() + 4, rc.top(), 56, rc.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   compOn_ ? QString("%1 dB").arg(comp_, 0, 'f', 1) : QStringLiteral("off"));
        const QRectF ro = row(2, "Out", out_, lim_ < -0.5f ? QColor(230, 80, 70)
                                                     : QColor(80, 200, 120),
                              lim_ < -0.5f ? QString("lim %1").arg(lim_, 0, 'f', 0)
                                           : QString("%1 dB").arg(out_, 0, 'f', 0));
        mark(ro, SdrRx::kCeilingDb, QColor(230, 80, 70));
        if (gateOn_) {                             // the gate's state, in words
            // On its own dark tag: over the green bar it vanished.
            const QRectF tag(ri.left() + 3, ri.top() + 2, 70, ri.height() - 4);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(11, 16, 22, 220));
            p.drawRoundedRect(tag, 3, 3);
            p.setPen(gateOpen_ ? QColor(80, 200, 120) : QColor(232, 176, 74));
            p.drawText(tag, Qt::AlignCenter, gateOpen_ ? "gate open" : "gate closed");
        }
    }

private:
    float in_ = -120.f, out_ = -120.f, comp_ = 0.f, lim_ = 0.f;
    bool gateOpen_ = true, gateOn_ = false, compOn_ = false;
    float gateThr_ = -35.f, compThr_ = -24.f;
};

void SdrRxDialog::loadEqPreset(int idx) {
    const auto& all = rxEqPresets();
    if (idx < 0 || idx >= int(all.size())) return;
    eq_ = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
    for (auto& b : eq_.bands) b.enabled = false;
    for (const auto& b : all[size_t(idx)].bands) {
        auto& d = eq_.bands[size_t(b.slot)];
        d.freqHz = b.hz;
        d.gainDb = b.db;
        d.q = b.q;
        if (b.slope) d.slopeDbPerOct = b.slope;
        d.enabled = true;
    }
    eq_.enabled = idx > 0;
    {
        const QSignalBlocker blk(eqOn_);
        eqOn_->setChecked(eq_.enabled);
    }
    curve_->setSettings(eq_);
    emitChanged();
}

SdrRxDialog::SdrRxDialog(bool on, const SdrRx::Settings& s, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("SDR receive audio");
    setStyleSheet(kStyle);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    on_ = new QCheckBox("SDR receive audio on (PC speakers / headset)", this);
    on_->setStyleSheet("QCheckBox { color: #e8b04a; font-weight: bold; }");
    lay->addWidget(on_);
    auto* note = new QLabel("The RSP2's IQ through WDSP, Thetis's receiver. Follows the\n"
                            "radio's mode, filter and CW pitch; silent while transmitting.\n"
                            "It runs ~50-100 ms behind the Orion — turn the radio down.",
                            this);
    lay->addWidget(note);

    auto* audio = new QGroupBox("AUDIO", this);
    auto* ag = new QGridLayout(audio);
    ag->addWidget(new QLabel("Volume", audio), 0, 0);
    vol_ = new QSlider(Qt::Horizontal, audio);
    vol_->setRange(-40, 20);
    vol_->setMinimumWidth(200);
    ag->addWidget(vol_, 0, 1);
    volVal_ = new QLabel(audio);
    volVal_->setFixedWidth(48);
    ag->addWidget(volVal_, 0, 2);
    ag->addWidget(new QLabel("AGC", audio), 1, 0);
    agc_ = new QComboBox(audio);
    agc_->addItems({"Fixed", "Long", "Slow", "Medium", "Fast"});
    agc_->setToolTip("WDSP AGC (Thetis's): attack/hang/decay presets");
    ag->addWidget(agc_, 1, 1);
    lay->addWidget(audio);

    auto* noise = new QGroupBox("NOISE", this);
    auto* ng = new QGridLayout(noise);
    ng->addWidget(new QLabel("Noise blanker", noise), 0, 0);
    nb_ = new QComboBox(noise);
    nb_->addItems({"Off", "NB — impulse blanker", "NB2 — blank and fill"});
    nb_->setToolTip("On the IQ, before any filter (as Thetis runs them). NB gates\n"
                    "an impulse to zero; NB2 fills the gap from either side —\n"
                    "better when the pop lands on the signal. Never both.");
    ng->addWidget(nb_, 0, 1);
    ng->addWidget(new QLabel("Noise reduction", noise), 1, 0);
    nr_ = new QComboBox(noise);
    nr_->addItems({"Off", "NR — LMS (Thetis NR)", "NR2 — spectral (Thetis NR2)",
                   "NR3 — neural (WDSP 2.10)"});
    nr_->setToolTip("One at a time, as Thetis offers them. NR2 is the usual\n"
                    "choice on SSB; NR3 is the neural reducer (adds ~51 ms).");
    ng->addWidget(nr_, 1, 1);
    snb_ = new QCheckBox("SNB — spectral noise blanker", noise);
    snb_->setToolTip("Thetis SNB: removes impulse noise in the frequency domain");
    anf_ = new QCheckBox("ANF — automatic notch", noise);
    anf_->setToolTip("Removes steady carriers / heterodynes (not for CW!)");
    rnn_ = new QCheckBox("RNNoise (on the audio)", noise);
    rnn_->setToolTip("Xiph RNNoise after the demod, as AetherSDR offers it — voice only");
    ng->addWidget(snb_, 2, 0);                     // two columns: the window
    ng->addWidget(anf_, 2, 1);                     // must fit a 1080 screen
    ng->addWidget(rnn_, 3, 0);
    lay->addWidget(noise);

    auto* fdvNote = new QLabel("FreeDV (RADE): the <b>FreeDV</b> button in the MODE panel sets "
                               "this receiver up for it.", this);
    fdvNote->setTextFormat(Qt::RichText);
    lay->addWidget(fdvNote);

    // RX EQ: the voice keyer's editor on the receive audio — drag a
    // handle (left/right = frequency, up/down = gain, Shift-drag or the
    // wheel = Q); grabbing a band that is off turns it on.
    auto* eqBox = new QGroupBox("EQ", this);
    auto* eqLay = new QVBoxLayout(eqBox);
    auto* eqRow = new QHBoxLayout;
    eqOn_ = new QCheckBox("EQ on", eqBox);
    eqRow->addWidget(eqOn_);
    eqRow->addStretch(1);
    eqRow->addWidget(new QLabel("Preset", eqBox));
    eqPreset_ = new QComboBox(eqBox);
    eqPreset_->addItem("Load…");
    for (const auto& p : rxEqPresets()) eqPreset_->addItem(p.name);
    eqPreset_->setToolTip("Starting points — then drag the curve to taste");
    eqRow->addWidget(eqPreset_);
    eqLay->addLayout(eqRow);
    curve_ = new VoiceEqCurve(eqBox);
    curve_->setEdge(0.f, QString());
    curve_->setFixedHeight(140);                   // its 200 px hint is the VK editor's
    curve_->setToolTip("Drag a dot: left/right = frequency, up/down = gain;\n"
                       "Shift-drag or the wheel = Q (width). HP/LP move in frequency.");
    eqLay->addWidget(curve_);
    lay->addWidget(eqBox);

    // RX dynamics, AetherSDR's receive order after the noise tools:
    // gate -> EQ -> compressor -> volume/BOOST -> final limiter.
    auto* dyn = new QGroupBox("DYNAMICS", this);
    auto* dg = new QGridLayout(dyn);
    const auto spin = [dyn](double lo, double hi, double step, int dec, const QString& suf) {
        auto* b = new QDoubleSpinBox(dyn);
        b->setRange(lo, hi);
        b->setSingleStep(step);
        b->setDecimals(dec);
        b->setSuffix(suf);
        return b;
    };
    gateOn_ = new QCheckBox("Gate", dyn);
    gateOn_->setToolTip("Turns the band hiss down between overs. Set the threshold\n"
                        "(amber mark on the In bar) just above the noise between words.");
    gateThr_ = spin(-70, -10, 1, 0, " dB");
    gateThr_->setToolTip("Opens above this level (the amber mark)");
    gateFloor_ = spin(-40, 0, 1, 0, " dB");
    gateFloor_->setToolTip("Depth: how far it turns the hiss down when closed");
    dg->addWidget(gateOn_, 0, 0);
    dg->addWidget(new QLabel("threshold", dyn), 0, 1);
    dg->addWidget(gateThr_, 0, 2);
    dg->addWidget(new QLabel("depth", dyn), 0, 3);
    dg->addWidget(gateFloor_, 0, 4);
    compOn_ = new QCheckBox("Compressor", dyn);
    compOn_->setToolTip("Evens out fading and loud/weak stations. Gentle on receive:\n"
                        "1.5-2:1 (AetherSDR's advice); threshold = the blue mark.");
    compThr_ = spin(-50, 0, 1, 0, " dB");
    compRatio_ = spin(1.0, 6.0, 0.1, 1, ":1");
    compMakeup_ = spin(0, 18, 0.5, 1, " dB");
    compMakeup_->setToolTip("Make-up gain after compression");
    dg->addWidget(compOn_, 1, 0);
    dg->addWidget(new QLabel("threshold", dyn), 1, 1);
    dg->addWidget(compThr_, 1, 2);
    dg->addWidget(new QLabel("ratio", dyn), 1, 3);
    dg->addWidget(compRatio_, 1, 4);
    dg->addWidget(new QLabel("make-up", dyn), 1, 5);
    dg->addWidget(compMakeup_, 1, 6);
    mute_ = new QCheckBox("MUTE", dyn);
    mute_->setToolTip("Silence the SDR audio (the receiver keeps running)");
    boost_ = new QCheckBox(QString("BOOST +%1 dB").arg(SdrRx::kBoostDb, 0, 'f', 0), dyn);
    boost_->setToolTip("Extra loudness into the final limiter (-1 dBFS, always on)");
    auto* mb = new QHBoxLayout;
    mb->addWidget(mute_);
    mb->addWidget(boost_);
    mb->addStretch(1);
    dg->addLayout(mb, 2, 0, 1, 7);
    meter_ = new SdrRxMeter(dyn);
    dg->addWidget(meter_, 3, 0, 1, 7);
    lay->addWidget(dyn);

    info_ = new QLabel(this);
    info_->setStyleSheet("QLabel { color: #c8d4e0; font-family: monospace; }");
    lay->addWidget(info_);
    auto* bRow = new QHBoxLayout;
    bRow->addStretch(1);
    auto* close = new QPushButton("Close", this);
    bRow->addWidget(close);
    lay->addLayout(bRow);

    loading_ = true;
    on_->setChecked(on);
    vol_->setValue(int(std::lround(s.volumeDb)));
    volVal_->setText(QString("%1 dB").arg(vol_->value()));
    agc_->setCurrentIndex(int(s.agc));
    nb_->setCurrentIndex(int(s.nb));
    nr_->setCurrentIndex(int(s.nr));
    snb_->setChecked(s.snb);
    anf_->setChecked(s.anf);
    rnn_->setChecked(s.rnnoise);
    eq_ = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
    eq_.enabled = s.eqOn;
    for (size_t i = 0; i < s.eq.size(); ++i) eq_.bands[i] = s.eq[i];
    eqOn_->setChecked(s.eqOn);
    curve_->setSettings(eq_);
    gateOn_->setChecked(s.gateOn);
    gateThr_->setValue(s.gateThrDb);
    gateFloor_->setValue(s.gateFloorDb);
    compOn_->setChecked(s.compOn);
    compThr_->setValue(s.compThrDb);
    compRatio_->setValue(s.compRatio);
    compMakeup_->setValue(s.compMakeupDb);
    mute_->setChecked(s.mute);
    boost_->setChecked(s.boost);
    loading_ = false;

    connect(on_, &QCheckBox::toggled, this, [this] { emitChanged(); });
    connect(vol_, &QSlider::valueChanged, this, [this](int v) {
        volVal_->setText(QString("%1 dB").arg(v));
        emitChanged();
    });
    for (QComboBox* c : {agc_, nb_, nr_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { emitChanged(); });
    for (QDoubleSpinBox* b : {gateThr_, gateFloor_, compThr_, compRatio_, compMakeup_})
        connect(b, &QDoubleSpinBox::valueChanged, this, [this] { emitChanged(); });
    for (QCheckBox* c : {gateOn_, compOn_, mute_, boost_})
        connect(c, &QCheckBox::toggled, this, [this] { emitChanged(); });
    for (QCheckBox* c : {snb_, anf_, rnn_})
        connect(c, &QCheckBox::toggled, this, [this] { emitChanged(); });
    connect(eqOn_, &QCheckBox::toggled, this, [this](bool v) {
        eq_.enabled = v;
        curve_->setSettings(eq_);
        emitChanged();
    });
    connect(eqPreset_, &QComboBox::activated, this, [this](int i) {
        if (i > 0) loadEqPreset(i - 1);
        const QSignalBlocker blk(eqPreset_);
        eqPreset_->setCurrentIndex(0);
    });
    curve_->onBandEdited = [this](int band, const ClientEq::BandParams& p) {
        eq_.bands[size_t(band)] = p;
        if (p.enabled && !eq_.enabled) {           // the curve turned it on
            eq_.enabled = true;
            const QSignalBlocker blk(eqOn_);
            eqOn_->setChecked(true);
        }
        emitChanged();
    };
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    auto* tick = new QTimer(this);
    tick->setInterval(250);
    connect(tick, &QTimer::timeout, this, [this] {
        if (status_) info_->setText(status_());
    });
    tick->start();
    auto* mtick = new QTimer(this);
    mtick->setInterval(50);
    connect(mtick, &QTimer::timeout, this, [this] {
        if (!levels_) return;
        meter_->setLevels(levels_(), gateOn_->isChecked(), float(gateThr_->value()),
                          compOn_->isChecked(), float(compThr_->value()));
    });
    mtick->start();
}

void SdrRxDialog::setVolumeMute(int db, bool mute) {
    const QSignalBlocker v(vol_), m(mute_);
    vol_->setValue(db);
    volVal_->setText(QString("%1 dB").arg(vol_->value()));
    mute_->setChecked(mute);
}

void SdrRxDialog::setOn(bool on) {
    const QSignalBlocker b(on_);
    on_->setChecked(on);
}

SdrRx::Settings SdrRxDialog::collect() const {
    SdrRx::Settings s;
    s.volumeDb = float(vol_->value());
    s.agc = SdrRx::Agc(agc_->currentIndex());
    s.nb = SdrRx::NoiseBlanker(nb_->currentIndex());
    s.nr = SdrRx::NoiseReduction(nr_->currentIndex());
    s.snb = snb_->isChecked();
    s.anf = anf_->isChecked();
    s.rnnoise = rnn_->isChecked();
    s.eqOn = eq_.enabled;
    s.gateOn = gateOn_->isChecked();
    s.gateThrDb = float(gateThr_->value());
    s.gateFloorDb = float(gateFloor_->value());
    s.compOn = compOn_->isChecked();
    s.compThrDb = float(compThr_->value());
    s.compRatio = float(compRatio_->value());
    s.compMakeupDb = float(compMakeup_->value());
    s.mute = mute_->isChecked();
    s.boost = boost_->isChecked();
    for (size_t i = 0; i < s.eq.size(); ++i) s.eq[i] = eq_.bands[i];
    return s;
}

void SdrRxDialog::emitChanged() {
    if (loading_) return;
    emit changed(on_->isChecked(), collect());
}

} // namespace ttc
