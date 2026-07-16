// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QImage>
#include <QStringList>
#include <QVector>
#include <QRect>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ttc {

// User-adjustable display parameters (KE9NS/Flex-style), persisted by the
// owner and applied via setDisplaySettings. refDb is the top of the spectrum
// scale; refDb - rangeDb is the bottom (contrast). One shared scale drives
// the trace, the gradient fill and the waterfall so colors always agree.
struct DisplaySettings {
    float refDb     = -55.0f;   // top of scale, dB
    float rangeDb   =  70.0f;   // scale height, dB (contrast)
    int   palette   = 0;        // index into paletteNames()
    int   avgFrames = 4;        // spectrum EMA length; 1 = off
    int   wfSpeed   = 2;        // FFT frames per waterfall row; 1 = fastest
    bool  fillTrace = true;     // KE9NS-style gradient fill under the trace
    bool  peakHold  = false;    // slow-decay peak trace
    float split     = 0.42f;    // spectrum/waterfall divider position (0..1)
    int   background = 0;       // index into backgroundNames()
    int   mapDay     = 78;      // map day-side brightness, percent
    int   mapNight   = 18;      // map night-side brightness, percent
    bool  showGrid   = true;    // freq/dB gridlines on or off
    bool  showCall   = true;    // subtle callsign watermark in the spectrum
    bool  showSolar  = true;    // space-weather panel in the spectrum corner
    bool  showRose   = true;    // compass rose (azimuthal world + bearing)
    bool  showBandPlan = true;  // US band-plan tints in the freq-scale strip
    int   traceColor = 0;       // 0 soft, 1 white, 2 green, 3 yellow, 4 cyan
    bool  bigVfo     = false;   // ~25 % larger VFO digits (applied by owner)
    bool  showClock  = true;    // UTC/local clock (radio panel bottom)
    bool  cwZap      = true;    // CW click snaps to the carrier peak (owner)
    bool  showWfTime = true;    // UTC timestamps down the waterfall edge
};

// A user-pinned frequency marker (SDR-Console/Thetis style): a labeled
// line the operator drops on a spot worth remembering — a sked, a net,
// W1AW code practice. Shift+right-click toggles one; the owner persists
// the set.
struct FreqMarker {
    qint64  hz = 0;
    QString label;              // empty = the frequency itself is shown
};

// A DX-cluster spot to mark on the display (absolute frequency). atSecs
// (epoch) lets fresh spots draw brighter than ones about to expire. kind
// picks the label color ('D' DX yellow, 'P' POTA green, 'F' FT8 cyan,
// 'S' own CW skimmer violet); tag is extra text after the call (the POTA
// park reference, or the skimmer's WPM estimate).
struct SpotLabel {
    QString call;
    qint64  hz = 0;
    qint64  atSecs = 0;
    char    kind = 'D';
    QString tag;
    double  lat = 999.0;        // station location (999 = unknown) — clicking
    double  lon = 999.0;        // the label swings the compass rose to it
    // Logbook status (set by the owner from cqrlog data): 'N' never worked,
    // 'B' worked but not this band, 'W' worked this band, 'C' confirmed,
    // '?' unknown — '?' keeps the plain kind color.
    char    status = '?';
    // DX-watch hit: the label draws with an orange alert ring.
    bool    alert = false;
};

// Spectrum + waterfall panadapter display. The flagship interactions live here:
//   * click a signal        -> tuneRequested(offsetHz)
//   * drag a passband edge   -> passbandChanged(loHz, hiHz)  (=> radio filter)
//   * mouse wheel            -> zoom the displayed span (drag-to-filter needs this)
// Data model: setSpectrum receives the FULL captured span (fftshifted dB bins);
// the view renders a zoomable sub-window centered on the dial. All mouse math
// goes through hzToX/xToHz, so zoom applies to every interaction automatically.
// The waterfall keeps raw dB rows (not pixels) so zoom/resize/palette changes
// re-render history crisply — each display row maps pixel columns to bins with
// a max-pick, never smooth image scaling.
class PanadapterWidget : public QWidget {
    Q_OBJECT
public:
    explicit PanadapterWidget(QWidget* parent = nullptr);

