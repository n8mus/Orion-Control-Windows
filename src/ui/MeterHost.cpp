// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/MeterHost.h"

#include <QHBoxLayout>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <vector>

#include "radio/RadioController.h"
#include "ui/SMeterWidget.h"
// AetherSDR's (src/ui/aether), by path: their SMeterWidget.h shares our name.
#ifdef HAVE_AETHER_METERS
#include "ui/aether/CrossNeedleMeterApplet.h"
#include "ui/aether/LpMeterApplet.h"
#include "ui/aether/SMeterWidget.h"
#endif

namespace ttc {

namespace {
constexpr const char* kFaceBtn =
    "QToolButton { background: #1c2430; color: #8fa3b8; border: 1px solid #2a3644;"
    " border-radius: 3px; font-size: 10px; font-weight: bold; padding: 2px 6px; }"
    "QToolButton:hover { border-color: #4a5a6e; }"
    "QToolButton:checked { background: #2b5c8a; color: #ffffff; border-color: #3d7ab6; }";
// The Orion's S9 is -73 dBm (the S-meter convention their arc is drawn on).
constexpr double kS9Dbm = -73.0;
}  // namespace

MeterHost::MeterHost(SMeterWidget* own, RadioController* radio, const Options& opt,
                     QWidget* parent)
    : QWidget(parent), faceKey_(opt.faceKey) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    lay->addLayout(bar);
    stack_ = new QStackedWidget(this);
    lay->addWidget(stack_, 1);

    struct Face { const char* id; const char* label; QWidget* w; const char* tip; };
    std::vector<Face> faces;
    if (opt.console)
        faces.push_back({"console", "CONSOLE", own,
                         "This console's meter (click it to cycle its faces)"});
#ifdef HAVE_AETHER_METERS
    arc_ = new AetherSDR::SMeterWidget;
    if (opt.needle) needle_ = new AetherSDR::CrossNeedleMeterApplet;
    lp_ = new AetherSDR::LpMeterApplet;
    // Station power: 100 W radio, the amp's legal limit when it's in line.
    const bool amp = QSettings().value("amp/enabled", false).toBool();
    arc_->setPowerScale(amp ? 1500 : 100, amp);
    if (needle_) needle_->setPowerScale(amp ? 1500 : 100, amp);

    faces.push_back({"arc", "ARC", arc_,
                     "AetherSDR's S-meter: signal on receive, power/SWR on transmit"});
    if (needle_)
        faces.push_back({"needle", "NEEDLE", needle_, "AetherSDR's cross-needle meter: "
                                                      "forward/reflected power with SWR curves"});
    faces.push_back({"lp", "LP-100A", lp_, "AetherSDR's LP-100A panel: power, SWR, |Z|, phase, "
                                           "return loss (needs the wattmeter enabled in Setup)"});
    // Receive: whatever OUR meter reads — the radio's ?S through the
    // Hamlib table, or the calibrated SDR passband power when the meter's
    // source is SDR. Feeding the radio's raw ?S through the Hamlib table
    // here regardless read S-units high against the Orion's own meter and
    // ours (live-found 2026-10-22, RF gain full up; the console meter was
    // on the calibrated SDR source).
    connect(own, &SMeterWidget::rxLevel, this, &MeterHost::feedRx);
    connect(radio, &RadioController::txMeterReported, this, &MeterHost::feedRadioTx);
#else
    Q_UNUSED(radio);
#endif
    for (const auto& f : faces) {
        f.w->setProperty("ttcPolicy", QVariant::fromValue(f.w->sizePolicy()));
        stack_->addWidget(f.w);
        faces_.insert(f.id, f.w);
        auto* b = new QToolButton(this);
        b->setText(f.label);
        b->setToolTip(f.tip);
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(kFaceBtn);
        bar->addWidget(b);
        buttons_.insert(f.id, b);
        const QString id = f.id;
        connect(b, &QToolButton::clicked, this, [this, id] {
            QSettings().setValue(faceKey_, id);
            showFace(id);
        });
    }
    bar->addStretch(1);
    const QString firstFace = faces.empty() ? QString() : QString(faces.front().id);
    showFace(QSettings().value(faceKey_, firstFace).toString());
}

void MeterHost::showFace(const QString& id) {
    QWidget* w = faces_.value(id, nullptr);
    if (!w) w = faces_.value("console", stack_->widget(0));   // unknown/absent face
    // A QStackedWidget is as tall as its tallest page; hidden faces go
    // Ignored so the box is the height of the face on show.
    for (int i = 0; i < stack_->count(); ++i) {
        QWidget* page = stack_->widget(i);
        page->setSizePolicy(page == w ? page->property("ttcPolicy").value<QSizePolicy>()
                                      : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
    }
    stack_->setCurrentWidget(w);
    if (auto* b = buttons_.value(faces_.key(w))) b->setChecked(true);
}

#ifdef HAVE_AETHER_METERS
void MeterHost::setTx(bool on) {
    if (on == tx_) return;
    tx_ = on;
    arc_->setTransmitting(on);
    if (needle_) needle_->setTransmitting(on);
}

void MeterHost::feedRx(double dbS9) {
    // The radio answers the meter poll in RX format only while unkeyed.
    setTx(false);
    arc_->setLevel(float(dbS9 + kS9Dbm));
}

void MeterHost::feedRadioTx(double fwd, double ref, double swr) {
    setTx(true);
    if (extFresh()) return;          // the wattmeter is the better source
    arc_->setRadioTxMeters(float(fwd), float(fwd), float(swr));
    if (needle_) needle_->setTxPowers(float(fwd), float(ref));
}

void MeterHost::attachTxMeter(TxMeter* m) {
    if (!m) return;
    connect(m, &TxMeter::reading, this, &MeterHost::feedExt);
    connect(m, &TxMeter::aliveChanged, this, [this](bool up) {
        lp_->setConnected(up);
        lp_->setDataFlowing(up);
    });
    lp_->setSource(QStringLiteral("SERIAL"));
}

void MeterHost::feedExt(const TxMeter::Reading& r) {
    lp_->setConnected(true);
    lp_->setDataFlowing(true);
    AetherSDR::LpMeter::Reading lr;
    lr.powerW = r.watts;
    lr.zOhms = r.zOhm;
    lr.phaseDeg = std::abs(r.phaseDeg);
    lr.dBm = r.dbm;
    lr.swr = r.swr;
    lr.powerRange = r.range;
    lr.callsign = r.callsign;
    lr.coherent = !r.valid || r.zValid;
    lp_->setReading(lr);
    if (!r.valid) return;            // no carrier: the radio's poll owns TX state
    extSeen_.start();
    // Reflected: the PowerMaster reports it; the LP-100A only SWR — derive.
    double ref = r.refWatts;
    if (ref <= 0.0 && r.swr > 1.0) {
        const double g = (r.swr - 1.0) / (r.swr + 1.0);
        ref = r.watts * g * g;
    }
    arc_->setTxMeters(float(r.watts), float(r.swr));
    if (needle_) needle_->setTxPowers(float(r.watts), float(ref));
}

#else
// Without their meters (Qt < 6.7) the box holds our face only.
void MeterHost::setTx(bool) {}
void MeterHost::feedRx(double) {}
void MeterHost::feedRadioTx(double, double, double) {}
void MeterHost::attachTxMeter(TxMeter*) {}
void MeterHost::feedExt(const TxMeter::Reading&) {}
#endif

} // namespace ttc
