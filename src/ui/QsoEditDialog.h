// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>

#include "log/LogDb.h"

class QLineEdit;

namespace ttc {

class CtyLookup;

// Edit one logged QSO (Logbook window: Edit… / double-click a row,
// 2026-11-07 — "I entered a call wrong and there is no edit"). Every
// field of the record on one form; a changed call refreshes country and
// zones from cty.dat, a changed frequency refreshes the band. Save hands
// back the corrected record; the owner writes it and re-pushes it.
class QsoEditDialog : public QDialog {
    Q_OBJECT
public:
    QsoEditDialog(const Qso& q, const CtyLookup* cty, QWidget* parent = nullptr);
    Qso result() const;                  // valid after accept()

private:
    void onCallEdited();
    void onFreqEdited();

    const CtyLookup* cty_;
    Qso orig_;
    QLineEdit *call_, *date_, *time_, *band_, *freq_, *mode_, *rstS_, *rstR_,
              *name_, *handle_, *qth_, *grid_, *country_, *pota_, *comment_;
    int cqz_ = 0, ituz_ = 0;
};

} // namespace ttc
