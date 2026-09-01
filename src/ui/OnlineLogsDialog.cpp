// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/OnlineLogsDialog.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "log/QslUploader.h"

namespace ttc {

namespace {
const char* kEditBase =
    "QLineEdit { background: #1c2430; color: #cfe0b9; border: 1px solid"
    " #3a4a5e; border-radius: 3px; padding: 4px 8px; font-size: 13px;"
    " font-family: monospace; }";
const char* kEditMissing =
    "QLineEdit { background: #3a1c18; color: #ffb0a4; border: 1px solid"
    " #a03a2c; border-radius: 3px; padding: 4px 8px; font-size: 13px;"
    " font-family: monospace; }";
} // namespace

OnlineLogsDialog::OnlineLogsDialog(QslUploader* up, QWidget* parent)
    : QDialog(parent), up_(up) {
    setModal(false);
    setWindowTitle("Setup — Online Logs");
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 14px; }"
        "QLabel { color: #b8c8d8; font-size: 13px; }"
        "QCheckBox { color: #b8c8d8; }"
        "QCheckBox::indicator { width: 16px; height: 16px; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px"
        " solid #3a4a5e; border-radius: 3px; padding: 5px 12px;"
        " font-size: 12px; font-weight: bold; }"
        "QPushButton:hover { border-color: #6aa5d8; }");

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(14, 12, 14, 12);
    v->setSpacing(10);

    grid_ = new QGridLayout;
    grid_->setHorizontalSpacing(12);
    grid_->setVerticalSpacing(10);
    grid_->setColumnStretch(2, 1);
    const auto header = [this](int col, const char* t) {
        auto* l = new QLabel(QLatin1String(t), this);
        l->setStyleSheet("QLabel { color: #77869a; font-size: 11px;"
                         " letter-spacing: 1px; }");
        grid_->addWidget(l, 0, col);
    };
    header(0, "SERVICE");
    header(1, "LOG?");
    header(2, "DETAILS");
    header(3, "TEST");
    header(4, "RESULT");
    row_ = 1;

    const auto svcName = [this](const char* t) {
        auto* l = new QLabel(QLatin1String(t), this);
        l->setStyleSheet("QLabel { color: #dde7f0; font-weight: 600; }");
        grid_->addWidget(l, row_, 0, Qt::AlignTop);
    };
    const auto resultLabel = [this](const QString& svc) {
        auto* l = new QLabel("—", this);
        l->setWordWrap(true);
        l->setMinimumWidth(170);
        l->setMaximumWidth(230);
        grid_->addWidget(l, row_, 4, Qt::AlignTop);
        results_.insert(svc, l);
        return l;
    };
    const auto testBtn = [this](const char* text) {
        auto* b = new QPushButton(QLatin1String(text), this);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    const auto pair = [this](QHBoxLayout* lay, const char* label,
                             QLineEdit* e) {
        auto* l = new QLabel(QLatin1String(label), this);
        l->setStyleSheet("QLabel { color: #77869a; font-size: 11px; }");
        lay->addWidget(l);
        lay->addWidget(e);
    };

    // ---- LoTW ---------------------------------------------------------
    {
        svcName("LoTW");
        grid_->addWidget(enableBox("lotw"), row_, 1, Qt::AlignTop);
        auto* box = new QVBoxLayout;
        auto* r1 = new QHBoxLayout;
        pair(r1, "Login",
             field("lotw/login", "LoTW username", 110, false, this));
        pair(r1, "Password",
             field("lotw/password", "", 110, true, this));
        pair(r1, "Station",
             field("lotw/station", "TQSL station name", 130, false, this));
        r1->addStretch(1);
        box->addLayout(r1);
        auto* r2 = new QHBoxLayout;
        // Pre-seed the default so a working out-of-the-box path never
        // shows as a red missing field (live confusion, 2026-09-01).
        if (QSettings().value("up/lotw/tqslPath").toString().isEmpty())
            QSettings().setValue("up/lotw/tqslPath",
#ifdef Q_OS_WIN
                "C:/Program Files (x86)/TrustedQSL/tqsl.exe"
#else
                "tqsl"
#endif
            );
        pair(r2, "TQSL path",
             field("lotw/tqslPath",
                   "C:/Program Files (x86)/TrustedQSL/tqsl.exe", 320,
                   false, this));
        pair(r2, "TQSL password",
             field("lotw/tqslPassword", "usually empty", 110, true, this));
        r2->addStretch(1);
        box->addLayout(r2);
        grid_->addLayout(box, row_, 2);
        auto* tb = new QVBoxLayout;
        auto* t1 = testBtn("Test Download");
        auto* t2 = testBtn("Test TQSL");
        tb->addWidget(t1);
        tb->addWidget(t2);
        tb->addStretch(1);
        grid_->addLayout(tb, row_, 3);
        resultLabel("lotw");
        connect(t1, &QPushButton::clicked, up_,
                [this] { up_->testLotwDownload(); });
        connect(t2, &QPushButton::clicked, up_,
                [this] { up_->testTqsl(); });
        ++row_;
    }
    // ---- eQSL ---------------------------------------------------------
    {
        svcName("eQSL.cc");
        grid_->addWidget(enableBox("eqsl"), row_, 1, Qt::AlignTop);
        auto* r = new QHBoxLayout;
        pair(r, "User",
             field("eqsl/user", "eQSL username", 110, false, this));
        pair(r, "Password", field("eqsl/password", "", 110, true, this));
        pair(r, "QTH nickname",
             field("eqsl/nickname", "optional", 100, false, this));
        r->addStretch(1);
        grid_->addLayout(r, row_, 2);
        auto* t = testBtn("Test");
        grid_->addWidget(t, row_, 3, Qt::AlignTop);
        resultLabel("eqsl");
        connect(t, &QPushButton::clicked, up_, [this] { up_->testEqsl(); });
        ++row_;
    }
    // ---- QRZ ----------------------------------------------------------
    {
        svcName("QRZ.com");
        grid_->addWidget(enableBox("qrz"), row_, 1, Qt::AlignTop);
        auto* r = new QHBoxLayout;
        pair(r, "Logbook API key",
             field("qrz/key", "", 220, true, this));
        r->addStretch(1);
        grid_->addLayout(r, row_, 2);
        auto* t = testBtn("Test");
        grid_->addWidget(t, row_, 3, Qt::AlignTop);
        resultLabel("qrz");
        connect(t, &QPushButton::clicked, up_, [this] { up_->testQrz(); });
        ++row_;
    }
    // ---- ClubLog ------------------------------------------------------
    {
        svcName("ClubLog");
        grid_->addWidget(enableBox("club"), row_, 1, Qt::AlignTop);
        auto* box = new QVBoxLayout;
        auto* r = new QHBoxLayout;
        pair(r, "Callsign",
             field("club/call", "ClubLog callsign", 120, false, this));
        pair(r, "Password", field("club/password", "", 110, true, this));
        pair(r, "Email",
             field("club/email", "account email", 170, false, this));
        r->addStretch(1);
        box->addLayout(r);
        auto* r2 = new QHBoxLayout;
        pair(r2, "App API key", field("club/apikey", "", 220, true, this));
        auto* note = new QLabel(
            "applications need a key from ClubLog — one email to request",
            this);
        note->setStyleSheet("QLabel { color: #6d7b8c; font-size: 11px; }");
        r2->addWidget(note);
        r2->addStretch(1);
        box->addLayout(r2);
        grid_->addLayout(box, row_, 2);
        auto* t = testBtn("Check");
        grid_->addWidget(t, row_, 3, Qt::AlignTop);
        resultLabel("club");
        connect(t, &QPushButton::clicked, this, [this] {
            const QSettings s;
            const bool ok = !s.value("up/club/call").toString().isEmpty()
                && !s.value("up/club/password").toString().isEmpty()
                && !s.value("up/club/email").toString().isEmpty()
                && !s.value("up/club/apikey").toString().isEmpty();
            onResult("club", ok,
                     ok ? "fields set — verified on first upload"
                        : "fill callsign, password, email and app key");
        });
        ++row_;
    }
    // ---- HRDLOG.net ---------------------------------------------------
    {
        svcName("HRDLOG.net");
        grid_->addWidget(enableBox("hrdlog"), row_, 1, Qt::AlignTop);
        auto* r = new QHBoxLayout;
        pair(r, "Callsign",
             field("hrdlog/call", "your callsign", 110, false, this));
        pair(r, "Upload code",
             field("hrdlog/code", "", 130, true, this));
        r->addStretch(1);
        grid_->addLayout(r, row_, 2);
        auto* t = testBtn("Check");
        grid_->addWidget(t, row_, 3, Qt::AlignTop);
        resultLabel("hrdlog");
        connect(t, &QPushButton::clicked, up_,
                [this] { up_->testHrdlogNet(); });
        ++row_;
    }
    // ---- HRD Logbook (local mirror) -----------------------------------
    {
        svcName("HRD Logbook");
        grid_->addWidget(enableBox("hrd"), row_, 1, Qt::AlignTop);
        auto* r = new QHBoxLayout;
        pair(r, "IP", field("hrd/ip", "127.0.0.1", 110, false, this));
        pair(r, "Port", field("hrd/port", "7826", 60, false, this));
        r->addStretch(1);
        grid_->addLayout(r, row_, 2);
        auto* t = testBtn("Test");
        grid_->addWidget(t, row_, 3, Qt::AlignTop);
        resultLabel("hrd");
        connect(t, &QPushButton::clicked, up_,
                [this] { up_->testLoggerPush("hrd"); });
        ++row_;
    }
    // ---- N1MM+ (local mirror) -----------------------------------------
    {
        svcName("N1MM Logger+");
        grid_->addWidget(enableBox("n1mm"), row_, 1, Qt::AlignTop);
        auto* r = new QHBoxLayout;
        pair(r, "IP", field("n1mm/ip", "127.0.0.1", 110, false, this));
        pair(r, "Port", field("n1mm/port", "2333", 60, false, this));
        r->addStretch(1);
        grid_->addLayout(r, row_, 2);
        auto* t = testBtn("Test");
        grid_->addWidget(t, row_, 3, Qt::AlignTop);
        resultLabel("n1mm");
        connect(t, &QPushButton::clicked, up_,
                [this] { up_->testLoggerPush("n1mm"); });
        ++row_;
    }

    v->addLayout(grid_);
    auto* foot = new QLabel(
        "Uploads fire the moment LOG QSO is pressed; failures queue and "
        "retry every two minutes. Corrections re-send (the services "
        "dedupe). Deletes stay local, by design.", this);
    foot->setWordWrap(true);
    foot->setStyleSheet("QLabel { color: #6d7b8c; font-size: 12px; }");
    v->addWidget(foot);
    auto* close = new QPushButton("Close", this);
    auto* fr = new QHBoxLayout;
    fr->addStretch(1);
    fr->addWidget(close);
    v->addLayout(fr);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    if (up_)
        connect(up_, &QslUploader::serviceResult, this,
                [this](const QString& svc, bool ok, const QString& d) {
                    onResult(svc, ok, d);
                });
}

