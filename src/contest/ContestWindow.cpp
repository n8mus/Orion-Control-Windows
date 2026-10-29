// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestWindow.h"
#include <QFont>
#include <QDialog>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QApplication>

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "contest/Cabrillo.h"
#include "contest/ContestEngine.h"
#include "util/CtyLookup.h"

namespace ttc {

namespace {
QString rstDefault(const QString& mode) {
    return mode == QLatin1String("SSB") ? QStringLiteral("59")
                                        : QStringLiteral("599");
}
} // namespace

ContestWindow::ContestWindow(ContestDb* db, const CtyLookup* cty,
                             LogDb* logDb, QWidget* parent)
    : QDialog(parent), db_(db), cty_(cty), logDb_(logDb) {
    setWindowTitle("Contest manager");
    buildUi();
    connect(db_, &ContestDb::changed, this, [this] {
        if (contestId_ >= 0) refreshAll();
        refreshResumeList();
    });
    const QByteArray geom =
        QSettings().value("contest/mgrGeom").toByteArray();
    if (!geom.isEmpty()) restoreGeometry(geom);
    refreshResumeList();
    // Come up on the live contest so the log grid is one click away.
    const qint64 last =
        QSettings().value("contest/currentId", -1).toLongLong();
    if (last > 0) openContest(last);
}

void ContestWindow::buildUi() {
    auto* lay = new QVBoxLayout(this);

    // ---- new / resume ---------------------------------------------------
    {
        auto* h = new QHBoxLayout;
        h->addWidget(new QLabel("New:", this));
        defPick_ = new QComboBox(this);
        defPick_->setObjectName("defPick");
        // A blank first entry so nothing is armed until the operator
        // chooses — Start on a preselected top-of-list is exactly what
        // minted the seven practice contests.
        defPick_->addItem("— choose a contest —", QString());
        for (const ContestDef* d : contestDefs())
            defPick_->addItem(d->title, d->id);
        h->addWidget(defPick_);
        h->addWidget(new QLabel("sent exch:", this));
        sentExchEdit_ = new QLineEdit(this);
        sentExchEdit_->setObjectName("sentExch");
        sentExchEdit_->setFixedWidth(110);
        sentExchEdit_->setToolTip(
            "What you send besides RST/serial — All Asian your age, "
            "CQ WW your zone, Sweepstakes prec/check/section.\n"
            "REQUIRED for those; serial contests can leave it empty.");
        h->addWidget(sentExchEdit_);
        h->addWidget(new QLabel("location:", this));
        locationEdit_ = new QLineEdit(
            QSettings().value("contest/location", "").toString(), this);
        locationEdit_->setFixedWidth(50);
        locationEdit_->setToolTip(
            "Cabrillo LOCATION: (ARRL section / state)");
        h->addWidget(locationEdit_);
        h->addWidget(new QLabel("email:", this));
        emailEdit_ = new QLineEdit(
            QSettings().value("station/email",
                              QSettings().value("club/email").toString())
                .toString(),
            this);
        emailEdit_->setObjectName("email");
        emailEdit_->setFixedWidth(170);
        emailEdit_->setToolTip("Cabrillo EMAIL: — the log robots require "
                               "it. Saved with your station.");
        connect(emailEdit_, &QLineEdit::editingFinished, this, [this] {
            QSettings().setValue("station/email",
                                 emailEdit_->text().trimmed());
        });
        h->addWidget(emailEdit_);
        auto* startBtn = new QPushButton("Start", this);
        connect(startBtn, &QPushButton::clicked, this,
                [this] { newContest(); });
        h->addWidget(startBtn);
        h->addSpacing(18);
        h->addWidget(new QLabel("Resume:", this));
        resumePick_ = new QComboBox(this);
        resumePick_->setObjectName("resumePick");
        resumePick_->setMinimumWidth(220);
        h->addWidget(resumePick_);
        auto* openBtn = new QPushButton("Open", this);
        connect(openBtn, &QPushButton::clicked, this, [this] {
            const qint64 id = resumePick_->currentData().toLongLong();
            if (id > 0) openContest(id);
        });
        h->addWidget(openBtn);
        auto* delBtn = new QPushButton("Delete", this);
        delBtn->setToolTip("Remove the selected contest — only if it has "
                           "no QSOs.\nA logged contest is cleared "
                           "QSO-by-QSO, never wholesale.");
        connect(delBtn, &QPushButton::clicked, this,
                [this] { deleteContestRow(); });
        h->addWidget(delBtn);
        h->addStretch(1);
        // Picking a contest seeds its default exchange and flags the
        // field REQUIRED when that contest sends a fixed token.
        connect(defPick_, &QComboBox::currentIndexChanged, this, [this] {
            const ContestDef* d =
                contestDef(defPick_->currentData().toString());
            sentExchEdit_->setText(d ? d->sentExchDefault : QString());
            const bool needs =
                d && d->cabExch.contains(QStringLiteral("exch"));
            sentExchEdit_->setPlaceholderText(
                needs ? QStringLiteral("REQUIRED") : QString());
        });
        defPick_->setCurrentIndex(0);    // the blank entry
        lay->addLayout(h);
    }

    // ---- header ---------------------------------------------------------
    {
        auto* h = new QHBoxLayout;
        title_ = new QLabel("no contest open", this);
        QFont bf = title_->font();
        bf.setBold(true);
        title_->setFont(bf);
        h->addWidget(title_);
        // The OPEN contest's sent exchange, editable — All Asian showed
        // why: the age was left blank at Start and every Cabrillo line
        // shipped without it. Change here writes straight to the row.
        h->addSpacing(14);
        h->addWidget(new QLabel("sent exch:", this));
        openExchEdit_ = new QLineEdit(this);
        openExchEdit_->setObjectName("openSentExch");
        openExchEdit_->setFixedWidth(120);
        openExchEdit_->setToolTip(
            "What you send every QSO besides RST/serial — your age for "
            "All Asian, zone for CQ WW.\nEmpty here means a blank field "
            "in the Cabrillo, which log checkers reject.");
        connect(openExchEdit_, &QLineEdit::editingFinished, this, [this] {
            if (contestId_ < 0) return;
            row_.sentExch = openExchEdit_->text().trimmed();
            db_->updateContest(row_);
            refreshAll();
        });
        h->addWidget(openExchEdit_);
        score_ = new QLabel(this);
        h->addWidget(score_);
        h->addStretch(1);
        lay->addLayout(h);
    }

    // ---- the log --------------------------------------------------------
    table_ = new QTableWidget(this);
    table_->setColumnCount(12);
    table_->setHorizontalHeaderLabels({"UTC", "Call", "kHz", "Snt", "SNr",
                                       "Rcv", "RNr", "Ex1", "Ex2", "Ex3",
                                       "Pt", "QTC"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { editSelected(); });
    lay->addWidget(table_, 1);

    {
        auto* h = new QHBoxLayout;
        auto* editBtn = new QPushButton("Edit…", this);
        connect(editBtn, &QPushButton::clicked, this,
                [this] { editSelected(); });
        h->addWidget(editBtn);
        auto* delBtn = new QPushButton("Delete", this);
        connect(delBtn, &QPushButton::clicked, this,
                [this] { deleteSelected(); });
        h->addWidget(delBtn);
        h->addStretch(1);
        auto* adifBtn = new QPushButton("ADIF…", this);
        adifBtn->setToolTip("Write this contest's QSOs to a .adi file "
                            "(cqrlog's File ▸ Import takes it whole)");
        connect(adifBtn, &QPushButton::clicked, this,
                [this] { exportAdif(); });
        h->addWidget(adifBtn);
        auto* pushBtn = new QPushButton("→ Logbook", this);
        pushBtn->setToolTip(
            "Copy this contest's QSOs into the everyday station log —\n"
            "worked-before colors, LoTW and the online logs pick them up "
            "from there.\nSafe to press twice: duplicates are skipped.");
        connect(pushBtn, &QPushButton::clicked, this,
                [this] { pushToLogbook(); });
        h->addWidget(pushBtn);
        auto* sumBtn = new QPushButton("Summary…", this);
        sumBtn->setToolTip("Post-contest summary: QSOs/points/mults by band, on-air "
                           "time, best hour, QSOs by hour — copy it into the soapbox");
        connect(sumBtn, &QPushButton::clicked, this, [this] { showSummary(); });
        h->addWidget(sumBtn);
        auto* catBtn = new QPushButton("Categories…", this);
        catBtn->setToolTip("Cabrillo CATEGORY-* header lines for this "
                           "contest (operator / assisted / band / power / "
                           "transmitter / station / overlay), CLUB and "
                           "SOAPBOX. Remembered for the next contest.");
        connect(catBtn, &QPushButton::clicked, this,
                [this] { editCategories(); });
        h->addWidget(catBtn);
        auto* cabBtn = new QPushButton("Cabrillo…", this);
        connect(cabBtn, &QPushButton::clicked, this,
                [this] { exportCabrillo(); });
        h->addWidget(cabBtn);
        lay->addLayout(h);
    }

    status_ = new QLabel(this);
    lay->addWidget(status_);
    resize(880, 560);
}

void ContestWindow::refreshResumeList() {
    const qint64 keep = resumePick_->currentData().toLongLong();
    resumePick_->clear();
    for (const ContestRow& c : db_->contests())
        resumePick_->addItem(
            QString("%1  (%2 QSOs)")
                .arg(c.title)
                .arg(db_->qsos(c.id).size()),
            c.id);
    if (keep > 0) {
        const int i = resumePick_->findData(keep);
        if (i >= 0) resumePick_->setCurrentIndex(i);
    }
}

bool ContestWindow::openContestId(qint64 id) {
    openContest(id);
    return contestId_ == id;
}

void ContestWindow::newContest() {
    const ContestDef* d = contestDef(defPick_->currentData().toString());
    if (!d) {
        status_->setText("choose a contest first");
        return;
    }
    // A fixed-exchange contest may not START blank — that is the All
    // Asian empty-age failure, caught before a single QSO is logged.
    if (d->cabExch.contains(QStringLiteral("exch"))
        && sentExchEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(
            this, "Sent exchange required",
            QString("%1 sends a fixed exchange every QSO — your age for "
                    "All Asian, zone for CQ WW, prec/check/section for "
                    "Sweepstakes.\n\nFill \"sent exch\" before Start.")
                .arg(d->title));
        sentExchEdit_->setFocus();
        return;
    }
    ContestRow c;
    c.defId = d->id;
    c.title = d->title + " — "
        + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd");
    c.startUtc = QDateTime::currentDateTimeUtc();
    c.sentExch = sentExchEdit_->text().trimmed();
    c.catMode = d->modeCategory;
    // Start from the operator's real setup (last confirmed categories),
    // not the struct's LOW/ASSISTED defaults. Mode stays the def's.
    {
        QSettings s;
        const auto seed = [&](const char* key, QString& field) {
            const QString v = s.value(key).toString().trimmed();
            if (!v.isEmpty()) field = v;
        };
        seed("contest/lastCatOp", c.catOp);
        seed("contest/lastCatAssisted", c.catAssisted);
        seed("contest/lastCatPower", c.catPower);
        seed("contest/lastCatTx", c.catTx);
        seed("contest/lastCatStation", c.catStation);
        seed("contest/lastClub", c.club);
        // Band and overlay are per-event choices — ALL / none each time.
    }
    const qint64 id = db_->createContest(c);
    if (id < 0) {
        status_->setText("could not create the contest (database error)");
        return;
    }
    QSettings().setValue("contest/location",
                         locationEdit_->text().trimmed());
    trace(QString("NEW %1 \"%2\"").arg(d->id, c.title));
    openContest(id);
}

void ContestWindow::deleteContestRow() {
    const qint64 id = resumePick_->currentData().toLongLong();
    if (id <= 0) return;
    const ContestRow c = db_->contest(id);
    const int n = db_->qsos(id).size();
    if (n > 0) {
        QMessageBox::warning(
            this, "Contest not empty",
            QString("\"%1\" holds %2 QSO%3 — a logged contest is cleared "
                    "QSO-by-QSO (open it, delete rows), never wholesale.")
                .arg(c.title)
                .arg(n)
                .arg(n == 1 ? "" : "s"));
        return;
    }
    if (QMessageBox::question(
            this, "Delete contest",
            QString("Remove \"%1\"? It has no QSOs.").arg(c.title))
        != QMessageBox::Yes)
        return;
    if (!db_->deleteContest(id)) {
        status_->setText("delete refused (contest not empty, or "
                         "database error)");
        return;
    }
    trace(QString("MGR DELETE contest id=%1 \"%2\"").arg(id).arg(c.title));
    emit contestDeleted(id);
    if (contestId_ == id) {          // the manager was showing it
        contestId_ = -1;
        def_ = nullptr;
        title_->setText("no contest open");
        score_->clear();
        table_->setRowCount(0);
        QSettings().remove("contest/currentId");
    }
    refreshResumeList();
    status_->setText(QString("deleted \"%1\"").arg(c.title));
}

void ContestWindow::openContest(qint64 id) {
    row_ = db_->contest(id);
    def_ = contestDef(row_.defId);
    if (row_.id < 0 || !def_) {
        status_->setText("contest not found or its definition is missing");
        return;
    }
    contestId_ = id;
    QSettings().setValue("contest/currentId", id);
    ctx_ = ContestContext();
    ctx_.myCall =
        QSettings().value("station/callsign", "").toString().toUpper();
    CtyInfo me;
    if (cty_ && cty_->info(normalizeForCty(ctx_.myCall), me)) {
        ctx_.myCont = me.cont;
        ctx_.myCountry = me.country;
        ctx_.myCq = me.cq;
        ctx_.myItu = me.itu;
    }
    title_->setText(row_.title);
    setWindowTitle("Contest manager — " + row_.title);
    if (openExchEdit_) {
        openExchEdit_->setText(row_.sentExch);
        // Only meaningful when the contest sends a fixed exchange token.
        const bool needsExch =
            def_ && def_->cabExch.contains(QStringLiteral("exch"));
        openExchEdit_->setEnabled(needsExch);
        openExchEdit_->setPlaceholderText(
            needsExch ? QStringLiteral("REQUIRED") : QString());
    }
    refreshAll();
    trace(QString("MGR OPEN %1 id=%2").arg(row_.defId).arg(id));
    emit contestOpened(id);
}

void ContestWindow::refreshAll() {
    qsos_ = db_->qsos(contestId_);
    QList<CQsoValues> vals;
    for (const ContestQso& q : qsos_) vals << q.v;
    const int qtcN = def_->hasQtc ? db_->qtcCount(contestId_) : 0;
    const ScoreBreakdown sb =
        computeScore(*def_, vals, cty_, ctx_, qtcN);
    QString s = QString("· %1 QSOs · %2 pts · %3 wt-mults · score %4")
                    .arg(sb.qsos)
                    .arg(sb.points)
                    .arg(sb.weightedMults)
                    .arg(QLocale::c().toString(qlonglong(sb.total)));
    if (def_->hasQtc) s += QString(" · QTC %1").arg(qtcN);
    score_->setText(s);

    const QSet<qint64> reported =
        def_->hasQtc ? db_->qtcReportedQsoIds(contestId_)
                     : QSet<qint64>();
    table_->setRowCount(int(qsos_.size()));
    for (int i = 0; i < qsos_.size(); ++i) {
        const ContestQso& q = qsos_[qsos_.size() - 1 - i];  // newest first
        const auto put = [&](int col, const QString& t) {
            auto* it = new QTableWidgetItem(t);
            if (col == 0) it->setData(Qt::UserRole, q.id);
            table_->setItem(i, col, it);
        };
        put(0, q.tsUtc.toString("HHmm"));
        put(1, q.v.call);
        put(2, QString::number(q.freqHz / 1000.0, 'f', 1));
        put(3, q.v.rstS);
        put(4, q.v.serialS > 0 ? QString::number(q.v.serialS) : QString());
        put(5, q.v.rstR);
        put(6, q.v.serialR);
        put(7, q.v.exch1);
        put(8, q.v.exch2);
        put(9, q.v.exch3);
        put(10, QString::number(q.points));
        put(11, reported.contains(q.id) ? QStringLiteral("✓ sent")
                                        : QString());
    }
}

qint64 ContestWindow::selectedQsoId() const {
    const int r = table_->currentRow();
    if (r < 0) return -1;
    const QTableWidgetItem* it = table_->item(r, 0);
    return it ? it->data(Qt::UserRole).toLongLong() : -1;
}

void ContestWindow::editSelected() {
    const qint64 id = selectedQsoId();
    if (id < 0 || !def_) return;
    if (db_->qsoReported(id)) {
        QMessageBox::warning(
            this, "Frozen QSO",
            "This QSO has gone out in a QTC block — the receiving "
            "station holds a copy of exactly what was logged, so it can "
            "no longer be edited.");
        return;
    }
    ContestQso q;
    for (const ContestQso& c : qsos_)
        if (c.id == id) { q = c; break; }
    if (q.id < 0) return;

    QDialog d(this);
    d.setWindowTitle(QString("Edit %1").arg(q.v.call));
    auto* form = new QFormLayout(&d);
    auto* call = new QLineEdit(q.v.call, &d);
    form->addRow("Call", call);
    QLineEdit *rstS = nullptr, *rstR = nullptr, *serR = nullptr,
              *ex1 = nullptr, *ex2 = nullptr, *ex3 = nullptr;
    if (def_->hasRst) {
        rstS = new QLineEdit(q.v.rstS, &d);
        rstR = new QLineEdit(q.v.rstR, &d);
        form->addRow("RST sent", rstS);
        form->addRow("RST rcvd", rstR);
    }
    for (const ExchFieldDef& fd : def_->fields) {
        switch (fd.col) {
            case ExchCol::RstR: break;             // covered above
            case ExchCol::SerialR:
                serR = new QLineEdit(q.v.serialR, &d);
                form->addRow(fd.label, serR);
                break;
            case ExchCol::Exch1:
                ex1 = new QLineEdit(q.v.exch1, &d);
                form->addRow(fd.label, ex1);
                break;
            case ExchCol::Exch2:
                ex2 = new QLineEdit(q.v.exch2, &d);
                form->addRow(fd.label, ex2);
                break;
            case ExchCol::Exch3:
                ex3 = new QLineEdit(q.v.exch3, &d);
                form->addRow(fd.label, ex3);
                break;
        }
    }
    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &d);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;

    if (!loggableCall(call->text())) {
        status_->setText("edit refused: that call is not loggable");
        return;
    }
    q.v.call = call->text().trimmed().toUpper();
    if (rstS) q.v.rstS = rstS->text().trimmed();
    if (rstR) q.v.rstR = rstR->text().trimmed();
    if (serR) q.v.serialR = serR->text().trimmed();
    if (ex1) q.v.exch1 = ex1->text().trimmed();
    if (ex2) q.v.exch2 = ex2->text().trimmed();
    if (ex3) q.v.exch3 = ex3->text().trimmed();
    // Points follow the edit (a corrected call can change the country).
    CtyInfo ci;
    const bool ok = cty_ && cty_->info(normalizeForCty(q.v.call), ci);
    q.points = def_->points ? def_->points(q.v, ci, ok, ctx_) : 0;
    if (!db_->updateQso(q)) {
        status_->setText("edit refused (reported in a QTC, or database "
                         "error)");
        return;
    }
    trace(QString("MGR EDIT id=%1 -> %2").arg(id).arg(q.v.call));
    status_->setText(QString("edited %1").arg(q.v.call));
}

void ContestWindow::deleteSelected() {
    const qint64 id = selectedQsoId();
    if (id < 0) return;
    if (db_->qsoReported(id)) {
        QMessageBox::warning(
            this, "Frozen QSO",
            "This QSO has gone out in a QTC block and cannot be deleted "
            "— the stored traffic would disagree with the log.");
        return;
    }
    QString call;
    for (const ContestQso& c : qsos_)
        if (c.id == id) { call = c.v.call; break; }
    if (QMessageBox::question(
            this, "Delete QSO",
            QString("Delete %1 from the log?").arg(call))
        != QMessageBox::Yes)
        return;
    if (!db_->deleteQso(id)) {
        status_->setText("delete refused (database)");
        return;
    }
    trace(QString("MGR DELETE id=%1 %2").arg(id).arg(call));
    status_->setText(QString("deleted %1").arg(call));
}

void ContestWindow::showSummary() {
    if (contestId_ < 0 || !def_) return;
    const QList<CQsoValues> vals = db_->qsoValues(contestId_);
    const int qtcN = def_->hasQtc ? db_->qtcCount(contestId_) : 0;
    const ScoreBreakdown sb = computeScore(*def_, vals, cty_, ctx_, qtcN);
    QString text = summaryText(*def_, row_, qsos_, sb);
    const QStringList w = logChecks(*def_, row_, qsos_);
    if (!w.isEmpty()) text += "\nLog checks:\n  • " + w.join("\n  • ") + "\n";
    auto* d = new QDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle("Contest summary — " + row_.title);
    auto* lay = new QVBoxLayout(d);
    auto* ed = new QPlainTextEdit(text, d);
    ed->setReadOnly(true);
    ed->setFont(QFont("DejaVu Sans Mono"));
    ed->setMinimumSize(560, 480);
    lay->addWidget(ed);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, d);
    auto* copy = bb->addButton("Copy", QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, d, [text] { QApplication::clipboard()->setText(text); });
    connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
    lay->addWidget(bb);
    d->show();
}

