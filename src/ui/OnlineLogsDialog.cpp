// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/OnlineLogsDialog.h"

#include <QCheckBox>
#include <QFrame>
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
    " #3a4a5e; border-radius: 3px; padding: 5px 8px; font-size: 13px;"
    " font-family: monospace; }";
const char* kEditMissing =
    "QLineEdit { background: #33201c; color: #ffb0a4; border: 1px solid"
    " #8a3a2e; border-radius: 3px; padding: 5px 8px; font-size: 13px;"
    " font-family: monospace; }";
} // namespace

OnlineLogsDialog::OnlineLogsDialog(QslUploader* up, QWidget* parent)
    : QDialog(parent), up_(up) {
    setModal(false);
    setWindowTitle("Setup — Online Logs");
    setMinimumWidth(1010);
    setStyleSheet(
        "QDialog { background: #141b24; color: #dde7f0; font-size: 14px; }"
        "QLabel { color: #b8c8d8; font-size: 13px; }"
        "QCheckBox::indicator { width: 17px; height: 17px; }"
        "QPushButton { background: #24303e; color: #dde7f0; border: 1px"
        " solid #3a4a5e; border-radius: 3px; padding: 6px 10px;"
        " font-size: 12px; font-weight: bold; min-width: 104px; }"
        "QPushButton:hover { border-color: #6aa5d8; }");

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(0);

    grid_ = new QGridLayout;
    grid_->setHorizontalSpacing(16);
    grid_->setVerticalSpacing(0);
    grid_->setColumnMinimumWidth(0, 104);      // service
    grid_->setColumnMinimumWidth(1, 40);       // enable
    grid_->setColumnStretch(2, 1);             // details
    grid_->setColumnMinimumWidth(4, 210);      // result
    const auto header = [this](int col, const char* t) {
        auto* l = new QLabel(QLatin1String(t), this);
        l->setStyleSheet("QLabel { color: #77869a; font-size: 11px;"
                         " letter-spacing: 1px; font-weight: 600; }");
        grid_->addWidget(l, 0, col);
    };
    header(0, "SERVICE");
    header(1, "LOG?");
    header(2, "DETAILS");
    header(3, "TEST");
    header(4, "RESULT");
    row_ = 1;

    // Each service occupies two grid rows: a spacer/separator row and the
    // content row — the separator lines are what make it read as a table.
    const auto beginRow = [this](const char* name, const QString& svc) {
        auto* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("QFrame { color: #212b38; background: #212b38;"
                           " max-height: 1px; border: none; }");
        grid_->addWidget(sep, row_, 0, 1, 5);
        ++row_;
        grid_->setRowMinimumHeight(row_, 56);
        auto* l = new QLabel(QLatin1String(name), this);
        l->setStyleSheet("QLabel { color: #e6edf5; font-weight: 600;"
                         " font-size: 14px; padding-top: 10px; }");
        grid_->addWidget(l, row_, 0, Qt::AlignTop);
        auto* c = enableBox(svc);
        auto* cw = new QWidget(this);
        auto* cl = new QVBoxLayout(cw);
        cl->setContentsMargins(0, 10, 0, 0);
        cl->addWidget(c);
        cl->addStretch(1);
        grid_->addWidget(cw, row_, 1, Qt::AlignTop);
    };
    const auto detailBox = [this] {
        auto* w = new QWidget(this);
        auto* lay = new QVBoxLayout(w);
        lay->setContentsMargins(0, 8, 0, 8);
        lay->setSpacing(6);
        grid_->addWidget(w, row_, 2);
        return lay;
    };
    const auto lineOf = [](QVBoxLayout* box) {
        auto* h = new QHBoxLayout;
        h->setSpacing(8);
        box->addLayout(h);
        h->addStretch(1);
        return h;
    };
    const auto pair = [this](QHBoxLayout* lay, const char* label,
                             QLineEdit* e) {
        auto* l = new QLabel(QLatin1String(label), this);
        l->setStyleSheet("QLabel { color: #77869a; font-size: 10px;"
                         " letter-spacing: 0.5px; }");
        const int at = lay->count() - 1;       // before the stretch
        lay->insertWidget(at, l);
        lay->insertWidget(at + 1, e);
        lay->insertSpacing(at + 2, 8);
    };
    const auto testCol = [this](std::initializer_list<QPushButton*> btns) {
        auto* w = new QWidget(this);
        auto* lay = new QVBoxLayout(w);
        lay->setContentsMargins(0, 8, 0, 8);
        lay->setSpacing(6);
        for (QPushButton* b : btns) lay->addWidget(b);
        lay->addStretch(1);
        grid_->addWidget(w, row_, 3, Qt::AlignTop);
    };
    const auto resultLabel = [this](const QString& svc) {
        auto* l = new QLabel("—", this);
        l->setWordWrap(true);
        l->setStyleSheet("QLabel { color: #6d7b8c; font-size: 12px;"
                         " padding-top: 12px; }");
        l->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        grid_->addWidget(l, row_, 4, Qt::AlignTop);
        results_.insert(svc, l);
    };
    const auto testBtn = [this](const char* text) {
        auto* b = new QPushButton(QLatin1String(text), this);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };

    // ---- LoTW ---------------------------------------------------------
    {
        beginRow("LoTW", "lotw");
        auto* box = detailBox();
        auto* r1 = lineOf(box);
        pair(r1, "LOGIN",
             field("lotw/login", "LoTW username", 110, false, this));
        pair(r1, "PASSWORD", field("lotw/password", "", 120, true, this));
        pair(r1, "STATION",
             field("lotw/station", "TQSL station name", 140, false, this));
        auto* r2 = lineOf(box);
        pair(r2, "TQSL PATH",
             field("lotw/tqslPath", "", 340, false, this,
#ifdef Q_OS_WIN
                   "C:/Program Files (x86)/TrustedQSL/tqsl.exe"
#else
                   "tqsl"
#endif
                   ));
        pair(r2, "TQSL PASSWORD",
             field("lotw/tqslPassword", "usually empty", 120, true, this));
        auto* t1 = testBtn("Test Download");
        auto* t2 = testBtn("Test TQSL");
        testCol({t1, t2});
        resultLabel("lotw");
        connect(t1, &QPushButton::clicked, this,
                [this] { up_->testLotwDownload(); });
        connect(t2, &QPushButton::clicked, this,
                [this] { up_->testTqsl(); });
        ++row_;
    }
    // ---- eQSL ---------------------------------------------------------
    {
        beginRow("eQSL.cc", "eqsl");
        auto* box = detailBox();
        auto* r = lineOf(box);
        pair(r, "USER",
             field("eqsl/user", "eQSL username", 110, false, this));
        pair(r, "PASSWORD", field("eqsl/password", "", 120, true, this));
        pair(r, "QTH NICKNAME",
             field("eqsl/nickname", "optional", 110, false, this));
        auto* t = testBtn("Test");
        testCol({t});
        resultLabel("eqsl");
        connect(t, &QPushButton::clicked, this,
                [this] { up_->testEqsl(); });
        ++row_;
    }
    // ---- QRZ ----------------------------------------------------------
    {
        beginRow("QRZ.com", "qrz");
        auto* box = detailBox();
        auto* r = lineOf(box);
        pair(r, "LOGBOOK API KEY", field("qrz/key", "", 240, true, this));
        auto* t = testBtn("Test");
        testCol({t});
        resultLabel("qrz");
        connect(t, &QPushButton::clicked, this, [this] { up_->testQrz(); });
        ++row_;
    }
    // ---- ClubLog ------------------------------------------------------
    {
        beginRow("ClubLog", "club");
        auto* box = detailBox();
        auto* r = lineOf(box);
        pair(r, "CALLSIGN",
             field("club/call", "ClubLog callsign", 110, false, this));
        pair(r, "PASSWORD", field("club/password", "", 120, true, this));
        pair(r, "EMAIL",
             field("club/email", "account email", 180, false, this));
        auto* r2 = lineOf(box);
        pair(r2, "APP API KEY", field("club/apikey", "", 240, true, this));
        auto* note = new QLabel("issued per application — one email to "
                                "ClubLog to request", this);
        note->setStyleSheet("QLabel { color: #5f6e7e; font-size: 11px; }");
        r2->insertWidget(r2->count() - 1, note);
        auto* t = testBtn("Check");
        testCol({t});
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
        beginRow("HRDLOG.net", "hrdlog");
        auto* box = detailBox();
        auto* r = lineOf(box);
        pair(r, "CALLSIGN",
             field("hrdlog/call", "your callsign", 110, false, this));
        pair(r, "UPLOAD CODE", field("hrdlog/code", "", 140, true, this));
        auto* t = testBtn("Check");
        testCol({t});
        resultLabel("hrdlog");
        connect(t, &QPushButton::clicked, this,
                [this] { up_->testHrdlogNet(); });
        ++row_;
    }
    // ---- HRD Logbook (local mirror) -----------------------------------
    {
        beginRow("HRD Logbook", "hrd");
        auto* box = detailBox();
        auto* r = lineOf(box);
        pair(r, "IP",
             field("hrd/ip", "", 120, false, this, "127.0.0.1"));
        pair(r, "PORT", field("hrd/port", "", 70, false, this, "7826"));
        auto* t = testBtn("Test");
        testCol({t});
        resultLabel("hrd");
        connect(t, &QPushButton::clicked, this,
                [this] { up_->testLoggerPush("hrd"); });
        ++row_;
    }
    // ---- N1MM+ (local mirror) -----------------------------------------
    {
        beginRow("N1MM Logger+", "n1mm");
        auto* box = detailBox();
        auto* r = lineOf(box);
        pair(r, "IP",
             field("n1mm/ip", "", 120, false, this, "127.0.0.1"));
        pair(r, "PORT", field("n1mm/port", "", 70, false, this, "2333"));
        auto* t = testBtn("Test");
        testCol({t});
        resultLabel("n1mm");
        connect(t, &QPushButton::clicked, this,
                [this] { up_->testLoggerPush("n1mm"); });
        ++row_;
    }

    v->addLayout(grid_);
    v->addSpacing(12);
    auto* foot = new QLabel(
        "Uploads fire the moment LOG QSO is pressed; failures queue and "
        "retry every two minutes. Corrections re-send (the services "
        "dedupe). Deletes stay local, by design.", this);
    foot->setWordWrap(true);
    foot->setStyleSheet("QLabel { color: #6d7b8c; font-size: 12px; }");
    v->addWidget(foot);
    v->addSpacing(8);
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
                                   bool secret, QWidget* parent,
                                   const QString& def) {
    // A field with a working default carries the REAL value (seeded into
    // settings), and placeholders are gray DESCRIPTIONS of what belongs
    // here — never example values. A hint that looks like a filled-in
    // answer reads as one, and a red field showing its own default reads
    // as broken (both live-found, 2026-09-01).
    if (!def.isEmpty()
        && QSettings().value("up/" + key).toString().trimmed().isEmpty())
        QSettings().setValue("up/" + key, def);
    auto* e = new QLineEdit(QSettings().value("up/" + key).toString(),
                            parent);
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(width);
    if (secret) e->setEchoMode(QLineEdit::Password);
    const auto paint = [e] {
        e->setStyleSheet(e->text().trimmed().isEmpty() ? kEditMissing
                                                       : kEditBase);
    };
    const bool optional = key.endsWith("tqslPassword")
        || key.endsWith("nickname");
    if (optional) e->setStyleSheet(kEditBase);
    else paint();
    connect(e, &QLineEdit::textChanged, this,
            [key, paint, optional](const QString& t) {
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
    l->setStyleSheet(ok ? "QLabel { color: #82de8c; font-size: 12px;"
                          " padding-top: 12px; }"
                        : "QLabel { color: #ff5c46; font-size: 12px;"
                          " padding-top: 12px; }");
}

} // namespace ttc