QLineEdit* OnlineLogsDialog::field(const QString& key,
                                   const QString& placeholder, int width,
                                   bool secret, QWidget* parent) {
    // Placeholders are gray DESCRIPTIONS of what belongs in the field,
    // never example values — a hint that looks like a filled-in answer
    // reads as one (live confusion, 2026-09-01).
    auto* e = new QLineEdit(QSettings().value("up/" + key).toString(),
                            parent);
    e->setPlaceholderText(placeholder);
    e->setMaximumWidth(width);
    e->setMinimumWidth(qMin(width, 60));
    if (secret) e->setEchoMode(QLineEdit::Password);
    const auto paint = [e] {
        e->setStyleSheet(e->text().trimmed().isEmpty() ? kEditMissing
                                                       : kEditBase);
    };
    // The TQSL-password field is legitimately empty for this station's
    // certificates — never paint it red.
    const bool optional = key.endsWith("tqslPassword")
        || key.endsWith("nickname");
    if (optional) e->setStyleSheet(kEditBase);
    else paint();
    connect(e, &QLineEdit::textChanged, this,
            [key, e, paint, optional](const QString& t) {
                QSettings().setValue("up/" + key, t.trimmed());
                if (!optional) paint();
            });
    return e;
}

QCheckBox* OnlineLogsDialog::enableBox(const QString& svc) {
    auto* c = new QCheckBox(this);
    c->setChecked(QSettings().value("up/" + svc + "/enabled",
                                    false).toBool());
    connect(c, &QCheckBox::toggled, this, [svc](bool onNow) {
        QSettings().setValue("up/" + svc + "/enabled", onNow);
    });
    return c;
}

void OnlineLogsDialog::onResult(const QString& svc, bool ok,
                                const QString& detail) {
    // LoTW's two tests share the row's label.
    QString key = svc;
    if (svc == "lotw-dl" || svc == "lotw-tqsl") key = "lotw";
    auto* l = results_.value(key);
    if (!l) return;
    l->setText((ok ? "✓ " : "✕ ") + detail);
    l->setStyleSheet(ok ? "QLabel { color: #82de8c; font-size: 12px; }"
                        : "QLabel { color: #ff5c46; font-size: 12px; }");
}

} // namespace ttc
