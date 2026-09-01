// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>
#include <QHash>

class QCheckBox;
class QGridLayout;
class QLabel;
class QLineEdit;

namespace ttc {

class QslUploader;

// Online-log uploads, configured the way GridTracker's Logging tab does it
// (the operator's explicit model): one row per service, credentials inline,
// a Test button that proves the setup before the first real QSO, and
// required-but-empty fields shown red. Settings write live (no OK button);
// the uploader reads them fresh on every push.
class OnlineLogsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OnlineLogsDialog(QslUploader* up, QWidget* parent = nullptr);

private:
    QLineEdit* field(const QString& key, const QString& placeholder,
                     int width, bool secret, QWidget* parent);
    QCheckBox* enableBox(const QString& svc);
    void onResult(const QString& svc, bool ok, const QString& detail);

    QslUploader* up_;
    QGridLayout* grid_ = nullptr;
    int row_ = 0;
    QHash<QString, QLabel*> results_;      // svc -> result label
};

} // namespace ttc