void ContestWindow::editCategories() {
    if (contestId_ < 0 || !def_) return;
    QDialog d(this);
    d.setWindowTitle("Cabrillo categories — " + row_.title);
    auto* form = new QFormLayout(&d);
    form->setLabelAlignment(Qt::AlignRight);
    const auto combo = [&](const char* label, const QStringList& items,
                           const QString& cur, bool editable = false) {
        auto* c = new QComboBox(&d);
        c->addItems(items);
        c->setEditable(editable);
        const int i = c->findText(cur);
        if (i >= 0) c->setCurrentIndex(i);
        else if (editable) c->setEditText(cur);
        else if (!cur.isEmpty()) { c->addItem(cur); c->setCurrentIndex(c->count() - 1); }
        form->addRow(label, c);
        return c;
    };
    auto* op = combo("OPERATOR", {"SINGLE-OP", "MULTI-OP", "CHECKLOG"},
                     row_.catOp);
    auto* as = combo("ASSISTED", {"ASSISTED", "NON-ASSISTED"},
                     row_.catAssisted);
    auto* bd = combo("BAND", {"ALL", "160M", "80M", "40M", "20M", "15M",
                              "10M"}, row_.catBand);
    auto* pw = combo("POWER", {"HIGH", "LOW", "QRP"}, row_.catPower);
    auto* md = combo("MODE", {"CW", "SSB", "RTTY", "DIGI", "MIXED", "FM"},
                     row_.catMode);
    auto* tx = combo("TRANSMITTER", {"ONE", "TWO", "LIMITED", "UNLIMITED",
                                     "SWL"}, row_.catTx);
    auto* st = combo("STATION", {"FIXED", "MOBILE", "PORTABLE", "ROVER",
                                 "EXPEDITION", "HQ", "SCHOOL"},
                     row_.catStation);
    auto* ov = combo("OVERLAY", {"", "CLASSIC", "ROOKIE", "TB-WIRES",
                                 "YOUTH"}, row_.catOverlay, true);
    ov->setToolTip("Blank = no CATEGORY-OVERLAY line");
    auto* club = new QLineEdit(row_.club, &d);
    club->setPlaceholderText("blank = no CLUB line");
    form->addRow("CLUB", club);
    auto* soap = new QPlainTextEdit(row_.soapbox, &d);
    soap->setPlaceholderText("blank = no SOAPBOX line; one line each");
    soap->setFixedHeight(70);
    form->addRow("SOAPBOX", soap);
    auto* bb = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(bb);
    if (d.exec() != QDialog::Accepted) return;

    row_.catOp = op->currentText().trimmed().toUpper();
    row_.catAssisted = as->currentText().trimmed().toUpper();
    row_.catBand = bd->currentText().trimmed().toUpper();
    row_.catPower = pw->currentText().trimmed().toUpper();
    row_.catMode = md->currentText().trimmed().toUpper();
    row_.catTx = tx->currentText().trimmed().toUpper();
    row_.catStation = st->currentText().trimmed().toUpper();
    row_.catOverlay = ov->currentText().trimmed().toUpper();
    row_.club = club->text().trimmed();
    row_.soapbox = soap->toPlainText().trimmed();
    row_.catConfirmed = true;
    db_->updateContest(row_);
    // Remembered for newContest(): the next event starts from this setup.
    QSettings s;
    s.setValue("contest/lastCatOp", row_.catOp);
    s.setValue("contest/lastCatAssisted", row_.catAssisted);
    s.setValue("contest/lastCatPower", row_.catPower);
    s.setValue("contest/lastCatTx", row_.catTx);
    s.setValue("contest/lastCatStation", row_.catStation);
    s.setValue("contest/lastClub", row_.club);
    trace(QString("CAT %1 %2 %3 %4 %5 %6 %7")
              .arg(row_.catOp, row_.catAssisted, row_.catBand,
                   row_.catPower, row_.catMode, row_.catTx, row_.catStation));
    refreshAll();
}

