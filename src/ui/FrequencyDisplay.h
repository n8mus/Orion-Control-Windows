// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QWidget>
#include <QColor>
#include <cstdint>

class QLineEdit;

namespace ttc {

// KE9NS-style frequency readout: bold ITALIC digits, big MHz/kHz + a
// smaller Hz group set off by a space, not a dot ("14.230 000"); leading
// zeros dropped. The TX VFO (setTxRole) reads white MHz/kHz with an
// accent-colored Hz group so the live TX frequency pops; an RX VFO stays
// one accent color throughout.
//   * wheel over a digit  -> step that decade (the whole dial is a tune knob)
//   * click top/bottom half of a digit -> +/- that decade
//   * double-click        -> inline type-in entry (MHz, kHz or Hz accepted)
// setFrequency() feeds it from the radio; user changes emit frequencyEdited().
// The caption row above the digits carries the VFO name, an optional badge
// chip ("SPLIT", red) and the right-aligned band label ("40m"). Digits can
// render ~25 % larger via setLargeDigits (DISPLAY option).
class FrequencyDisplay : public QWidget {
    Q_OBJECT
public:
    explicit FrequencyDisplay(const QString& caption = QString(),
                              const QColor& accent = QColor(255, 210, 60),
                              QWidget* parent = nullptr);

    void setFrequency(uint64_t hz);
    void setAccent(const QColor& c);        // digit color
    void setTxRole(bool tx);                // TX VFO = white/accent two-tone
    void setBandText(const QString& t);     // "40m", right end of caption row
    void setBadge(const QString& t);        // "SPLIT" chip; empty = none
    void setLargeDigits(bool on);           // Thetis-scale digits

signals:
    void frequencyEdited(uint64_t hz);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    int  cellW() const;                     // per-glyph cell, MHz/kHz (scaled)
    int  hzCellW() const;                   // narrower Hz-group cell
    int  cellWAt(int i) const;              // the one wide-vs-narrow rule
    int  dotW() const;
    int  cellX(int i) const;                // x of digit cell i (0..7)
    void applyGeometry();                   // size for the current scale
    int  digitAt(int x) const;              // 0 = 10 MHz digit ... 7 = 1 Hz; -1 = none
    void bump(int digit, int direction);
    void beginEdit();
    void finishEdit();

    uint64_t hz_ = 14200000;
    QString  caption_;
    QString  bandText_;
    QString  badge_;
    QColor   accent_;                       // MHz/kHz digit color
    bool     isTx_ = false;                 // set by setTxRole, never inferred
    double   scale_ = 1.0;                  // 1.25 when large digits are on
    int      digitTop_ = 0;                 // y offset of the digit row
    QLineEdit* edit_ = nullptr;
};

} // namespace ttc