    void setSpanHz(int spanHz);                    // full captured span
    void setViewSpanHz(int spanHz);                // zoom (slider); no signal re-emit
    // PowerSDR-style offset LO (if_freq): the SDR captures offset ABOVE the
    // dial so the zero-IF DC artifact never sits on the tuned frequency. The
    // view stays dial-centered; this tells it where the dial lives within
    // the capture. Also shrinks the maximum symmetric view span.
    void setDialOffsetHz(int hz);
    int  viewSpanHz() const { return viewSpanHz_; }
    int  fullSpanHz() const { return fullSpanHz_; }
    int  maxViewSpanHz() const { return fullSpanHz_ - 2 * std::abs(dialOffsetHz_); }
    int  minViewSpanHz() const;
    void setPassband(int loHz, int hiHz);          // offsets from center, in Hz
    // Which passband edge a pure-bandwidth change pins in place. On the Orion
    // in SSB, bandwidth grows away from the carrier: USB anchors the low
    // (zero-beat) edge, LSB the high edge; CW/AM/FM widen symmetrically.
    // Drives the plain-edge drag preview so it matches what the radio does.
    void setBwAnchor(int a);                       // -1 lock lo, +1 lock hi, 0 sym
    // Manual-notch marker, in display (RF-offset) space; caller maps the
    // radio's audio-Hz notch through the mode sideband. Ignored mid-drag.
    // peak = SAF flavor (green, passes the band instead of rejecting it).
    void setNotch(bool on, int rfOffsetHz, int widthHz, bool peak = false);
    void setSpectrum(const std::vector<float>& magsDb);
    void setCenterHz(uint64_t hz);                 // dial freq, for grid labels
    void setSpots(const QVector<SpotLabel>& s);    // empty = feature off
    void setMarkers(const QVector<FreqMarker>& m); // pinned frequency lines
    void setCallsign(const QString& call);         // watermark text (ds_.showCall)
    // Space weather for the sun marker (on map backdrops) + the green
    // corner panel (ds_.showSolar). Values < 0 / empty = unknown, not drawn.
    void setSolarInfo(int sfi, int aIdx, double kIdx, int ssn,
                      const QString& xray);
    // Station location (from the grid square) — centers the compass rose.
    void setQth(double latDeg, double lonDeg);
    // Swing the rose to a target (spot clicks use this internally; public
    // for future rotor/logging integration).
    void pointRoseAt(double lat, double lon, const QString& label);
    // Rotor integration: the antenna's actual heading, drawn as a second
    // (cyan) needle on the rose. -1 = no rotor / unknown, needle hidden.
    void setRotorAz(double azDeg);
    // VFO B: drawn like the main VFO (passband tint + center line + flag)
    // when its dial falls inside the view. role = 'T' (B transmits — red),
    // 'R' (B is the main RX — green) or 'N' (parked sub dial — blue); A's
    // flag takes the complementary color, all matching the routing buttons.
    // loHz/hiHz are B's filter edges as RF offsets from B's dial. B is
    // grabbable (vfoBDragged, streamed) and RIGHT-CLICK anywhere drops B on
    // that frequency (pileup TX placement). The A dial line is grabbable too
    // (vfoADragged with an absolute target).
    void setVfoB(uint64_t hz, char role, int loHz, int hiHz);
    void setVfoBVisible(bool on);                  // hide B entirely when unused
    // Console-side tune locks (mirror the Orion's *AL/*BL, which only freeze
    // the front panel): locked VFOs stop being drag/click-tune targets here.
    // Filter-edge gestures stay live — the lock is about frequency.
    void setVfoLocks(bool a, bool b);
    // Regulatory zones (channelized allocations, e.g. US 60 m): green boxes
    // with labels drawn whenever they fall inside the view.
    struct BandZone { qint64 loHz, hiHz; QString label; };
    void setBandZones(const QVector<BandZone>& zones);

