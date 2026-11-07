// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QStringList>
#include <QWidget>

class QComboBox;

namespace ttc {

// The tuner/amp card as a sidebar box (it used to sit in the
// waterfall's corner, under the contest logger's window — operator,
// 2026-10-18): what to set the 238C and the Titan III to here for the
// current antenna, read at a glance across the shack, labels dim and
// values bright. Left-click = clicked() (the entry form), right-click =
// menuRequested(). A dropdown above the card picks the antenna
// (2026-11-07): the ones with settings saved on this band first, the rest
// under a separator — the Moxon is 40 m only, the KT34 20/15/10, the
// vertical and the end-fed most of it, so the short list is the useful one.
class TuneBoxWidget : public QWidget {
    Q_OBJECT
public:
    struct Box {
        QString title;          // "40m DIPOLE · 40 m"; empty = outside the bands
        QString at;             // "@ 7.030, 5 kHz above"
        bool    exact = false;  // TX dial sits on the saved kHz
        bool    empty = true;   // nothing saved for this antenna on this band
        QString tuner, amp;     // value lines (no TUNER/AMP label)
        QString swr, note;      // optional bottom line
    };

    explicit TuneBoxWidget(QWidget* parent = nullptr);
    void setBox(const Box& b);
    // onBand: antennas with settings on this band (listed first); others:
    // the rest of the station's antennas; current: the one selected ("" =
    // unlabeled). Repopulates without emitting antennaChosen().
    void setAntennas(const QStringList& onBand, const QStringList& others,
                     const QString& current);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {120, sizeHint().height()}; }

signals:
    void clicked();
    void antennaChosen(const QString& antenna);   // "" = unlabeled
    void menuRequested(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    QFont cardFont() const;
    int rowCount() const;
    int comboBand() const;          // height taken by the dropdown + gap

    Box box_;
    QComboBox* combo_ = nullptr;
};

} // namespace ttc
