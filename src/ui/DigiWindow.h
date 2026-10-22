// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QCheckBox;
class QSpinBox;

namespace ttc {

class FldigiClient;

// fldigi companion window: live modem/carrier readout, the decoded RX
// text stream, and the click-to-carrier switch. fldigi keeps doing the
// modem work; the console adds the panadapter as its pointing device —
// with "clicks set carrier" on and the radio in DIG, clicking inside the
// passband moves fldigi's audio cursor instead of retuning the radio.
class DigiWindow : public QDialog {
    Q_OBJECT
public:
    explicit DigiWindow(FldigiClient* client, QWidget* parent = nullptr);

    bool trackClicks() const;

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    FldigiClient* fl_;
    QLabel* status_ = nullptr;
    QPlainTextEdit* rx_ = nullptr;
    QCheckBox* track_ = nullptr;
    QSpinBox* carrier_ = nullptr;
};

} // namespace ttc
