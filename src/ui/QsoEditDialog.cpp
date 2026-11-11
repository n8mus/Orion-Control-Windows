// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/QsoEditDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimeZone>
#include <QVBoxLayout>
#include <cmath>

#include "util/CtyLookup.h"
#include "util/LogbookIndex.h"

namespace ttc {

QsoEditDialog::QsoEditDialog(const Qso& q, const CtyLookup* cty,
                             QWidget* parent)
    : QDialog(parent), cty_(cty), orig_(q), cqz_(q.cqz), ituz_(q.ituz) {
    setWindowTitle("Edit QSO — " + q.call);
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 14px; }"
        "QLabel { color: #b8c8d8; font-size: 13px; }"
        "QLineEdit { background: #1c2430; color: #eef4e2; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 4px 8px; font-size: 14px; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px solid"
        " #3a4a5e; border-radius: 3px; padding: 6px 14px; font-size: 13px;"
        " font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }");
    auto* v = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    const auto edit = [this, form](const char* label, const QString& text,
                                   int maxW = 0) {
        auto* e = new QLineEdit(text, this);
        if (maxW) e->setMaximumWidth(maxW);
        form->addRow(QLatin1String(label), e);
        return e;
    };
    call_ = edit("CALL", q.call, 160);
    auto* when = new QHBoxLayout;
    date_ = new QLineEdit(q.tsUtc.toUTC().toString("yyyy-MM-dd"), this);
    date_->setMaximumWidth(120);
    time_ = new QLineEdit(q.tsUtc.toUTC().toString("HH:mm:ss"), this);
    time_->setMaximumWidth(100);
    when->addWidget(date_);
    when->addWidget(time_);
    when->addWidget(new QLabel("UTC", this));
    when->addStretch(1);
    form->addRow("DATE / TIME", when);
    auto* where = new QHBoxLayout;
    freq_ = new QLineEdit(q.freqHz > 0 ? QString::number(q.freqHz / 1e6, 'f', 3)
                                       : QString(), this);
    freq_->setMaximumWidth(100);
    band_ = new QLineEdit(q.band, this);
    band_->setMaximumWidth(70);
    mode_ = new QLineEdit(q.mode, this);
    mode_->setMaximumWidth(80);
    where->addWidget(new QLabel("MHz", this));
    where->addWidget(freq_);
    where->addWidget(new QLabel("BAND", this));
    where->addWidget(band_);
    where->addWidget(new QLabel("MODE", this));
    where->addWidget(mode_);
    where->addStretch(1);
    form->addRow("FREQ", where);
    auto* rst = new QHBoxLayout;
    rstS_ = new QLineEdit(q.rstS, this);
    rstS_->setMaximumWidth(70);
    rstR_ = new QLineEdit(q.rstR, this);
    rstR_->setMaximumWidth(70);
    rst->addWidget(new QLabel("SENT", this));
    rst->addWidget(rstS_);
    rst->addWidget(new QLabel("RCVD", this));
    rst->addWidget(rstR_);
    rst->addStretch(1);
    form->addRow("RST", rst);
    name_    = edit("NAME", q.name);
    handle_  = edit("HANDLE", q.handle, 160);
    qth_     = edit("QTH", q.qth);
    grid_    = edit("GRID", q.grid, 100);
    country_ = edit("COUNTRY", q.country);
    pota_    = edit("POTA", q.pota, 120);
    comment_ = edit("COMMENT", q.comment);
    v->addLayout(form);

    auto* note = new QLabel(
#ifdef Q_OS_WIN
        "Saving re-sends the corrected QSO to every online log that is on,\n"
        "and to HRD Logbook. QRZ replaces its copy; where the call, time,\n"
        "band or mode changed, the old record stays on the other services\n"
        "(and in HRD) until you remove it there.", this);
#else
        "Saving re-sends the corrected QSO to every online log that is on,\n"
        "and to cqrlog. QRZ replaces its copy; where the call, time, band\n"
        "or mode changed, the old record stays on the other services\n"
        "(and in cqrlog) until you remove it there.", this);
#endif
    note->setStyleSheet("QLabel { color: #7e8fa1; font-size: 12px; }");
    v->addWidget(note);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Save
                                    | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Save)->setText("Save && re-send");
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        const Qso r = result();
        if (r.call.isEmpty()) {
            QMessageBox::warning(this, "Edit QSO", "The call is empty.");
            return;
        }
        if (!r.tsUtc.isValid()) {
            QMessageBox::warning(this, "Edit QSO",
                                 "Date/time must be yyyy-MM-dd HH:mm:ss (UTC).");
            return;
        }
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);

    connect(call_, &QLineEdit::editingFinished, this,
            &QsoEditDialog::onCallEdited);
    connect(freq_, &QLineEdit::editingFinished, this,
            &QsoEditDialog::onFreqEdited);
    call_->setFocus();
    call_->selectAll();
}

// A corrected call is usually a different country: refill it, and the
// zones, from cty.dat the way the LOG window would for a fresh entry.
void QsoEditDialog::onCallEdited() {
    const QString c = call_->text().trimmed().toUpper();
    call_->setText(c);
    if (!cty_ || c == orig_.call) return;
    CtyInfo ci;
    if (cty_->info(c, ci)) {
        country_->setText(ci.country);
        cqz_ = ci.cq;
        ituz_ = ci.itu;
    }
}

void QsoEditDialog::onFreqEdited() {
    bool ok = false;
    const double mhz = freq_->text().trimmed().toDouble(&ok);
    if (!ok || mhz <= 0) return;
    band_->setText(LogbookIndex::bandForHz(qint64(std::llround(mhz * 1e6))));
}

Qso QsoEditDialog::result() const {
    Qso r = orig_;
    r.call = call_->text().trimmed().toUpper();
    r.tsUtc = QDateTime::fromString(date_->text().trimmed() + " "
                                    + time_->text().trimmed(),
                                    "yyyy-MM-dd HH:mm:ss");
    r.tsUtc.setTimeZone(QTimeZone::utc());
    bool ok = false;
    const double mhz = freq_->text().trimmed().toDouble(&ok);
    r.freqHz = ok && mhz > 0 ? qint64(std::llround(mhz * 1e6)) : 0;
    r.band = band_->text().trimmed().toUpper();
    r.mode = mode_->text().trimmed().toUpper();
    r.rstS = rstS_->text().trimmed();
    r.rstR = rstR_->text().trimmed();
    r.name = name_->text().trimmed();
    r.handle = handle_->text().trimmed();
    r.qth = qth_->text().trimmed();
    r.grid = grid_->text().trimmed().toUpper();
    r.country = country_->text().trimmed();
    r.cqz = cqz_;
    r.ituz = ituz_;
    r.pota = pota_->text().trimmed().toUpper();
    r.comment = comment_->text().trimmed();
    return r;
}

} // namespace ttc
