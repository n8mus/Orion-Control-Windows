// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <functional>

namespace ttc {

class SkimStft;
class SkimmerEngine;
class SkimViewCanvas;

// SKIM view: a CW-Skimmer-style waterfall of the band's CW segment.
// Frequency runs up the vertical axis, time flows left, and the STFT
// behind it is fast enough (~8 ms columns) that every station's dits and
// dahs are visible as keying in its trace — with the skimmer's decoded
// callsigns riding next to the traces, colored by worked-before status
// like everything else in the console. Click a trace (or its label) to
// tune there; the wheel zooms, dragging pans.
//
// The window owns nothing heavy: SkimStft (fed on the SDR thread) is
// gated by our visibility, and the labels come from the same
// SkimmerEngine the band map reads.
class SkimViewWindow : public QDialog {
    Q_OBJECT
public:
    // status: call+hz -> logbook code ('N'/'B'/'W'/'C'/'?'), same lambda
    // the band map gets. tuning: fills the live dial and LO offset.
    SkimViewWindow(SkimStft* stft, SkimmerEngine* eng,
                   std::function<QChar(const QString&, qint64)> status,
                   std::function<void(qint64&, int&)> tuning,
                   QWidget* parent = nullptr);

signals:
    void tuneTo(qint64 hz, const QString& call);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    SkimStft* stft_;
    SkimViewCanvas* canvas_ = nullptr;
};

} // namespace ttc
