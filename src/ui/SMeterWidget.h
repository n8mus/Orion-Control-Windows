// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QElapsedTimer>
#include <utility>
#include <vector>

class QTimer;

namespace ttc {

// Multi-style S-meter / TX meter — the meter-suite ideas from the KE9NS
// PowerSDR and Thetis lineages (GPL), all faces drawn in QPainter.
// Click cycles the STYLE:
//   Analog   — the Orion 565's own meter: amber face, black lettering,
//              dual S/watts scales, red instant + blue peak needles
//   Edge     — edgewise meter: linear ivory window, vertical needle
//   LED      — segmented LED bargraph, green/amber/red (Thetis LED item)
//   X-needle — Orion RX face; TX becomes a Daiwa-style cross-needle
//              face: FWD and REF needles crossing, SWR readout
//   Eye      — magic-eye tube (Thetis MAGIC_EYE): the green shadow wedge
//              closes as the signal rises, EM80 style
//   Omni     — the Omni VII (588)'s on-screen meter: horizontal LCD
//              bargraph on the black TFT — white rail with drop ticks,
//              bar filling beneath, chunky readout, no printed numbers
// Right-click opens a menu: RX READING (Signal / Average / Peak-hold), RX
// SOURCE (Radio / SDR) and SDR calibration. The SDR source reads the
// panadapter's own passband power — computed outside the radio's AGC and RF
// gain chain entirely, so the S-meter stays honest with RF gain backed way
// down (the Omni VII's hardware meter pins at the RF-gain floor and no
// firmware fix exists; the Orion only reads right because its firmware was
// fixed). Needle/eye styles run ~30 fps ballistics (fast attack, slow decay)
// so the 2 Hz poll data moves like a real movement. TX flips every style to
// a power face (watts + SWR), always fed by the radio regardless of source.
// Radio readings are raw ?S units, converted to dB-relative-to-S9 via the
// hamlib TT565 V2 calibration table.
class SMeterWidget : public QWidget {
    Q_OBJECT
public:
    explicit SMeterWidget(QWidget* parent = nullptr);

    // Face lettering follows the connected radio ("ORION" / "OMNI-VII"):
    // the gold face's wordmark and the TFT style's screen caption.
    void setWordmark(const QString& mark);

    // Raw ?S units -> dB rel S9 (public: MainWindow converts the radio's
    // reading for SDR-source calibration).
    static double rawToDbS9(int raw);

    // Panel layout: the face grows with its box (drawn at the design size,
    // scaled, proportions kept). Off = today's fixed size per style.
    void setScalable(bool on);
    bool hasHeightForWidth() const override { return scalable_; }
    int heightForWidth(int w) const override;

signals:
    void calibrateRequested();            // "Calibrate SDR to radio" menu item
    // Every receive reading this meter takes, in dB rel S9, from whichever
    // source it is set to (radio or calibrated SDR) — so other faces read
    // what this one reads.
    void rxLevel(double dbS9);

public slots:
    void setRawLevel(int raw);            // raw @SRM units from the radio (RX)
    void setTxLevel(double fwdWatts, double refWatts, double swr);  // @STF (TX)
    void setSdrLevel(double dbS9);        // panadapter passband power, calibrated
    // External wattmeter (LP-100A / PowerMaster, ~10 Hz): the SmartMTR face's
    // TX source when fresh; a carrier on it also holds the TX face up.
    void setExtTx(double fwdWatts, double swr, double zOhm, double phaseDeg,
                  bool carrier, bool zValid, const QString& meterName);
    void useSdrSource();                  // switch RX source to SDR (post-cal)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(class QMouseEvent* e) override;

private:
    // The paint code's canvas: the design size when scaled, else the widget.
    int faceW() const { return scalable_ ? base_.width() : width(); }
    int faceH() const { return scalable_ ? base_.height() : height(); }
    bool scalable_ = false;
    QSize base_;
    // Omni appended last so stored meterStyle indices never shift again.
    // Append-only: stored meterStyle indices must never shift.
    enum Style { Analog = 0, Edge, Led, Cross, Eye, Omni, Tr7, SmartMtr, kStyleCount };
    enum RxMode { Sig = 0, Avg, Peak, kModeCount };
    enum Source { SrcRadio = 0, SrcSdr };