    void setDisplaySettings(const DisplaySettings& s);
    const DisplaySettings& displaySettings() const { return ds_; }
    static QStringList paletteNames();
    static QStringList backgroundNames();

signals:
    // Click-to-tune (offset from center). exact = Shift held: tune the raw
    // clicked frequency, bypassing owner-side snapping (CW zap).
    void tuneRequested(int offsetHz, bool exact = false);
    // A spot label was clicked: call + source kind ('D'/'P'/'F') + tag (park
    // ref for POTA). Prefills the LOG panel.
    void spotClicked(const QString& call, QChar kind, const QString& tag);
    // Ctrl+click on a spot callsign: turn the antenna to it too (bearing
    // from the spot's cty/POTA placement; -1 = location unknown).
    void spotTurnRequested(double bearingDeg, const QString& call);
    // Shift+right-click: add a marker here / remove the one already here.
    // The owner keeps the list (label prompt, persistence) and pushes it
    // back via setMarkers.
    void markerToggleRequested(qint64 absHz);
    // The rose pointer moved (spot click or manual heading; -1 = cleared).
    // The ROT panel uses this as its turn target.
    void roseBearingChanged(double bearingDeg, const QString& label);
    void tuneStepRequested(int steps, bool fine);  // wheel tune A; fine = Shift held
    void vfoBStepRequested(int steps, bool fine);  // wheel tune B (B touched last)
    void passbandEditBegan(int loHz, int hiHz);    // edge grabbed: anchor radio state
    void passbandChanged(int loHz, int hiHz);      // drag-to-filter
    void notchDragged(int rfOffsetHz);             // notch marker slid (streamed)
    void notchWidthAdjustRequested(int steps);     // wheel over the notch marker
    void pbtZeroRequested();                       // double-click an A passband edge
    void vfoBPbtZeroRequested();                   // double-click a B passband edge
    void viewSpanChanged(int spanHz);              // Ctrl+wheel zoom
    void vfoADragged(uint64_t hz);                 // A dial line slid (absolute target)
    void vfoBDragged(int rfOffsetHz);              // B slid (streamed, offset from center)
    void vfoBTuneRequested(int rfOffsetHz);        // right-click: put B here
    void vfoBEditBegan(int loHz, int hiHz);        // B edge grabbed (anchor sub state)
    void vfoBPassbandChanged(int loHz, int hiHz);  // B filter drag (offsets from B dial)
    // In-widget edits (dB-axis drag, range wheel, divider drag) changed ds_;
    // owner persists and syncs the DISPLAY panel.
    void displaySettingsEdited(const DisplaySettings& s);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    int    hzToX(int hz) const;
    int    xToHz(int x) const;
    int    spectrumHeight() const;                 // top area; waterfall below
    QRgb   mapColor(float t) const;                // t in [0,1], current palette
    void   pushWaterfallRow(const std::vector<float>& db);
    void   renderWfLine(const float* src, QRgb* dst, int w, int binLo, int binHi) const;
    void   ensureWaterfallImage(int w, int h, int binLo, int binHi);
    void   drawBandZones(QPainter& p, int hSpec);  // regulatory zone boxes
    void   drawFreqGrid(QPainter& p, int hSpec);   // gridlines, spectrum area only
    void   drawScaleBand(QPainter& p, int hSpec);  // freq scale strip on the divider
    void   drawDbScale(QPainter& p, int hSpec);    // horizontal dB lines + labels
    void   drawSpots(QPainter& p, int hSpec);      // cluster spot lines + callsigns
    void   drawMarkers(QPainter& p, int hSpec);    // pinned frequency lines
    void   drawWfTimes(QPainter& p, int wfTop);    // UTC labels down the waterfall

    bool   overNotch(int x) const;                 // cursor within the grab zone
    bool   overVfoB(int x) const;                  // over B's line or passband
    bool   inScaleBand(int y) const;               // over the draggable divider strip
    bool   inDbAxis(int x, int y) const;           // over the draggable dB scale

    // Drag-gesture bookkeeping (symmetric-BW and body-drag PBT).
    int dragStartX_  = 0;
    int dragStartY_  = 0;
    int dragStartLo_ = 0, dragStartHi_ = 0;
    int dragStartBLo_ = 0, dragStartBHi_ = 0;      // B filter edges at grab
    float dragStartRef_   = 0.0f;                  // refDb at dB-axis grab
    float dragStartSplit_ = 0.42f;                 // split at divider grab

    int fullSpanHz_ = 250000;                      // what the SDR captures
    int viewSpanHz_ = 250000;                      // what we display (zoom)
    int dialOffsetHz_ = 0;                         // LO minus dial (offset tuning)
    uint64_t centerHz_ = 0;                        // 0 = unknown, labels skipped
    int pbLoHz_ = -1200;
    int pbHiHz_ = 1200;
    int bwAnchor_ = 0;                             // see setBwAnchor
    QVector<BandZone> zones_;                      // regulatory overlays
    bool notchOn_     = false;
    bool notchPeak_   = false;                     // SAF flavor (green marker)
    int  notchRfHz_   = 0;                         // marker center, RF offset
    int  notchWidthHz_ = 0;

    DisplaySettings ds_;
    float dbMin_ = -125.0f, dbMax_ = -55.0f;       // derived from ds_ (ref/range)

    std::vector<float> spectrum_;                  // latest raw frame, full span
    std::vector<float> avg_;                       // EMA of spectrum_ (the trace)
    std::vector<float> peaks_;                     // slow-decay peak-hold trace