void ContestWindow::exportCabrillo() {
    if (contestId_ < 0 || !def_) return;
    // A contest that sends a fixed exchange token (age, zone, section)
    // MUST have one — a blank ships a hole the log checker rejects on
    // every line (All Asian, the EN-on-every-QSO lesson). The round-
    // trip check can't see this: an empty sent exchange is internally
    // consistent, just externally wrong.
    if (def_->cabExch.contains(QStringLiteral("exch"))
        && row_.sentExch.trimmed().isEmpty()) {
        QMessageBox::warning(
            this, "Sent exchange is empty",
            QString("%1 sends a fixed exchange every QSO (your age, "
                    "zone, section…) and it is BLANK — the Cabrillo "
                    "would ship an empty field on every line and be "
                    "rejected.\n\nFill \"sent exch\" at the top, then "
                    "export again.").arg(def_->title));
        if (openExchEdit_) openExchEdit_->setFocus();
        return;
    }
    // Categories never looked at for this contest: the header would be
    // whatever the struct defaults say (CQ WW SSB 2026 shipped LOW /
    // ASSISTED unseen and the robot bounced it). Offer the dialog first.
    if (!row_.catConfirmed) {
        const auto b = QMessageBox::question(
            this, "Categories not confirmed",
            QString("The Cabrillo categories for this contest were never "
                    "confirmed:\n\n%1 / %2 / %3 / %4 / %5 / %6%7\n\n"
                    "Open Categories… now?")
                .arg(row_.catOp, row_.catAssisted, row_.catBand,
                     row_.catPower, row_.catTx, row_.catStation,
                     row_.catOverlay.isEmpty() ? QString()
                                               : " / " + row_.catOverlay),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::Yes);
        if (b == QMessageBox::Cancel) return;
        if (b == QMessageBox::Yes) {
            editCategories();
            if (!row_.catConfirmed) return;      // dialog cancelled
        }
    }
    // Plain-language log checks first (holes, no frequency, future
    // timestamps, single-op time over the limit, multi-single band
    // changes). Warnings only — the operator decides.
    {
        const QStringList w = logChecks(*def_, row_, qsos_);
        if (!w.isEmpty()) {
            const auto b = QMessageBox::question(
                this, "Log checks",
                "Before the Cabrillo goes out:\n\n• " + w.join("\n• ")
                    + "\n\nWrite the Cabrillo anyway?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (b != QMessageBox::Yes) return;
        }
    }
    CabrilloStation st;
    QSettings s;
    st.call = s.value("station/callsign", "").toString().toUpper();
    st.gridLocator = s.value("station/grid", "").toString();
    st.location = s.value("contest/location", "").toString();
    // Setup → Station is the home (2026-10-27); the old contest/* keys
    // nobody could ever set are read as a fallback.
    st.name = s.value("station/opname",
                      s.value("contest/opname").toString()).toString();
    st.address = s.value("station/address",
                         s.value("contest/address").toString()).toString();
    st.city = s.value("station/addrCity").toString();
    st.state = s.value("station/addrState").toString();
    st.postal = s.value("station/addrPostal").toString();
    st.country = s.value("station/addrCountry").toString();
    // Sponsors' log robots require an EMAIL: header (All Asian rejects
    // without it). station/email is the home; club/email is the old
    // ClubLog address as a fallback so this is never blank again.
    st.email = s.value("station/email",
                       s.value("club/email").toString()).toString();
    st.club = row_.club;
    const QList<ContestDb::QtcRow> qtcs = db_->qtcRows(contestId_);
    const QString text =
        Cabrillo::build(*def_, row_, qsos_, qtcs, st, cty_, ctx_);
    QString err;
    if (!Cabrillo::selfCheck(text, *def_, row_, qsos_, qtcs, &err)) {
        // A file that fails its own parse-back must never reach a
        // sponsor's robot. Refuse loudly.
        QMessageBox::critical(
            this, "Cabrillo verification FAILED",
            "The generated file does not match the log — NOT written.\n\n"
                + err);
        trace("CABRILLO SELF-CHECK FAILED: " + err);
        return;
    }
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
        + "/Cabrillo";
    QDir().mkpath(dir);
    const QString path = dir + "/" + st.call + "-" + def_->cabrilloName
        + ".log";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, "Cabrillo", "Cannot write " + path);
        return;
    }
    f.write(text.toUtf8());
    f.close();
    trace(QString("CABRILLO %1 (%2 QSOs, %3 QTC, verified)")
              .arg(path)
              .arg(qsos_.size())
              .arg(qtcs.size()));
    status_->setText(
        QString("Cabrillo written and verified — %1 QSOs, %2 QTC → %3")
            .arg(qsos_.size())
            .arg(qtcs.size())
            .arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// One contest QSO as an everyday-log row: identity fields ride over,
// the exchange folds into name/comment, and cty stamps country/zones.
Qso ContestWindow::logbookQso(const ContestQso& q) const {
    Qso o;
    o.tsUtc = q.tsUtc;
    o.call = q.v.call;
    o.band = q.v.band;
    o.mode = q.v.mode;
    o.freqHz = q.freqHz;
    o.rstS = q.v.rstS.isEmpty() ? rstDefault(q.v.mode) : q.v.rstS;
    o.rstR = q.v.rstR.isEmpty() ? rstDefault(q.v.mode) : q.v.rstR;
    if (def_)
        for (int i = 0; i < def_->fields.size(); ++i)
            if (def_->fields[i].label.contains("NAME")) {
                switch (def_->fields[i].col) {
                    case ExchCol::Exch1: o.name = q.v.exch1; break;
                    case ExchCol::Exch2: o.name = q.v.exch2; break;
                    case ExchCol::Exch3: o.name = q.v.exch3; break;
                    default: break;
                }
            }
    CtyInfo ci;
    if (cty_ && cty_->info(normalizeForCty(q.v.call), ci)) {
        o.country = ci.country;
        o.cqz = ci.cq;
        o.ituz = ci.itu;
    }
    // Full exchange in the comment, BOTH ways — the contest-wide sent
    // exchange (age/zone/section) rides here too, so a pushed record
    // isn't half a QSO. What went out: my serial and the fixed
    // sent-exch; what came back: his serial + exch fields.
    QStringList s;
    if (q.v.serialS > 0) s << QString::number(q.v.serialS);
    if (!row_.sentExch.trimmed().isEmpty()) s << row_.sentExch.trimmed();
    QStringList r;
    if (!q.v.serialR.isEmpty()) r << q.v.serialR;
    for (const QString& e : {q.v.exch1, q.v.exch2, q.v.exch3})
        if (!e.isEmpty()) r << e;
    QStringList ex;
    if (!s.isEmpty()) ex << "sent " + s.join(' ');
    if (!r.isEmpty()) ex << "rcvd " + r.join(' ');
    o.comment = (def_ ? def_->cabrilloName : row_.defId)
        + (ex.isEmpty() ? QString() : " · " + ex.join(" · "));
    return o;
}

void ContestWindow::pushToLogbook() {
    if (contestId_ < 0 || !def_) return;
    if (!logDb_) {
        status_->setText("no station logbook attached");
        return;
    }
    int pushed = 0, skipped = 0, failed = 0;
    QList<qint64> ids;                   // for the cqrlog mirror
    for (const ContestQso& q : qsos_) {
        const Qso o = logbookQso(q);
        if (logDb_->hasNearDuplicate(o)) {
            ++skipped;                   // already there — never double
            continue;
        }
        const qint64 id = logDb_->addQso(o);
        if (id > 0) { ++pushed; ids << id; }
        else ++failed;
    }
    trace(QString("PUSH->LOGBOOK %1: %2 pushed, %3 already there, "
                  "%4 failed")
              .arg(row_.title)
              .arg(pushed)
              .arg(skipped)
              .arg(failed));
    status_->setText(
        QString("→ logbook: %1 pushed, %2 already there%3%4")
            .arg(pushed)
            .arg(skipped)
            .arg(failed ? QString(", %1 FAILED").arg(failed)
                        : QString())
            .arg(pushed ? QStringLiteral(" · online logs sweeping…")
                        : QString()));
    if (pushed) emit pushedToLogbook(ids);
}

void ContestWindow::exportAdif() {
    if (contestId_ < 0 || !def_) return;
    const QString suggested =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
        + "/" + QSettings().value("station/callsign", "").toString()
        + "-" + def_->cabrilloName + ".adi";
    const QString path = QFileDialog::getSaveFileName(
        this, "Export contest ADIF", suggested, "ADIF (*.adi *.adif)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        status_->setText("cannot write " + path);
        return;
    }
    f.write(Adif::fileHeader().toUtf8());
    for (const ContestQso& q : qsos_)
        f.write(Adif::writeRecord(LogDb::toAdif(logbookQso(q))).toUtf8());
    f.close();
    trace(QString("ADIF %1 (%2 QSOs)").arg(path).arg(qsos_.size()));
    status_->setText(QString("ADIF written — %1 QSOs → %2")
                         .arg(qsos_.size())
                         .arg(path));
}

void ContestWindow::closeEvent(QCloseEvent* e) {
    QSettings().setValue("contest/mgrGeom", saveGeometry());
    QDialog::closeEvent(e);
}

void ContestWindow::trace(const QString& line) {
    static const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/contest-trace.log";
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    f.write((QDateTime::currentDateTimeUtc().toString(
                 "yyyy-MM-dd HH:mm:ss ")
             + line + "\n")
                .toUtf8());
}

} // namespace ttc
