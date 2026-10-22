// SPDX-License-Identifier: GPL-2.0-or-later
// Contest UI smoke harness, offscreen: the MANAGER creates a contest
// through its widgets, the DECK logs QSOs through the ESM flow, the QTC
// dialog loads/sends/confirms a block against a seeded WAE log, and
// screenshots come out for eyeballing. No keyer, no radio, no network.
//   QT_QPA_PLATFORM=offscreen ./contestuitest [out.png]
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <cstdio>

#include "contest/ContestDb.h"
#include "contest/ContestDeck.h"
#include "contest/ContestWindow.h"
#include "contest/QtcDialog.h"
#include "ui/SpotTableWindow.h"
#include "util/CtyLookup.h"

using namespace ttc;

static int fails = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) {                                                        \
            std::printf("ok    %s\n", what);                               \
        } else {                                                           \
            std::printf("FAIL  %s\n", what);                               \
            ++fails;                                                       \
        }                                                                  \
    } while (0)

static QPushButton* buttonWithText(QWidget* w, const QString& t) {
    for (QPushButton* b : w->findChildren<QPushButton*>())
        if (b->text() == t) return b;
    return nullptr;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/cfg").toUtf8());

    CtyLookup cty;
    bool ctyOk = false;
    for (const char* p : {"resources/cty.dat", "../resources/cty.dat"})
        if (cty.load(QString::fromLatin1(p))) { ctyOk = true; break; }
    CHECK(ctyOk, "cty.dat loads");

    ContestDb db;
    CHECK(db.open(tmp.path() + "/contest.sqlite"), "contest.db opens");

    // ---- MANAGER: create a CWT through the widgets ----------------------
    ContestWindow w(&db, &cty);
    qint64 openedId = -1;
    QObject::connect(&w, &ContestWindow::contestOpened,
                     [&openedId](qint64 id) { openedId = id; });
    auto* defPick = w.findChild<QComboBox*>("defPick");
    CHECK(defPick, "mgr: contest picker exists");
    if (!defPick) return 1;
    defPick->setCurrentIndex(defPick->findData("CW-OPS"));
    auto* sentExch = w.findChild<QLineEdit*>("sentExch");
    CHECK(sentExch && sentExch->text() == "Jon MI",
          "mgr: sent-exchange default follows the picker");
    QPushButton* start = buttonWithText(&w, "Start");
    CHECK(start, "mgr: Start button exists");
    start->click();
    CHECK(openedId > 0 && db.contests().size() == 1,
          "mgr: Start creates and opens the contest");
    const qint64 cid = openedId;

    // ---- DECK: the ESM flow logs QSOs -----------------------------------
    ContestDeck deck(&db, &cty, nullptr, nullptr);
    CHECK(deck.openContestId(cid), "deck opens the contest");
    deck.setRig(14032000, "CW");
    deck.setMasterScp({"N3JT", "N3JTX", "K6RB"});
    deck.show();

    // The deck must never force the main window past the width budget
    // (losing the maximize button is the symptom). Its minimum stays
    // well under the toolbar's 1848 floor, and a long score can't drag
    // it — the score is on its own line and the labels are shrinkable.
    deck.ensurePolished();
    const int deckEmpty = deck.minimumSizeHint().width();
    for (QLabel* l : deck.findChildren<QLabel*>())
        if (l->text().startsWith("· ")) {
            l->setText("· 843 Q · 2107 pts · 187 mult · 394,009 · "
                       "10q 156/h · 10m 132/h · 892 pt/h");
            break;
        }
    deck.ensurePolished();
    CHECK(deck.minimumSizeHint().width() <= deckEmpty
              && deckEmpty < 1848,
          "deck: width under budget and immune to score length");
    auto* dCall = deck.findChild<QLineEdit*>("entryCall");
    auto* dEx0 = deck.findChild<QLineEdit*>("exchEdit0");
    auto* dEx1 = deck.findChild<QLineEdit*>("exchEdit1");
    CHECK(dCall && dEx0 && dEx1, "deck: entry fields exist");
    if (!dCall || !dEx0 || !dEx1) return 1;

    deck.prefillCall("N3JT");
    dEx0->setText("JIM");
    dEx1->setText("1");
    QMetaObject::invokeMethod(dCall, "returnPressed");  // answer beat
    // The answer keys his call and the exchange as ONE spaced string —
    // separate sends butted them together with no word gap (N2IC5NN4).
    {
        QString keyed;
        for (QLabel* l : deck.findChildren<QLabel*>())
            if (l->text().startsWith("→ ")) keyed = l->text();
        CHECK(keyed.contains("N3JT ") && keyed.contains("Jon MI"),
              "deck: ESM answer keys call + exch spaced, not run together");
    }
    QMetaObject::invokeMethod(dCall, "returnPressed");  // TU + log
    deck.prefillCall("K6RB");
    dEx0->setText("ROB");
    dEx1->setText("3");
    QMetaObject::invokeMethod(dCall, "returnPressed");
    QMetaObject::invokeMethod(dCall, "returnPressed");
    CHECK(db.qsos(cid).size() == 2, "deck: ESM logs two QSOs");
    CHECK(deck.classifySpot("N3JT") == 'W' &&
              deck.classifySpot("W9ZZZ") == 'M',
          "deck: classification tracks the log");

    // Manager's grid follows the deck through the changed() signal.
    auto* grid = w.findChild<QTableWidget*>();
    CHECK(grid && grid->rowCount() == 2, "mgr: log grid mirrors the deck");

    // Delete-contest guard: an empty instance goes, a logged one stays.
    {
        ContestRow empty;
        empty.defId = "CW-OPS";
        empty.title = "empty practice";
        empty.startUtc = QDateTime::currentDateTimeUtc();
        const qint64 eid = db.createContest(empty);
        CHECK(db.deleteContest(eid),
              "delete: an empty contest is removed");
        CHECK(db.contest(eid).id < 0, "delete: it's gone");
        CHECK(!db.deleteContest(cid),
              "delete: a contest with QSOs is refused");
        CHECK(db.contest(cid).id == cid, "delete: the logged one stays");
    }

    // Push to the everyday logbook: rows land once; a second press
    // skips every one of them (near-dupe guard).
    {
        LogDb ldb;
        CHECK(ldb.open(tmp.path() + "/logbook.sqlite"),
              "push: logbook opens");
        ContestWindow wp(&db, &cty, &ldb);
        wp.openContestId(cid);
        int swept = 0;
        QObject::connect(&wp, &ContestWindow::pushedToLogbook,
                         [&swept](int n) { swept = n; });
        QPushButton* push = buttonWithText(&wp, "→ Logbook");
        CHECK(push, "push: button exists");
        push->click();
        CHECK(ldb.count() == 2, "push: both QSOs land in the logbook");
        CHECK(swept == 2, "push: the online-log sweep gets rung");
        swept = 0;
        push->click();
        CHECK(ldb.count() == 2 && swept == 0,
              "push: second press doubles nothing, rings nothing");
        const Qso q0 = ldb.qso(1);
        CHECK(q0.comment.contains("CW-OPS"),
              "push: comment names the contest");
        CHECK(q0.comment.contains("sent") && q0.comment.contains("rcvd"),
              "push: comment carries BOTH sent and received exchange");
    }

    // ---- QTC: seeded WAE contest, full load/send/confirm ----------------
    ContestRow wae;
    wae.defId = "DARC-WAEDC-CW";
    wae.title = "WAE QTC ui";
    wae.startUtc = QDateTime::currentDateTimeUtc().addSecs(-7200);
    const qint64 wid = db.createContest(wae);
    QList<qint64> ids;
    for (int i = 0; i < 12; ++i) {
        ContestQso q;
        q.contestId = wid;
        q.tsUtc = wae.startUtc.addSecs(300 * (i + 1));
        q.freqHz = 7024000;
        q.v.call = i == 2 ? "DL8WPX" : QString("DL%1AA").arg(i);
        q.v.band = "40M";
        q.v.mode = "CW";
        q.v.rstS = "599";
        q.v.rstR = "599";
        q.v.serialR = QString::number(100 + i);
        q.v.serialS = i + 1;
        ids << db.addQso(q);
    }
    QStringList keyed;
    QtcDialog qtc(&db, [&keyed](const QString& t) { keyed << t; },
                  [] {});
    qtc.openFor(wid);
    qtc.followCall("DL8WPX");
    QPushButton* load = buttonWithText(&qtc, "Load");
    CHECK(load, "qtcui: Load button exists");
    load->click();
    auto* qtable = qtc.findChild<QTableWidget*>();
    CHECK(qtable && qtable->rowCount() == 10,
          "qtcui: ten rows loaded, receiver's own QSO excluded");
    buttonWithText(&qtc, "Send all")->click();
    CHECK(keyed.size() == 11 && keyed.first() == "QTC 1/10",
          "qtcui: Send all keys the header + ten lines");
    CHECK(keyed[1].contains("DL0AA") && keyed[1].contains("100"),
          "qtcui: line format time-call-serial");
    buttonWithText(&qtc, "Confirm && log block")->click();
    CHECK(db.qtcCount(wid) == 10, "qtcui: confirm lands the block");
    CHECK(qtable->rowCount() == 0, "qtcui: table clears after confirm");
    load->click();
    CHECK(qtable->rowCount() == 0,
          "qtcui: station at the 10-QTC cap loads nothing");

    // ---- the arrow walk: empty box, spot landing, typing ----------------
    {
        int walked = 0;
        QObject::connect(&deck, &ContestDeck::walkSpots,
                         [&walked](int) { ++walked; });
        auto* wCall = deck.findChild<QLineEdit*>("entryCall");
        wCall->clear();
        wCall->setFocus();
        QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &right);
        CHECK(walked == 1, "walk: empty call box, → walks");
        // A landing fills the box — the arrows must KEEP walking.
        deck.prefillCall("DL8WPX");
        QKeyEvent right2(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &right2);
        CHECK(walked == 2, "walk: rolls onward from an untouched landing");
        // The first typed character hands the arrows back to the cursor.
        QKeyEvent typeD(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, "D");
        QCoreApplication::sendEvent(wCall, &typeD);
        QKeyEvent left(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &left);
        CHECK(walked == 2, "walk: typing reclaims the arrows for editing");
        // ↑/↓ = keying speed, even with text in the box (CW contest).
        auto* wpmSpin = deck.findChild<QSpinBox*>("wpmSpin");
        const int wpm0 = wpmSpin->value();
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &up);
        QKeyEvent up2(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &up2);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QCoreApplication::sendEvent(wCall, &down);
        CHECK(wpmSpin->value() == wpm0 + 1,
              "speed: ↑↑↓ from the call box nets +1 wpm");
        wCall->clear();
    }

    // ---- phone contest: F-keys play voice slots -------------------------
    {
        ContestRow ssb;
        ssb.defId = "CQ-WW-SSB";
        ssb.title = "CQWW SSB ui";
        ssb.startUtc = QDateTime::currentDateTimeUtc();
        ssb.sentExch = "4";
        const qint64 sid = db.createContest(ssb);
        ContestDeck vdeck(&db, &cty, nullptr, nullptr);
        CHECK(vdeck.openContestId(sid), "voiceui: SSB contest opens");
        vdeck.setRig(14250000, "SSB");
        vdeck.show();
        QList<int> played;
        vdeck.setVoiceKeyer(
            [&played](int s) {
                played << s;
                return true;
            },
            [] {});
        QPushButton* f1 = nullptr;
        for (QPushButton* b : vdeck.findChildren<QPushButton*>())
            if (b->text().startsWith("F1\n")) f1 = b;
        CHECK(f1 && f1->isEnabled(), "voiceui: F1 is the CQ voice key");
        f1->click();
        CHECK(played == QList<int>{0},
              "voiceui: F1 plays VK1 (slot 0) instead of keying CW");
        // ESM on phone (restored by the operator once he knew the
        // toggle): Enter on an empty box plays the CQ message.
        auto* vCall = vdeck.findChild<QLineEdit*>("entryCall");
        QMetaObject::invokeMethod(vCall, "returnPressed");
        CHECK(played.size() == 2 && played.last() == 0,
              "voiceui: ESM Enter plays the CQ message");
        QPushButton* f2 = nullptr;
        for (QPushButton* b : vdeck.findChildren<QPushButton*>())
            if (b->text().startsWith("F2\n")) f2 = b;
        CHECK(f2 && !f2->isEnabled(),
              "voiceui: F2 (his call) is blank on phone — you speak it");
        // Phone reports are two digits (the AA Phone 599 live-find).
        auto* snt = vdeck.findChild<QLineEdit*>("sntEdit");
        auto* rcv = vdeck.findChild<QLineEdit*>("exchEdit0");
        CHECK(snt && snt->text() == "59" && rcv && rcv->text() == "59",
              "voiceui: SSB contest presets 59, not 599");

        // The full phone ESM flow: answering Enter plays the exchange,
        // closing Enter plays TU and logs.
        auto* vCall2 = vdeck.findChild<QLineEdit*>("entryCall");
        vdeck.prefillCall("JI2MED");
        rcv->setText("59");
        vdeck.findChild<QLineEdit*>("exchEdit1")->setText("45");
        QMetaObject::invokeMethod(vCall2, "returnPressed");  // answer
        CHECK(played.size() == 3 && played.last() == 1,
              "voiceui: answering Enter plays the exchange (VK2)");
        QMetaObject::invokeMethod(vCall2, "returnPressed");  // TU + log
        CHECK(played.size() == 4 && played.last() == 2
                  && db.qsos(sid).size() == 1
                  && db.qsos(sid).last().v.call == "JI2MED"
                  && vCall2->text().isEmpty(),
              "voiceui: closing Enter plays TU and logs");

        // Dead slots never advance the beats: Enter retries the answer
        // forever instead of wandering into a state where it means
        // nothing (the original JI2MED failure mode).
        vdeck.setVoiceKeyer([](int) { return false; }, [] {});
        vdeck.prefillCall("JA3AAA");
        rcv->setText("59");
        vdeck.findChild<QLineEdit*>("exchEdit1")->setText("30");
        QMetaObject::invokeMethod(vCall2, "returnPressed");
        QMetaObject::invokeMethod(vCall2, "returnPressed");
        CHECK(db.qsos(sid).size() == 1 && !vCall2->text().isEmpty(),
              "voiceui: dead slots never advance the beats");

        // ESM off: Enter is a plain log — no keying, box wipes.
        QPushButton* esm = buttonWithText(&vdeck, "ESM");
        CHECK(esm && esm->isChecked(), "voiceui: ESM button exists, on");
        esm->click();
        QMetaObject::invokeMethod(vCall2, "returnPressed");
        CHECK(db.qsos(sid).size() == 2
                  && db.qsos(sid).last().v.call == "JA3AAA"
                  && vCall2->text().isEmpty(),
              "voiceui: ESM off, Enter just logs and wipes");

        // Space skips the preset RST and lands on the field that needs
        // copy — the JI2MED trap: the age went into the 59 box.
        vdeck.prefillCall("JA9XYZ");
        QKeyEvent sp(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
                     " ");
        QCoreApplication::sendEvent(vCall2, &sp);
        auto* age = vdeck.findChild<QLineEdit*>("exchEdit1");
        CHECK(vdeck.focusWidget() == age,
              "voiceui: Space lands on AGE, not the 59 box");

        // The bust alarm: a loggable call that maps to no country.
        vdeck.prefillCall("J12MED");
        bool alarm = false;
        for (QLabel* l : vdeck.findChildren<QLabel*>())
            if (l->text().contains("no country")) alarm = true;
        CHECK(alarm, "voiceui: J12MED raises the no-country alarm");

        // And the double-check gate: first Enter refuses, an unedited
        // second Enter logs on the operator's authority.
        rcv->setText("59");
        vdeck.findChild<QLineEdit*>("exchEdit1")->setText("45");
        const int qsoCount = int(db.qsos(sid).size());
        QMetaObject::invokeMethod(vCall2, "returnPressed");
        CHECK(int(db.qsos(sid).size()) == qsoCount,
              "voiceui: no-country call refused on the first Enter");
        QMetaObject::invokeMethod(vCall2, "returnPressed");
        CHECK(int(db.qsos(sid).size()) == qsoCount + 1
                  && db.qsos(sid).last().v.call == "J12MED",
              "voiceui: unedited second Enter logs it anyway");

        // Phone deck: no CW panes.
        bool readVisible = false;
        for (QWidget* w : vdeck.findChildren<QWidget*>())
            if (w->objectName().isEmpty() && w->isVisible()
                && qobject_cast<QPlainTextEdit*>(w))
                readVisible = true;
        CHECK(!readVisible, "voiceui: CW READ pane hidden on phone");
    }

    // ---- abandon-on-QSY: grabbed calls clear, typed calls park ----------
    {
        int parks = 0;
        QObject::connect(&deck, &ContestDeck::callParked,
                         [&parks](const QString&, qint64) { ++parks; });
        auto* aCall = deck.findChild<QLineEdit*>("entryCall");
        // Grabbed from a spot at 14032, then rolled away: box clears,
        // nothing parks (the spot is already on the map).
        deck.setRig(14032000, "CW");
        deck.prefillCall("K6TK", 14032000);
        deck.setRig(14040000, "CW");
        CHECK(aCall->text().isEmpty() && parks == 0,
              "qsy: a grabbed call clears on roll-away, no park");
        // Typed at 14040 (real key events set the anchor), rolled away:
        // parks once, box clears.
        for (const char* k : {"K", "6", "T", "K"}) {
            QKeyEvent ev(QEvent::KeyPress,
                         k[0] == '6' ? Qt::Key_6 : Qt::Key_K,
                         Qt::NoModifier, QString::fromLatin1(k));
            QCoreApplication::sendEvent(aCall, &ev);
        }
        CHECK(aCall->text() == "K6TK", "qsy: typing lands in the box");
        deck.setRig(14060000, "CW");
        CHECK(parks == 1 && aCall->text().isEmpty(),
              "qsy: a typed call parks once and clears");
    }

    // ---- spot table: contest view + per-band summary --------------------
    {
        SpotTableWindow tw(nullptr, nullptr);
        tw.setContestMode(true);
        QVector<SpotLabel> sl;
        const auto mk = [](const char* c, qint64 hz, char cls) {
            SpotLabel l;
            l.call = c;
            l.hz = hz;
            l.atSecs = QDateTime::currentSecsSinceEpoch();
            l.contest = cls;
            return l;
        };
        sl << mk("DL2CC", 7004500, 'M') << mk("OK1RR", 7012000, 'N')
           << mk("F5IN", 7020000, 'W') << mk("G4AMT", 14022000, 'M')
           << mk("SM5CAK", 14030000, 'M') << mk("W1AW", 14040000, 'Z');
        tw.setSpots(sl);
        tw.show();                        // triggers rebuild
        QLabel* bandLine = nullptr;
        for (QLabel* l : tw.findChildren<QLabel*>())
            if (l->text().contains("40m")) bandLine = l;
        CHECK(bandLine && bandLine->text().contains("40m 2Q 1M")
                  && bandLine->text().contains("20m 2Q 2M ◀"),
              "table: per-band summary counts and marks the best band");
        bool countOk = false;
        for (QLabel* l : tw.findChildren<QLabel*>())
            if (l->text().contains("3 new mults")) countOk = true;
        CHECK(countOk, "table: mult total in the count line");
    }

    // ---- screenshots ----------------------------------------------------
    deck.prefillCall("N3J");
    deck.resize(1900, 270);
    QCoreApplication::processEvents();
    QPushButton* scpHit = nullptr;
    for (QPushButton* b : deck.findChildren<QPushButton*>())
        if (b->text() == "N3JT") scpHit = b;
    CHECK(scpHit && scpHit->isVisible(),
          "deck: super check offers N3JT for partial N3J");
    const QString png = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : tmp.path() + "/contestui.png";
    CHECK(deck.grab().save(png), "deck screenshot saved");
    CHECK(w.grab().save(png + ".mgr.png"), "manager screenshot saved");
    std::printf("      -> %s\n", qPrintable(png));

    std::printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
