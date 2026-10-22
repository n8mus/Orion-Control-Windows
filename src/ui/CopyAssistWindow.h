// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
#include <functional>

#include "asr/AsrEngine.h"

class QCheckBox;
class QLabel;
class QSpinBox;
class QTextBrowser;

namespace ttc {

// SDR ▾ → Copy Assist…: the running transcript of the SDR receive audio.
// Each utterance is a line — time, dial, text — coloured by whisper's
// confidence (AetherSDR's bands: green >= .85, yellow >= .65, orange
// >= .45, red below). Callsigns found in it are links in worked-before
// colours; clicking one opens it in the log window.
class CopyAssistWindow : public QDialog {
    Q_OBJECT
public:
    CopyAssistWindow(bool on, int threads, QWidget* parent = nullptr);
    void addResult(const AsrEngine::Result& r);
    void setStatus(const QString& s);
    void setOn(bool on);
    // 'N' new, 'B' new band, 'W' worked, 'C' confirmed, '?' unknown.
    void setStatusFor(std::function<char(const QString& call, qint64 hz)> f) {
        statusFor_ = std::move(f);
    }

signals:
    void enableChanged(bool on, int threads);
    void callClicked(const QString& call);

private:
    QCheckBox* on_ = nullptr;
    QSpinBox* threads_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* last_ = nullptr;                       // the last decode's numbers
    QTextBrowser* text_ = nullptr;
    std::function<char(const QString&, qint64)> statusFor_;
};

} // namespace ttc