    void feedRx(double db);               // common RX reading pipeline
    void showMenu(const QPoint& globalPos);
    void drawSdrTag(class QPainter& p, double x, double y,
                    const QColor& c);     // "SDR" source tag on RX faces
    double displayDb() const;             // reading after the RX-mode filter
    double scaleFrac(double db) const;    // dB -> 0..1 along the meter scale
    void   applyStyle();                  // size + tooltip for current style
    void   paintAnalog(class QPainter& p);  // RX + TX faces (Orion amber)
    void   paintEdge(class QPainter& p);    // RX + TX faces (edgewise)
    void   paintLed(class QPainter& p);     // segmented LED bargraph
    void   paintCross(class QPainter& p);   // TX cross-needle (RX = analog)
    void   paintTr7(class QPainter& p);     // Drake TR-7 homage (blue, white ink)
    void   paintEye(class QPainter& p);     // magic-eye tube
    void   paintOmni(class QPainter& p);    // Omni VII TFT bargraph
    void   paintSmartMtr(class QPainter& p);  // AetherSDR SmartMTR bars
    void   smartTick(qint64 nowMs, double dtSec);   // feed + animate the bars

    // SmartMTR (ported from AetherSDR, GPL-3: SmartMtrWidget / SmartMtrConfig /
    // MeterSmoother; the port lives in SMeterSmartMtr.cpp, GPL-3.0-or-later
    // like the rest of the shipped binary): one bar per measurement in 0..1 of its scale, analog
    // ballistics (18 ms attack, 269 ms release) and min/max extremes markers
    // over a 3 s window that glide back at a constant slew.
    struct SmartBar {
        double disp = 0.0, target = 0.0;
        double maxShown = 0.0, minShown = 1.0;
        std::vector<std::pair<qint64, double>> win;   // (ms, fraction)
        void push(qint64 nowMs, double v);
        void tick(qint64 nowMs, double dtSec);
    };
    SmartBar barSig_, barPwr_, barSwr_;
    QElapsedTimer smartClock_;
    qint64 lastSmartMs_ = 0;
    double extW_ = 0.0, extSwr_ = 1.0, extZ_ = 0.0, extPhase_ = 0.0;
    bool   extZValid_ = false;
    QString extName_;
    QElapsedTimer sinceExt_;              // freshness of the external meter
    double pwrScaleW_ = 120.0;            // auto-ranged 120 / 600 / 1200 / 1800
    QElapsedTimer sinceBigW_;             // hold a raised scale a while
    bool   extFresh() const { return sinceExt_.isValid() && sinceExt_.elapsed() < 600; }
    double txWatts() const { return extFresh() ? extW_ : fwdW_; }
    double txSwr() const { return extFresh() ? extSwr_ : swr_; }
    QString sUnitsText(double db) const;  // "S7" / "S9+23"

    int  style_  = Analog;
    int  mode_   = Sig;
    int  source_ = SrcRadio;
    bool sdrSeen_ = false;                // an SDR reading has arrived (gates menu)
    QElapsedTimer sinceFeed_;             // time-based EMA (feed rate varies 2-20 Hz)
    QString wordmark_ = QStringLiteral("ORION");
    QTimer* anim_ = nullptr;              // needle ballistics tick
    double needleDb_ = -120.0;            // animated needle position (RX)
    double needleW_  = 0.0;               // animated needle position (TX)

    bool tx_ = false;                     // which face the meter shows
    double dbS9_    = -120.0;             // current level, dB relative to S9
    double emaDb_   = -120.0;             // Average mode reading
    bool   emaInit_ = false;
    double peakDb_  = -120.0;             // Peak mode / peak-hold marker
    QElapsedTimer sincePeak_;
    bool haveReading_ = false;
    double fwdW_ = 0.0, refW_ = 0.0, swr_ = 1.0;
    double peakW_ = 0.0;                  // TX peak hold (watts)
    QElapsedTimer sinceTxPeak_;
    QElapsedTimer sinceTx_;               // TX-face hang (CW keying gaps)
    static constexpr int kTxHangMs = 1500;
    double needleRef_ = 0.0;              // animated REF needle (cross style)
};

} // namespace ttc