    // Waterfall: ring buffer of raw dB rows + a pixel-resolution rendered image.
    // New rows render incrementally; zoom/resize/settings force a full rebuild.
    std::vector<float> wfHist_;                    // kWfHistRows x wfBins_, ring
    std::vector<qint64> wfTimes_;                  // epoch ms per row, same ring
    int  wfBins_ = 0, wfHead_ = 0, wfCount_ = 0;
    int  wfPending_ = 0;                           // rows added since last render
    std::vector<float> wfAccum_;                   // max-merge across wfSpeed frames
    int  wfAccumN_ = 0;
    QImage wfImg_;                                 // rendered, 1 row = 1 pixel line
    int  lastBinLo_ = -1, lastBinHi_ = -1;         // geometry key for wfImg_
    bool wfDirty_ = true;                          // settings changed: full re-render
    QImage fillImg_;                               // level-colored fill (reused buffer)

    // DX-cluster spots: markers + callsigns in the spectrum area; hit rects
    struct SpotHit { QRect rect; qint64 hz; QString call; double lat, lon;
                     QChar kind; QString tag; };
    // (rebuilt each paint) make the labels click-to-tune targets.
    QVector<SpotLabel> spots_;
    QVector<SpotHit>   spotHits_;                  // label rect -> freq + call
    QVector<FreqMarker> markers_;                  // pinned frequency lines

    QString callsign_;                             // watermark (empty = off)

    uint64_t vfoBHz_ = 0;                          // 0 = marker off
    bool     vfoBVisible_ = true;                  // user toggle (VIEW button)
    bool     lockA_ = false, lockB_ = false;       // tune locks (see setVfoLocks)
    char     vfoBRole_ = 'N';                      // 'T' tx / 'R' main rx / 'N' parked
    int      vfoBLo_ = -1250, vfoBHi_ = 1250;      // B filter edges (offsets from B)
    qint64   dragStartBOff_ = 0;                   // B offset at grab time
    uint64_t dragStartCenter_ = 0;                 // A dial at grab time
    bool     bodyPbt_ = false;                     // Ctrl held at body grab -> PBT slide
    char     wheelVfo_ = 'A';                      // wheel follows the last-tuned VFO

    // Spectrum-area background (KE9NS-style): cached render, rebuilt when the
    // mode/size changes — or each minute for the world map's moving grayline.
    const QImage& backgroundImage(int w, int h);
    QImage bgCache_;
    int    bgMode_ = -1, bgW_ = 0, bgH_ = 0;
    qint64 bgMinute_ = -1;
    int    bgDay_ = -1, bgNight_ = -1;             // brightness cache keys
    bool   bgSun_ = false;                         // sun marker cache key
    QImage  mapSrc_;                               // decoded basemap
    QString mapSrcKey_;                            // resource path / custom file
    // Space weather (fed by MainWindow's SolarClient; -1/empty = unknown).
    int     solSfi_ = -1, solA_ = -1, solSsn_ = -1;
    double  solK_ = -1.0;
    QString solXray_;
    void drawSolarPanel(QPainter& p, int hSpec);   // green corner readout

    // Compass rose: azimuthal-equidistant world disc centered on the QTH,
    // bottom-left of the spectrum. Clicking a located spot label points it;
    // clicking inside the rose sets a manual heading, right-click clears.
    void drawCompassRose(QPainter& p, int hSpec);
    bool overRose(int x, int y) const;             // inside the disc?
    static constexpr int kRoseR = 64;
    double  qthLat_ = 42.5, qthLon_ = -83.0;       // EN82 until the grid is set
    double  roseBearing_ = -1.0;                   // pointer heading (-1 = none)
    double  roseDistKm_  = -1.0;
    double  rotorAz_     = -1.0;                   // antenna heading (-1 = off)
    QString roseLabel_;
    QImage  roseCache_;                            // projected disc, cached
    qint64  roseMinute_ = -1;                      // grayline advances
    QString roseKey_;                              // size+QTH cache key

    enum class Drag {
        None,
        LoEdge, HiEdge,     // one-edge drag: hi/lo-cut semantics (bw + pbt)
        SymEdge,            // Shift+edge: symmetric width, pure bw, pbt kept
        BodyPending, Body,  // press inside passband; becomes a pure-pbt slide
        Notch,
        VfoA,               // slide the A dial line (drag-to-tune)
        VfoB,               // slide VFO B (split TX placement)
        BLoEdge, BHiEdge,   // VFO B filter edges: same gestures as A's
        BSymEdge,           //   (drives the sub RX filter)
        Divider,            // drag the freq-scale band: move the wf split
        DbAxis,             // drag the left dB scale: shift ref level
    } drag_ = Drag::None;
};

} // namespace ttc
