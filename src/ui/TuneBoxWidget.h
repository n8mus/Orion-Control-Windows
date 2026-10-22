// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QWidget>

namespace ttc {

// The tuner/amp card as a sidebar box (it used to sit in the
// waterfall's corner, under the contest logger's window — operator,
// 2026-10-18): what to set the 238C and the Titan III to here for the
// current antenna, read at a glance across the shack, labels dim and
// values bright. Left-click = clicked() (the entry form), right-click =
// menuRequested().
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

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return {120, sizeHint().height()}; }

signals:
    void clicked();
    void menuRequested(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    QFont cardFont() const;
    int rowCount() const;

    Box box_;
};

} // namespace ttc
