// SPDX-License-Identifier: GPL-2.0-or-later
#include "contest/ContestWindow.h"

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
            QSettings().value("contest/location", "MI").toString(), this);
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
    CabrilloStation st;
    QSettings s;
    st.call = s.value("station/callsign", "").toString().toUpper();
    st.gridLocator = s.value("station/grid", "").toString();
    st.location = s.value("contest/location", "MI").toString();
    st.name = s.value("contest/opname").toString();
    st.address = s.value("contest/address").toString();
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
