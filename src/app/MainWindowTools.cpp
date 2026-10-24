// SPDX-License-Identifier: GPL-2.0-or-later
// The second-deck tool buttons, one self-contained builder per feature.
// Each runs once from the MainWindow constructor; new tools get a new
// function here (and a one-line call there) — buttons go on topLay2_,
// never the first deck, or the window minimum outgrows the screen.
#include "app/MainWindow.h"
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QAbstractItemView>
#include <QTextEdit>
#include <QComboBox>
#include <QKeyEvent>
#include <QCheckBox>
#include "cw/AudioCwSource.h"
#include "app/MainWindowInternal.h"
#include "app/Bands.h"
#include "cw/CwDecoder.h"
#include "cw/SkimmerEngine.h"
#include "cw/SkimServer.h"
#include "cw/SkimStft.h"
#include "cw/WinKeyer.h"
#include "net/FldigiClient.h"
#include "ui/DigiWindow.h"
#include "log/CqrlogSync.h"
#include "log/LogDb.h"
#include "log/WsjtxListener.h"
#include "log/QrzLookup.h"
#include "log/QslUploader.h"
#include "contest/ContestDb.h"
#include "contest/ContestDeck.h"
#include "contest/ContestWindow.h"
#include <algorithm>
#include "log/QsoContext.h"
#include "ui/LogWindow.h"
#include "ui/LogbookWindow.h"
#include "ui/SpotTableWindow.h"
#include "ui/SkimmerWindow.h"
#include "ui/SkimViewWindow.h"
#include "ui/VoiceEqDialog.h"
#include "ui/WinKeyerPanel.h"
#ifdef HAVE_WDSP
#include "sdr/SdrRx.h"
#include "ui/SdrRxDialog.h"
#include "audio/VoiceEq.h"
#include "audio/LiveVoice.h"
#include "ui/FreeDvWindow.h"
#include <QDesktopServices>
#ifdef HAVE_WHISPER
#include "asr/AsrEngine.h"
#include "ui/CopyAssistWindow.h"
#include "util/LogbookIndex.h"
#endif
#include <QJsonDocument>
#include <QJsonObject>
#endif
#include <QActionGroup>
#include <QUrl>
#include <QStandardPaths>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QApplication>
#include <QClipboard>
#include <QProcess>
#include <csignal>
#include <QDesktopServices>
#include <QSlider>
#include <QWidgetAction>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QUdpSocket>
#include <QWidgetAction>
#include <algorithm>

namespace ttc {

// The big tool windows (CW, band map, SKIM waterfall, DIGI) get parked on
// other monitors in a shack. On Windows an OWNED window has no taskbar
// button and rides its owner's z-order, and dragging one across monitors
// with different DPI recreates the native window — live-found 2026-08-31
// on the all-in-one + second display: both windows stranded invisible
// behind the maximized console ("they are gone"). Parentless top-levels
// are the Windows convention for multi-window ham software (N1MM-style:
// own taskbar entry, free stacking on any monitor). Linux keeps the
// transient-for parent the operator's WM already handles well. The pair
// below implements the split: parent choice + the quit/lifetime fixups a
// parentless window needs (MainWindow's destructor deletes them).
static QWidget* toolWinParent(QWidget* mainWin) {
#ifdef Q_OS_WIN
    Q_UNUSED(mainWin);
    return nullptr;
#else
    return mainWin;
#endif
}
static void adoptToolWindow(QWidget* w) {
#ifdef Q_OS_WIN
    // The console closing must still quit the app even while a parentless
    // tool window is open.
    w->setAttribute(Qt::WA_QuitOnClose, false);
#else
    Q_UNUSED(w);
#endif
}

// MASTER.SCP loader (shared logic): cqrlog ships the contest super-check
// list; the user's own copy wins if present.
static QSet<QString> loadMasterScp() {
    QSet<QString> out;
    for (const QString& p :
         {QDir::homePath() + "/.config/cqrlog/MASTER.SCP",
          QStringLiteral("/usr/share/cqrlog/ctyfiles/MASTER.SCP")}) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            out.insert(QString::fromLatin1(line).toUpper());
        }
        break;
    }
    return out;
}


void MainWindow::setupLogUi() {
    auto* logBtn = new QToolButton(topStrip_);
    logBtn->setText("LOG");
    logBtn->setFocusPolicy(Qt::NoFocus);
    logBtn->setStyleSheet(QString(kSideBtnStyle));
    logBtn->setCheckable(true);                    // lit while the window is up
    logBtn_ = logBtn;
    // Label stays "LOG" (operator call 2026-07-31: relabeling "messes up the
    // button"); the hover text carries the truth instead.
    logBtn->setToolTip("New QSO — the console's log entry window\n"
                       "(on Linux it also pops a fresh New QSO in cqrlog)\n"
                       "Right-click: logbook browser — search, edit, ADIF");
    topLay2_->addWidget(logBtn);
    logUdp_ = new QUdpSocket(this);
    // Pre-bind so the first datagram isn't lost (an unbound socket auto-binds
    // on first write and can drop that very first send).
    logUdp_->bind(QHostAddress::LocalHost, 0);
    // LOG -> the console's own New QSO window (SQLite station log), and the
    // same nudge to cqrlog as always — on the Linux box both stay in step,
    // on Windows the datagram lands on deaf ears by design (fire-and-forget).
    connect(logBtn, &QToolButton::clicked, this, [this] {
        // A second press closes the window (its × still does too).
        if (windowUp(logWin_)) {
            logWin_->close();
            return;
        }
        openLogWindow();
        logUdp_->writeDatagram("CQRNEWQSO", QHostAddress::LocalHost,
            quint16(QSettings().value("log/port", 2334).toInt()));
        // "Fresh QSO" has to mean fresh on BOTH sides: cqrlog's form goes
        // blank, so the console's DX box can't keep pointing at the
        // station just worked (operator had to clear it by hand).
        if (cwWin_) cwWin_->setHisCall(QString());
    });
    // Right-click = the logbook, directly — the operator's standing
    // gesture, not a menu. The contest manager has its own doors (the
    // CNTST toggle auto-opens it when no contest is loaded, and the
    // deck's "Contest log…" button), and the fldigi window lives at the
    // bottom of SDR ▾.
    logBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(logBtn, &QToolButton::customContextMenuRequested, this,
            [this](const QPoint&) { openLogbookWindow(); });
    // Spot click -> send the call (and POTA park/grid) to cqrlog's New QSO,
    // and pre-fill the console's own entry window when it's open.
    connect(pan_, &PanadapterWidget::spotClicked, this,
            [this](const QString& call, QChar kind, const QString& tg) {
                if (cwWin_) cwWin_->setHisCall(call);
                if (contestDeckVisible()) {
                    // The clicked spot's frequency anchors the call.
                    qint64 spotHz = 0;
                    for (const SpotLabel& s : shownSpots_)
                        if (s.call == call) { spotHz = s.hz; break; }
                    contestDeck_->prefillCall(call, spotHz);
                }
                QString park, grid;
                if (kind == QChar('P')) {
                    park = tg;
                    for (const Spot& s : potaClient_.spots())
                        if (s.call == call) { grid = s.grid; break; }
                }
                sendCqrLookup(call, park, grid);
            });

}

namespace {
// The rig's mode as the ADIF mode the logbook stores.
QString adifModeText(Mode m) {
    switch (m) {
        case Mode::CWU: case Mode::CWL: return QStringLiteral("CW");
        case Mode::USB: case Mode::LSB: return QStringLiteral("SSB");
        case Mode::AM:  return QStringLiteral("AM");
        case Mode::FM:  return QStringLiteral("FM");
    }
    return QStringLiteral("SSB");
}
} // namespace

// One record for both windows; the first of them to open makes it.
QsoContext* MainWindow::qsoContext() {
    if (!qsoCtx_) qsoCtx_ = new QsoContext(this);
    return qsoCtx_;
}

void MainWindow::openLogWindow(const QString& call, const QString& park,
                               const QString& grid) {
    if (!logWin_) {
        logWin_ = new LogWindow(logDb_, &logbook_, &cty_, &rotor_, qrz_,
                                toolWinParent(this));
        logWin_->setContext(qsoContext());
        WindowLight::bind(logBtn_, logWin_);
        adoptToolWindow(logWin_);
        // The compass rose follows whoever is in the window: country
        // center from cty.dat, sharpened to the real grid when WSJT-X or
        // a QRZ lookup provides one. (The 3D globe is the window's own
        // Globe button — the cqrlog browser globe, see BrowserGlobe.)
        connect(logWin_, &LogWindow::dxLocated, this,
                [this](double lat, double lon, const QString& call) {
                    rose_->pointAt(lat, lon, call);
                });
        // Clear (and the post-log wipe) rides to cqrlog too — its bridge
        // treats CQRCLEAR as the New QSO action, so both forms blank
        // together instead of cqrlog sitting on the last QSO's data.
        connect(logWin_, &LogWindow::cleared, this, [this] {
            if (logUdp_)
                logUdp_->writeDatagram(
                    "CQRCLEAR", QHostAddress::LocalHost,
                    quint16(QSettings().value("log/port", 2334).toInt()));
        });
        // Spot: hand the call up to the cluster at the dial frequency.
        connect(logWin_, &LogWindow::spotRequested, this,
                [this](const QString& call) {
                    statusBar()->showMessage(
                        spotClient_.spotDx(call, qint64(centerHz_))
                            ? QString("spotted %1 at %2 kHz")
                                  .arg(call)
                                  .arg(centerHz_ / 1000.0, 0, 'f', 1)
                            : QStringLiteral(
                                  "spot NOT sent — no logged-in cluster "
                                  "connection (SPOTS ▾)"),
                        6000);
                });
        connect(logWin_, &LogWindow::qsoLogged, this,
                [this](qint64 id, const QString& c) {
                    statusBar()->showMessage("logged " + c, 3000);
                    if (cwWin_) cwWin_->setHisCall(QString());
                    if (uploader_) uploader_->pushQso(id);
                    // Mirror the finished QSO to cqrlog's bridge so both
                    // logs stay in step. Only console-born QSOs: WSJT-X
                    // reaches cqrlog on its own 2237 ear, mirroring those
                    // would double-log them.
                    if (logDb_) mirrorToCqrlog(logDb_->qso(id));
                    enrichQso(id, c);
                });
        // Dial and mode ride in once a second while the window is up — every
        // tune path (knob, band button, WSJT-X, click) funnels into
        // centerHz_/rigMode_, so polling beats hooking each one.
        auto* feed = new QTimer(logWin_);
        feed->setInterval(1000);
        connect(feed, &QTimer::timeout, logWin_, [this] {
            if (logWin_->isVisible())
                logWin_->setRig(qint64(centerHz_), adifModeText(rigMode_));
        });
        feed->start();
    }
    logWin_->setRig(qint64(centerHz_), adifModeText(rigMode_));
    if (!call.isEmpty()) logWin_->prefill(call, park, grid);
    logWin_->show();
    logWin_->raise();
    logWin_->activateWindow();
}

void MainWindow::openLogbookWindow() {
    if (!logbookWin_) {
        logbookWin_ = new LogbookWindow(logDb_, &cty_, uploader_,
                                        toolWinParent(this));
        adoptToolWindow(logbookWin_);
        // "→ cqrlog": compare, confirm with the count, then send —
        // cqrlog is the award log, nothing goes over without a yes.
        connect(logbookWin_, &LogbookWindow::cqrlogSyncRequested, this,
                [this] {
                    statusBar()->showMessage("comparing against cqrlog…");
                    if (cqrSync_) cqrSync_->checkMissing();
                });
        connect(cqrSync_, &CqrlogSync::missingReady, this,
                [this](const QList<qint64>& ids, const QString& err) {
                    if (!err.isEmpty()) {
                        statusBar()->showMessage("cqrlog sync: " + err,
                                                 8000);
                        return;
                    }
                    if (ids.isEmpty()) {
                        statusBar()->showMessage(
                            "cqrlog has everything — logs are in sync",
                            6000);
                        return;
                    }
                    const auto a = QMessageBox::question(
                        logbookWin_, "Sync to cqrlog",
                        QString("%1 QSO%2 in the station log %3 not in "
                                "cqrlog.\nSend %4 through the bridge "
                                "now?")
                            .arg(ids.size())
                            .arg(ids.size() == 1 ? "" : "s")
                            .arg(ids.size() == 1 ? "is" : "are")
                            .arg(ids.size() == 1 ? "it" : "them"));
                    if (a == QMessageBox::Yes && cqrSync_)
                        cqrSync_->sendMissing(ids);
                });
        connect(cqrSync_, &CqrlogSync::pushedToCqrlog, this,
                [this](int n) {
                    statusBar()->showMessage(
                        QString("→ cqrlog: %1 QSO%2 sent through the "
                                "bridge").arg(n).arg(n == 1 ? "" : "s"),
                        8000);
                });
    }
    logbookWin_->show();
    logbookWin_->raise();
    logbookWin_->activateWindow();
}

// The tuned reader serves two customers — the CW window's RX pane and
// the contest deck's CW READ — and it has two possible ears: the SDR at
// the dial or the RADIO's audio via the SignaLink (cw/rxRadio, the
// operator's weak-signal choice). Whichever customer is awake powers
// the OPERATOR'S chosen ear; wiring the deck to the IQ side only left
// its pane blank at a station that decodes the radio audio (live-found
// on the first contest-mode drive).
void MainWindow::applyCwRxRouting() {
    const bool want = rxWanted_ || contestRx_;
    if (cwDec_) cwDec_->setEnabled(want && !rxRadio_);
    if (audioDec_) audioDec_->setEnabled(want && rxRadio_);
    if (audioSrc_) {
        // Capture runs whenever decode is on, regardless of source: the
        // pitch readout measures the radio's audio even while the SDR
        // does the decoding.
        if (want) audioSrc_->start();
        else audioSrc_->stop();
    }
}

bool MainWindow::contestDeckVisible() const {
    return contestDeck_ && contestDeck_->isVisible();
}

char MainWindow::contestClassify(const QString& call, qint64 hz) const {
    return contestDeck_ ? contestDeck_->classifySpot(call, hz) : 0;
}

// CONTEST button: swap the waterfall for the contest deck and back. The
// spectrum above stays live — it IS the band map (operator's sketch).
void MainWindow::toggleContestMode(bool on) {
    if (on) {
        if (!contestDeck_) {
            if (!contestDb_) {
                contestDb_ = new ContestDb(this);
                if (!contestDb_->open()) {
                    statusBar()->showMessage(
                        "contest.db failed to open — contest mode "
                        "unavailable", 8000);
                    delete contestDb_;
                    contestDb_ = nullptr;
                    return;
                }
            }
            // Full CW plumbing, window closed: deck F-keys must key
            // whether or not the CW window is ever opened.
            ensureCwWindow();
            contestDeck_ = new ContestDeck(contestDb_, &cty_, cwWin_,
                                           &rotor_, qrz_, this);
            layout_->placeUnderPan(contestDeck_);     // under the pan
            connect(contestDeck_, &ContestDeck::openManagerRequested,
                    this, [this] { openContestWindow(); });
            connect(contestDeck_, &ContestDeck::rotorAimed,
                    rose_, &CompassRoseWidget::pointBearing);
            connect(contestDeck_, &ContestDeck::spotDxRequested, this,
                    [this](const QString& call, qint64 hz) {
                        statusBar()->showMessage(
                            spotClient_.spotDx(call, hz)
                                ? QString("spotted %1 at %2 kHz")
                                      .arg(call)
                                      .arg(hz / 1000.0, 0, 'f', 1)
                                : QStringLiteral(
                                      "spot NOT sent — no logged-in "
                                      "cluster connection (SPOTS ▾)"),
                            6000);
                    });
            connect(contestDeck_, &ContestDeck::walkSpots, this,
                    [this](int d) { walkContestSpot(d); });
            connect(contestDeck_, &ContestDeck::pttToggleRequested, this,
                    [this] { txBar_->toggleMox(); });   // phone: Space = MOX
            // SUB line: VFO B's dial, and "take it" = swap A/B (B's station
            // comes to the main receiver/transmitter) + the call into the
            // main entry, anchored at B's old frequency.
            contestDeck_->setVfoBSource([this] { return qint64(vfoBHz_); });
            connect(contestDeck_, &ContestDeck::subTakeRequested, this,
                    [this](const QString& call) {
                        if (vfoLockA_ || vfoLockB_) {
                            statusBar()->showMessage("SUB: A<>B blocked — a VFO is LOCKED", 6000);
                            return;
                        }
                        const qint64 bHz = qint64(vfoBHz_);
                        emit routing_->swapABRequested();
                        contestDeck_->prefillCall(call, bHz);
                        statusBar()->showMessage(QString("SUB: %1 — A<>B swapped, work it on %2 kHz")
                                                     .arg(call).arg(bHz / 1000.0, 0, 'f', 1), 6000);
                    });
            // BOTH ears feed the deck; applyCwRxRouting keeps exactly
            // one enabled, following the operator's source choice.
            if (cwDec_)
                connect(cwDec_, &CwDecoder::textDecoded, contestDeck_,
                        &ContestDeck::appendRead, Qt::QueuedConnection);
            if (audioDec_)
                connect(audioDec_, &CwDecoder::textDecoded, contestDeck_,
                        &ContestDeck::appendRead, Qt::QueuedConnection);
            auto* feed = new QTimer(contestDeck_);
            feed->setInterval(1000);
            connect(feed, &QTimer::timeout, contestDeck_, [this] {
                if (!contestDeck_->isVisible()) return;
                contestDeck_->setRig(qint64(centerHz_),
                                     adifModeText(rigMode_));
                // The call frame: nearest spot within 500 Hz of the
                // dial — knob-tune onto someone and their call offers
                // itself; Space grabs it.
                const qint64 cur = qint64(centerHz_);
                const SpotLabel* best = nullptr;
                qint64 bestD = 501;
                for (const SpotLabel& s : shownSpots_) {
                    const qint64 d = qAbs(s.hz - cur);
                    if (d < bestD) {
                        bestD = d;
                        best = &s;
                    }
                }
                contestDeck_->setNearbySpot(
                    best ? best->call : QString(),
                    best ? best->contest : 0, best ? best->hz : 0);
            });
            feed->start();
            connect(contestDeck_, &ContestDeck::callParked, this,
                    [this](const QString& call, qint64 hz) {
                        Spot s;
                        s.call = call;
                        s.hz = hz;
                        s.atSecs = QDateTime::currentSecsSinceEpoch();
                        for (int i = parkedSpots_.size() - 1; i >= 0; --i)
                            if (parkedSpots_[i].call == call)
                                parkedSpots_.removeAt(i);
                        parkedSpots_.push_back(s);
                        if (pushSpots_) pushSpots_();
                        statusBar()->showMessage(
                            QString("parked %1 at %2 kHz — it's on the "
                                    "map for 20 min")
                                .arg(call)
                                .arg(hz / 1000.0, 0, 'f', 1),
                            5000);
                    });
            contestDeck_->setMasterScp(loadMasterScp());
            // Phone contests: {VKn} F-keys play DVR slots on the same
            // rails as the TX-bar buttons; a press during playback
            // aborts rather than stacking clips.
            contestDeck_->setVoiceKeyer(
                [this](int slot) -> bool {
                    // A press during playback aborts (handled, beats
                    // stand); otherwise report whether audio went out.
                    if (stopVoicePlayback()) return true;
                    return playVoiceSlot(slot);
                },
                [this] { stopVoicePlayback(); });
        }
        contestDeck_->setRig(qint64(centerHz_), adifModeText(rigMode_));
        savedSplit_ = pan_->displaySettings().split;
        DisplaySettings ds = pan_->displaySettings();
        ds.split = 1.0f;                // spectrum takes the whole widget
        pan_->setDisplaySettings(ds);
        contestDeck_->setVisible(true);
        contestRx_ = true;
        applyCwRxRouting();
        pan_->installEventFilter(this);      // arrows walk from the pan too
        if (spotTable_) spotTable_->setContestMode(true);
        if (!contestDeck_->contestActive()) openContestWindow();
    } else {
        if (contestDeck_) contestDeck_->setVisible(false);
        pan_->removeEventFilter(this);
        if (spotTable_) spotTable_->setContestMode(false);
        DisplaySettings ds = pan_->displaySettings();
        ds.split = savedSplit_ > 0.0f && savedSplit_ < 1.0f ? savedSplit_
                                                            : 0.42f;
        pan_->setDisplaySettings(ds);
        contestRx_ = false;
        applyCwRxRouting();
    }
    if (pushSpots_) pushSpots_();      // recolor the labels immediately
}

// Contest mode: the arrows must work wherever focus landed — a mouse
// click parks focus on the panadapter, and the operator's hands live on
// the keyboard. The deck's call box has its own filter; this one covers
// the pan.
void MainWindow::setMox(bool on) {
    if (!txBar_ || txBar_->moxActive() == on) return;
    txBar_->toggleMox();                     // emits moxToggled -> the MOX handler
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    // Keyboard PTT, app-wide: Right Ctrl held = TX (X11 keysym Control_R),
    // Pause = MOX toggle. Auto-repeat is ignored so a held key is one
    // press and one release.
    if ((ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease)
        && QApplication::activeWindow()) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (!ke->isAutoRepeat()) {
            const bool press = ev->type() == QEvent::KeyPress;
            if (ke->key() == Qt::Key_Control && ke->nativeVirtualKey() == 0xffe4) {
                setMox(press);
                return true;
            }
            if (ke->key() == Qt::Key_Pause && press) {
                setMox(!txBar_->moxActive());
                return true;
            }
        }
    }
    // Deck up: ←/→ walk the spots and ↑/↓ nudge the speed from ANYWHERE
    // in the main window, not only from the pan or the call box. Any
    // click on a band button, slider or TX-bar control used to take the
    // keyboard with it and the arrows went dead until a spot was clicked
    // (live-found 2026-10-24, CQ WW SSB). Widgets where the arrows have a
    // real job (text cursor, spin box, list, slider) keep them; the deck
    // decides for its own fields (its call-box filter).
    if (ev->type() == QEvent::KeyPress && contestDeckVisible()
        && qobject_cast<QWidget*>(obj) && QApplication::activeWindow() == this) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        const int k = ke->key();
        const bool arrow = k == Qt::Key_Left || k == Qt::Key_Right
            || k == Qt::Key_Up || k == Qt::Key_Down;
        if (arrow && ke->modifiers() == Qt::NoModifier) {
            QWidget* fw = QApplication::focusWidget();
            const bool deckOwns = fw && contestDeck_
                && contestDeck_->isAncestorOf(fw);
            const bool editing = fw
                && (qobject_cast<QLineEdit*>(fw)
                    || qobject_cast<QAbstractSpinBox*>(fw)
                    || qobject_cast<QComboBox*>(fw)
                    || qobject_cast<QAbstractSlider*>(fw)
                    || qobject_cast<QAbstractItemView*>(fw)
                    || qobject_cast<QTextEdit*>(fw)
                    || qobject_cast<QPlainTextEdit*>(fw));
            if (!deckOwns && (obj == pan_ || !editing)) {
                if (k == Qt::Key_Left || k == Qt::Key_Right)
                    walkContestSpot(k == Qt::Key_Right ? +1 : -1);
                else
                    contestDeck_->nudgeSpeed(k == Qt::Key_Up ? +1 : -1);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}

// ←/→ with an empty call box: jump the dial along the spots on screen,
// skipping everything the contest says is worthless (worked, zero-pt).
// Fenced to the band under the dial: the cluster aggregate spans every
// band, and an unfenced walk once QSY'd the operator to 20 m mid-40 m
// run — at this station a band change means antennas and amp retuning,
// never a side effect of an arrow key. Band moves stay on the band
// buttons.
void MainWindow::walkContestSpot(int dir) {
    if (shownSpots_.isEmpty()) return;
    QVector<const SpotLabel*> ord;
    for (const SpotLabel& s : shownSpots_) ord << &s;
    std::sort(ord.begin(), ord.end(),
              [](const SpotLabel* a, const SpotLabel* b) {
                  return a->hz < b->hz;
              });
    const qint64 cur = qint64(centerHz_);
    const QString band = LogbookIndex::bandForHz(cur);
    const auto worthy = [&band](const SpotLabel* s) {
        if (LogbookIndex::bandForHz(s->hz) != band) return false;
        return s->contest == 0 || s->contest == 'M' || s->contest == 'N';
    };
    const SpotLabel* pick = nullptr;
    if (dir > 0) {
        for (const SpotLabel* s : ord)
            if (s->hz > cur + 20 && worthy(s)) { pick = s; break; }
    } else {
        for (auto it = ord.rbegin(); it != ord.rend(); ++it)
            if ((*it)->hz < cur - 20 && worthy(*it)) { pick = *it; break; }
    }
    if (!pick) return;                 // nothing worth tuning that way
    onTuneRequested(int(pick->hz - cur), true);
    if (contestDeck_) contestDeck_->prefillCall(pick->call, pick->hz);
    if (cwWin_) cwWin_->setHisCall(pick->call);
}

void MainWindow::openDigiWindow() {
    if (!digiWin_) {
        fldigi_ = new FldigiClient(this);
        fldigi_->setEndpoint(
            QSettings().value("digi/host", "127.0.0.1").toString(),
            quint16(QSettings().value("digi/port", 7362).toUInt()));
        digiWin_ = new DigiWindow(fldigi_, toolWinParent(this));
        adoptToolWindow(digiWin_);
    }
    digiWin_->show();
    digiWin_->raise();
    digiWin_->activateWindow();
}

void MainWindow::openContestWindow() {
    if (!contestWin_) {
        // The deck may have opened contest.db first — ONE instance, or
        // the two surfaces stop hearing each other's changed() signals.
        if (!contestDb_) {
            contestDb_ = new ContestDb(this);
            if (!contestDb_->open()) {
                statusBar()->showMessage(
                    "contest.db failed to open — contest logger "
                    "unavailable", 8000);
                delete contestDb_;
                contestDb_ = nullptr;
                return;
            }
        }
        contestWin_ = new ContestWindow(contestDb_, &cty_, logDb_,
                                        toolWinParent(this));
        adoptToolWindow(contestWin_);
        connect(contestWin_, &ContestWindow::contestOpened, this,
                [this](qint64 id) {
                    if (contestDeck_) contestDeck_->openContestId(id);
                });
        connect(contestWin_, &ContestWindow::contestDeleted, this,
                [this](qint64 id) {
                    // Deck was showing the deleted contest? Drop it.
                    if (contestDeck_ && contestDeck_->contestActive()
                        && contestDeck_->currentContestId() == id)
                        contestDeck_->closeContest();
                });
        // Pushed contest QSOs ride to LoTW & the online logs on the
        // uploader's catch-up sweep — one batch, one tqsl run — AND
        // mirror to cqrlog's bridge one by one, paced so its fill-form-
        // then-save keeps up (the MST push that never reached cqrlog,
        // live-found). CqrlogSync's echo of these comes back as a
        // near-duplicate and is skipped, so the loop is idempotent.
        connect(contestWin_, &ContestWindow::pushedToLogbook, this,
                [this](const QList<qint64>& ids) {
                    if (uploader_) uploader_->sweepSoon(500);
                    int i = 0;
                    if (logDb_)
                        for (const qint64 id : ids)
                            mirrorToCqrlog(logDb_->qso(id), 250 * i++);
                    statusBar()->showMessage(
                        QString("%1 contest QSOs → online logs + cqrlog")
                            .arg(ids.size()), 6000);
                });
    }
    contestWin_->show();
    contestWin_->raise();
    contestWin_->activateWindow();
}

void MainWindow::enrichQso(qint64 id, const QString& call) {
    if (!qrz_ || id < 0) return;
    if (QSettings().value("up/qrzweb/user").toString().trimmed().isEmpty())
        return;                            // no callbook login configured
    enrichPending_.insert(call.trimmed().toUpper(), id);
    qrz_->lookup(call);
}

void MainWindow::openSpotTable() {
    if (!spotTable_) {
        spotTable_ = new SpotTableWindow(&logbook_, &rotor_,
                                         toolWinParent(this));
        adoptToolWindow(spotTable_);
        // Double-click = the band-map spot click, plus the QSY the band map
        // can't do (a table row may live on another band entirely).
        connect(spotTable_, &SpotTableWindow::spotActivated, this,
                [this](const QString& call, qint64 hz, QChar kind,
                       const QString& tag) {
                    if (hz > 0) radio_->setFrequencyHz(Rx::Main, hz);
                    if (cwWin_) cwWin_->setHisCall(call);
                    // A table row is the deliberate cross-band gesture
                    // (antennas and tuner follow the operator) — the
                    // deck rides along like any spot click.
                    if (contestDeckVisible())
                        contestDeck_->prefillCall(call, hz);
                    QString park, grid;
                    if (kind == QChar('P')) {
                        park = tag;
                        for (const Spot& s : potaClient_.spots())
                            if (s.call == call) { grid = s.grid; break; }
                    }
                    sendCqrLookup(call, park, grid);
                });
    }
    // Opened mid-contest: come up already wearing the contest view.
    spotTable_->setContestMode(contestDeckVisible());
    spotTable_->show();
    spotTable_->raise();
    spotTable_->activateWindow();
}

// One QSO -> cqrlog's bridge (builder shared with CqrlogSync — the
// bridge drops anything that doesn't start with <CALL).
void MainWindow::mirrorToCqrlog(const Qso& q, int delayMs) {
    if (!logUdp_ || q.id < 0) return;
    if (!QSettings().value("log/mirrorCqrlog", true).toBool()) return;
    const QByteArray d = CqrlogSync::bridgeDatagram(q);
    const auto send = [this, d] {
        if (logUdp_)
            logUdp_->writeDatagram(
                d, QHostAddress::LocalHost,
                quint16(QSettings().value("log/port", 2334).toInt()));
    };
    if (delayMs > 0)
        QTimer::singleShot(delayMs, this, send);
    else
        send();
}

void MainWindow::sendCqrLookup(const QString& call, const QString& park,
                               const QString& grid) {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty() || !logUdp_) return;
    // Every path that feeds a call here (spot click, CW decode double-click,
    // skimmer windows) also pre-fills the console's own entry window.
    if (logWin_ && logWin_->isVisible()) logWin_->prefill(c, park, grid);
    QString msg = "CQRLOOKUP:" + c;
    if (!park.isEmpty()) msg += ";PARK:" + park.trimmed().toUpper();
    if (!grid.isEmpty()) msg += ";GRID:" + grid.trimmed();
    // Fire-and-forget on purpose: a click shouldn't nag when cqrlog is
    // closed — the LOG button already does the is-anyone-listening probe.
    logUdp_->writeDatagram(
        msg.toUtf8(), QHostAddress::LocalHost,
        quint16(QSettings().value("log/port", 2334).toInt()));
}

// Build — but do not show — the CW window with its full plumbing:
// keyer backend, decode feeds, cqrlog rails. Split out of the CW
// button so the contest window can key through the console while the
// CW window stays closed. The external-logger era silently no-op'd
// every F-key whenever this window wasn't up; that trap ends here.
void MainWindow::ensureCwWindow() {
    if (cwWin_) return;
    cwWin_ = new CwWindow(radio_, toolWinParent(this));
    cwWin_->setContext(qsoContext());
    adoptToolWindow(cwWin_);
    WindowLight::bind(cwBtn_, cwWin_);
    cwWin_->setMyCall(QSettings()
        .value("station/callsign", "").toString());
    cwWin_->setHisCall(QString());
    // Double-clicked call in the decode pane rides the same rails
    // as a spot click: send it to cqrlog's New QSO, arm the %c macro.
    connect(cwWin_, &CwWindow::callDoubleClicked, this,
            [this](const QString& call) {
                cwWin_->setHisCall(call);
                sendCqrLookup(call);
                statusBar()->showMessage(
                    QString("cqrlog ← %1 (from CW copy)")
                        .arg(call), 5000);
            });
    // Same rails for a call typed by hand in the DX box: the
    // console is the master of the callsign, cqrlog follows.
    // (There is no path the other way — cqrlog's bridge is
    // receive-only, so typing it THERE reaches nothing here.)
    connect(cwWin_, &CwWindow::hisCallEntered, this,
            [this](const QString& call) {
                sendCqrLookup(call);
                statusBar()->showMessage(
                    QString("cqrlog ← %1 (typed)").arg(call), 5000);
            });
    if (cwDec_) {                  // SDR-fed CW reader plumbing
        connect(cwDec_, &CwDecoder::textDecoded,
                cwWin_, &CwWindow::appendRx, Qt::QueuedConnection);
        connect(cwDec_, &CwDecoder::wpmEstimated,
                cwWin_, &CwWindow::setRxWpm, Qt::QueuedConnection);
        // Two selectable ears for the same reader: the SDR at
        // the dial (default; AF can be zero) or the RADIO's audio
        // via the SignaLink — the input real fldigi gets, and the
        // weak-signal winner while the SDR rides the passive tap.
        // One decoder instance per source; exactly one enabled.
        const int pitch =
            QSettings().value("cw/pitchHz", 550).toInt();
        audioDec_ = new CwDecoder(48000.0, double(pitch), this);
        audioSrc_ = new AudioCwSource(audioDec_, this);
        audioSrc_->setTargetPitch(pitch);   // never notch the target
        connect(audioDec_, &CwDecoder::textDecoded,
                cwWin_, &CwWindow::appendRx, Qt::QueuedConnection);
        connect(audioDec_, &CwDecoder::wpmEstimated,
                cwWin_, &CwWindow::setRxWpm, Qt::QueuedConnection);
        connect(audioSrc_, &AudioCwSource::pitchMeasured,
                cwWin_, &CwWindow::setRxPitch);
        connect(audioSrc_, &AudioCwSource::pitchMeasured, this,
                [this](double hz) {
                    lastPitchHz_ = hz;
                    lastPitchMs_ =
                        QDateTime::currentMSecsSinceEpoch();
                    pitchTrimFeed(hz);
                });
        // The RADIO ear on the wrong sound device (PipeWire fell back to
        // another source): a red status-bar button that says so and fixes
        // it in one click — hidden while the capture is where it belongs.
        connect(audioSrc_, &AudioCwSource::captureWrong, this,
                [this](const QString& actual, const QString&) {
                    if (!cwAudioWarn_) {   // created on first need: even hidden, a
                                           // permanent status-bar widget nudged the bar
                                           // 1 px (the Classic guard caught it)
                        cwAudioWarn_ = new QPushButton(statusBar());
                        cwAudioWarn_->setFocusPolicy(Qt::NoFocus);
                        cwAudioWarn_->setStyleSheet(
                            "QPushButton { background: #5a1c1c; color: #ffd8d8; border: 1px solid #c04848;"
                            " border-radius: 3px; padding: 1px 10px; font-weight: bold; }"
                            "QPushButton:hover { background: #7a2626; }");
                        cwAudioWarn_->hide();
                        statusBar()->addPermanentWidget(cwAudioWarn_);
                        connect(cwAudioWarn_, &QPushButton::clicked, this, [this] {
                            cwAudioWarn_->setText("Fixing the CW audio…");
                            audioSrc_->fixCapture();
                        });
                    }
                    cwAudioWarn_->setText("⚠ CW decoder isn't hearing the SignaLink — click to fix");
                    cwAudioWarn_->setToolTip(
                        "The RADIO ear asked for the SignaLink, but the sound system\n"
                        "connected it to:\n  " + actual + "\n"
                        "Click: re-apply the SignaLink's audio profile (off, then back\n"
                        "on), which brings its input back; the capture follows.");
                    cwAudioWarn_->show();
                    std::fprintf(stderr, "[cw] capture on %s, not the SignaLink\n",
                                 actual.toLocal8Bit().constData());
                });
        connect(audioSrc_, &AudioCwSource::captureOk, this, [this] {
            if (!cwAudioWarn_) return;
            if (cwAudioWarn_->isVisible())
                statusBar()->showMessage("CW decoder is hearing the SignaLink again", 6000);
            cwAudioWarn_->hide();
        });
        connect(audioSrc_, &AudioCwSource::statusChanged, this,
                [this](const QString& t) {
                    statusBar()->showMessage(t, 6000);
                });
        rxRadio_ = QSettings().value("cw/rxRadio", false).toBool();
        const auto applyRouting = [this] { applyCwRxRouting(); };
        connect(cwWin_, &CwWindow::rxDecodeWanted, this,
                [this, applyRouting](bool on) {
                    rxWanted_ = on;
                    applyRouting();
                });
        audioSrc_->setNr(QSettings().value("cw/nr", false).toBool());
        connect(cwWin_, &CwWindow::rxNrChanged, this,
                [this](bool on) { audioSrc_->setNr(on); });
        connect(cwWin_, &CwWindow::zeroBeatRequested, this,
                [this] { zeroBeat(); });
        connect(cwWin_, &CwWindow::txImminent, this, [this] {
            txPredictMs_ = QDateTime::currentMSecsSinceEpoch();
        });
        connect(cwWin_, &CwWindow::rxSourceChanged, this,
                [this, applyRouting](bool radio) {
                    rxRadio_ = radio;
                    applyRouting();
                });
        // Decode-engine adjustments: apply the persisted state now,
        // then live-follow the window's controls. These knobs steer
        // the tuned reader (and its audio twin) only — the skimmer's
        // channels run the engine at its defaults (SOM off; see
        // SkimmerEngine's constructor for why).
        const auto applyCfg = [this](bool eng, bool som, bool deep,
                                     int atk, int dcy) {
            for (CwDecoder* d : {cwDec_, audioDec_}) {
                d->setEngineMode(eng);
                d->setSom(som);
                d->setDeep(deep);
                d->setAttack(atk);
                d->setDecay(dcy);
            }
        };
        const auto applySql = [this](int sql) {
            for (CwDecoder* d : {cwDec_, audioDec_})
                d->setSquelch(double(sql));
        };
        QSettings cs;
        applyCfg(cs.value("cw/engine", true).toBool(),
                 cs.value("cw/som", true).toBool(),
                 cs.value("cw/deep", false).toBool(),
                 cs.value("cw/attack", 1).toInt(),
                 cs.value("cw/decay", 1).toInt());
        applySql(cs.value("cw/squelch", 12).toInt());
        connect(cwWin_, &CwWindow::rxDecodeConfigChanged, this,
                applyCfg);
        connect(cwWin_, &CwWindow::rxSquelchChanged, this, applySql);
    }
    wireRigCwPanel();
}

// EQ: the voice keyer's EQ & dynamics window (it lived in the AUDIO menu
// until AUDIO became a sidebar box, 2026-10-18). Lit while it is up; a
// second press closes it, as its × does.
void MainWindow::setupEqUi() {
    auto* eqBtn = new QToolButton(topStrip_);
    eqBtn->setText("EQ");
    eqBtn->setCheckable(true);
    eqBtn->setFocusPolicy(Qt::NoFocus);
    eqBtn->setStyleSheet(QString(kSideBtnStyle));
    eqBtn->setToolTip("Voice keyer EQ & dynamics (gate, EQ, de-esser,\n"
                      "compressor, limiter) — press again to close");
    eqBtn_ = eqBtn;
    topLay2_->addSpacing(8);
    topLay2_->addWidget(eqBtn);
    connect(eqBtn, &QToolButton::clicked, this, [this] {
        if (windowUp(vkEqDlg_.data())) {
            vkEqDlg_->close();
            return;
        }
        showVoiceEq();
    });
}

void MainWindow::lightWhile(QToolButton* b, QMenu* m, std::function<bool()> on) {
    b->setCheckable(true);
    lights_.push_back({b, std::move(on)});
    // A menu click usually flips the feature: show it as the menu closes.
    connect(m, &QMenu::aboutToHide, this, [this] {
        QTimer::singleShot(0, this, [this] { refreshLights(); });
    });
}

void MainWindow::refreshLights() {
    for (const auto& l : lights_)
        if (l.first) l.first->setChecked(l.second());
}

void MainWindow::setupCwUi() {
    // "CW" button: the WinKeyer sending window (type-ahead + memories).
    // The keyer hardware keeps the paddle in charge; this is the keyboard.
    auto* cwBtn = new QToolButton(topStrip_);
    cwBtn->setText("CW");
    cwBtn->setFocusPolicy(Qt::NoFocus);
    cwBtn->setStyleSheet(QString(kSideBtnStyle));
    cwBtn->setCheckable(true);                     // lit while the window is up
    cwBtn_ = cwBtn;
    cwBtn->setToolTip("CW keyboard/memories via the WinKeyer\n(paddle always "
                      "wins — touching it dumps the buffer)\n"
                      "Right-click: WinKeyer control (weighting, key "
                      "compensation, ratio…)");
    topLay2_->addSpacing(8);
    topLay2_->addWidget(cwBtn);
    connect(cwBtn, &QToolButton::clicked, this, [this] {
        if (windowUp(cwWin_)) {                    // second press closes it
            cwWin_->close();
            return;
        }
        ensureCwWindow();
        cwWin_->show();
        cwWin_->raise();
        cwWin_->activateWindow();
    });

    // Right-click opens the WinKeyer control box. It is a right-click and
    // not a sixth button because the top strip has ~2 px of slack against
    // the width budget, and losing the maximize button has bitten twice —
    // same reason the SWR menu hangs off a right-click on TUNE.
    cwBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(cwBtn, &QToolButton::customContextMenuRequested, this,
            [this, cwBtn](const QPoint&) {
        // Reuse the left-click path wholesale so the keyer gets built and
        // opened exactly the way it always is.
        if (!cwWin_) cwBtn->click();
        auto* wk = qobject_cast<ttc::WinKeyer*>(
            cwWin_ ? cwWin_->keyer() : nullptr);
        if (!wk) {
            statusBar()->showMessage(
                "WinKeyer control: the WinKeyer is not the active keyer — "
                "the radio's own keyer has no host-settable element timing "
                "(SDR ▸ Station setup… to switch)", 8000);
            return;
        }
        if (!wkPanel_) {
            auto* p = new ttc::WinKeyerPanel(wk, this);
            p->setAttribute(Qt::WA_DeleteOnClose);
            wkPanel_ = p;
        }
        wkPanel_->show();
        wkPanel_->raise();
        wkPanel_->activateWindow();
    });
}

// The CW window's "RADIO — CW" panel: the rig's own sidetone level and
// pitch, QSK delay and envelope, over CAT. Rig-side settings, so they
// apply with the WinKeyer doing the keying — which is why they live in
// the keyer window rather than a setup dialog.
void MainWindow::wireRigCwPanel() {
    if (!cwWin_ || !radio_) return;
    cwWin_->setRigCwAvailable(radio_->caps().catCwControls
                              && radio_->connected());

    connect(cwWin_, &CwWindow::rigSidetoneVolChanged, this,
            [this](int v) { radio_->setCwSidetoneVol(v); });
    connect(cwWin_, &CwWindow::rigQskDelayChanged, this,
            [this](int v) { radio_->setCwQskDelay(v); });
    connect(cwWin_, &CwWindow::rigAttackDecayChanged, this,
            [this](int v) { radio_->setCwAttackDecay(v); });
    connect(cwWin_, &CwWindow::rigSidetonePitchChanged, this, [this](int hz) {
        radio_->setCwSidetonePitch(hz);
        applyCwPitch(hz);                  // follow our own change at once
    });

    connect(radio_, &RadioController::cwSidetoneVolReported,
            cwWin_, &CwWindow::showRigSidetoneVol);
    connect(radio_, &RadioController::cwQskDelayReported,
            cwWin_, &CwWindow::showRigQskDelay);
    connect(radio_, &RadioController::cwAttackDecayReported,
            cwWin_, &CwWindow::showRigAttackDecay);
    connect(radio_, &RadioController::cwKeyerSpeedReported,
            cwWin_, &CwWindow::showRigKeyerSpeed);
    connect(radio_, &RadioController::cwKeyerWeightReported,
            cwWin_, &CwWindow::showRigKeyerWeight);
    connect(radio_, &RadioController::cwKeyerEnabledReported,
            cwWin_, &CwWindow::showRigKeyerEnabled);
    connect(radio_, &RadioController::cwSidetonePitchReported, this,
            [this](int hz) {
                cwWin_->showRigSidetonePitch(hz);
                applyCwPitch(hz);          // and follow the FRONT PANEL
            });
    radio_->queryCw();                     // fill the panel on first open
}

// The radio's sidetone pitch is the truth; cw/pitchHz is only our cache
// of it. Everything that assumed 550 now follows whatever the rig says —
// which is what makes the reader work for an operator who isn't Jon.
void MainWindow::applyCwPitch(int hz) {
    if (hz < 300 || hz > 1200) return;
    if (QSettings().value("cw/pitchHz", 550).toInt() == hz) return;
    QSettings().setValue("cw/pitchHz", hz);
    // The audio-path decoder mixes the sidetone down to DC, so its offset
    // IS the pitch — retune() is atomic and the capture thread applies it
    // at the next block. The SDR-path decoder is carrier-at-dial and the
    // skimmer hops its own channels, so neither cares. Zero-beat and the
    // Hz readout re-read the setting every time they run.
    if (audioDec_) audioDec_->retune(double(hz));
    if (audioSrc_) audioSrc_->setTargetPitch(hz);   // move the notch guard
    updateSdrRxDemod();                            // SDR receiver's CW pitch
    statusBar()->showMessage(
        QString("CW sidetone %1 Hz — reader and 0-beat follow").arg(hz), 4000);
}

void MainWindow::setupSkimUi(const QString& stationCall) {
    // "SKIM" dropdown: the CW skimmer — a bank of decoder channels parked on
    // the strongest signals in the band's CW segment, mining the decoded
    // text for callsigns. Found calls land on the panadapter as violet
    // spots, worked-before colored like everything else. The engine exists
    // even without an SDR build; it just never gets IQ.
    skim_ = new SkimmerEngine(                     // rate must match the SDR
        double(kSdrCaptureHz),
        std::clamp(QSettings().value("skim/channels", 24).toInt(), 4, 64),
        this);
    skim_->setCallValidator([this](const QString& call) {
        double la = 0.0, lo = 0.0;                 // decode artifacts have
        return cty_.lookup(call, la, lo);          // no country prefix
    });
    skim_->setKnownCalls(loadMasterScp());
    // The SKIM view's fast STFT taps the IQ stream in MainWindow's
    // iqHandler; it costs one branch per block until the window opens.
    skimStft_ = new SkimStft(double(kSdrCaptureHz), this);
    auto* skimBtn = new QToolButton(topStrip_);
    skimBtn->setText("SKIM ▾");
    skimBtn->setPopupMode(QToolButton::InstantPopup);
    skimBtn->setFocusPolicy(Qt::NoFocus);
    skimBtn->setStyleSheet(QString(kSideBtnStyle));
    skimBtn->setToolTip("CW skimmer: decode the whole CW segment at once;\n"
                        "found callsigns appear as violet spots");
    auto* skimMenu = new QMenu(skimBtn);
    styleMenu(skimMenu);
    skimEnable_ = skimMenu->addAction("Skim the CW segment");
    skimEnable_->setCheckable(true);
    skimEnable_->setChecked(QSettings().value("skim/enabled", false).toBool());
    skimEnable_->setToolTip(
        QString("Run %1 decoder channels over the CW segment of the current "
                "band.\nCalls are validated against the country file and "
                "must repeat (or follow DE)\nbefore they're spotted.")
            .arg(skim_->channelCount()));
    skim_->setEnabled(skimEnable_->isChecked());
    // Band map: the skimmer's finds as a frequency-sorted click-to-tune
    // list (separate window, so it can live on all session).
    auto* skimMap = skimMenu->addAction("Band map…");
    connect(skimMap, &QAction::triggered, this, [this] {
        if (!skimWin_) {
            skimWin_ = new SkimmerWindow(
                skim_,
                [this](const QString& call, qint64 hz) {
                    if (!logbook_.ready()) return QChar('?');
                    return logbook_.status(call, LogbookIndex::bandForHz(hz));
                },
                toolWinParent(this));
            adoptToolWindow(skimWin_);
            connect(skimWin_, &SkimmerWindow::tuneTo, this,
                    [this](qint64 hz, const QString& call) {
                        tuneAbsolute(uint64_t(hz));
                        if (!call.isEmpty()) {
                            if (cwWin_) cwWin_->setHisCall(call);
                            sendCqrLookup(call);
                        }
                    });
        }
        skimWin_->show();
        skimWin_->raise();
        skimWin_->activateWindow();
    });
    // Waterfall: the CW-Skimmer-style view — keying visible per station,
    // decoded calls riding the traces.
    auto* skimView = skimMenu->addAction("Waterfall view…");
    skimView->setToolTip(
        "High-resolution waterfall of the CW segment: every station's dits\n"
        "and dahs visible, decoded callsigns next to their traces. Click a\n"
        "trace to tune, wheel to zoom.");
    connect(skimView, &QAction::triggered, this, &MainWindow::openSkimView);
    // Local RBN: serve the finds over cluster telnet while the skimmer
    // runs — point cqrlog's DX-cluster window at localhost:7300 and this
    // station spots for itself.
    skimSrv_ = new SkimServer(this);
    skimSrv_->setSpotterCall(stationCall);
    connect(skim_, &SkimmerEngine::spotFound, skimSrv_, &SkimServer::announce);
    auto* skimTelnet = skimMenu->addAction(
        QString("Telnet feed on localhost:%1 (for cqrlog)")
            .arg(QSettings().value("skim/telnetPort", 7300).toInt()));
    skimTelnet->setEnabled(false);
    skimTelnet->setToolTip(
        "While the skimmer runs, its finds are served in DX-cluster telnet\n"
        "format. In cqrlog: DX cluster -> connect to localhost port 7300 —\n"
        "your own receiver becomes a spotting node.");
    skimStatus_ = new QLabel(skimMenu);
    skimStatus_->setTextFormat(Qt::RichText);
    skimStatus_->setStyleSheet("QLabel { background: #141b24; color: #c8d4e0;"
                               " padding: 8px 12px; }");
    auto* skimStatAct = new QWidgetAction(skimMenu);
    skimStatAct->setDefaultWidget(skimStatus_);
    skimMenu->addSeparator();
    skimMenu->addAction(skimStatAct);
    skimBtn->setMenu(skimMenu);
    lightWhile(skimBtn, skimMenu, [this] { return skimEnable_->isChecked(); });
    topLay2_->addSpacing(8);
    topLay2_->addWidget(skimBtn);
    const auto refreshSkim = [this, skimMenu] {
        if (!skimMenu->isVisible()) return;
        QString h = "<pre style='margin:0; font-size:12px;'>";
        // Which CW window the bank is on, and how much of it the capture
        // actually reaches — the dial may be up on phone (that is the
        // point: the CW end keeps being read), and then part or all of
        // the segment can sit outside the 1 MHz window.
        {
            qint64 segLo = 0, segHi = 0;
            QString line;
            if (!SkimmerEngine::cwSegment(qint64(centerHz_), segLo, segHi)) {
                line = "<span style='color:#c87a7a;'>dial is not on a ham "
                       "band — nothing to skim</span>";
            } else {
                const qint64 loAbs = qint64(centerHz_) + loOffHz_;
                const qint64 capLo = loAbs - sdrSpanHz_ / 2;
                const qint64 capHi = loAbs + sdrSpanHz_ / 2;
                const qint64 gotLo = std::max(segLo, capLo);
                const qint64 gotHi = std::min(segHi, capHi);
                const QString win = QString("CW %1-%2")
                        .arg(segLo / 1000.0, 0, 'f', 1)
                        .arg(segHi / 1000.0, 0, 'f', 1);
                if (gotHi - gotLo < 2000)
                    line = QString("<span style='color:#c87a7a;'>%1 — out of "
                                   "the capture from this dial</span>").arg(win);
                else if (gotLo > segLo || gotHi < segHi)
                    line = QString("<span style='color:#d8b45a;'>%1 — reaching "
                                   "%2-%3</span>")
                               .arg(win).arg(gotLo / 1000.0, 0, 'f', 1)
                               .arg(gotHi / 1000.0, 0, 'f', 1);
                else
                    line = QString("<span style='color:#7fc98f;'>%1 — all of it "
                                   "in the capture</span>").arg(win);
            }
            h += line + "\n\n";
        }
        h += "<span style='color:#8fa3b8;'>  kHz      WPM CALL     "
             "DECODE</span>\n";
        for (const auto& c : skim_->channelInfo()) {
            if (!c.active) { h += "<span style='color:#4a5a6e;'>  —</span>\n"; continue; }
            h += QString("  %1 %2 %3 %4\n")
                     .arg(c.hz / 1000.0, -8, 'f', 1)
                     .arg(c.wpm > 0 ? QString::number(c.wpm) : QString("--"), 3)
                     .arg(c.call.isEmpty()
                              ? QString("<span style='color:#4a5a6e;'>?"
                                        "       </span>")
                              : QString("<span style='color:#cd8cff;'>%1</span>")
                                    .arg(c.call.leftJustified(8)),
                          -8)
                     .arg(c.text.toHtmlEscaped());
        }
        h += "</pre>";
        skimStatus_->setText(h);
    };
    connect(skimMenu, &QMenu::aboutToShow, this, [this, refreshSkim] {
        skimStatus_->setText(" ");                 // sized before first tick
        refreshSkim();
    });
    // One clock drives channel (re)assignment from the latest averaged
    // spectrum and the menu readout.
    auto* skimTick = new QTimer(this);
    skimTick->setInterval(2500);
    connect(skimTick, &QTimer::timeout, this, [this, refreshSkim] {
#ifdef HAVE_SDRPLAY
        if (skim_->enabled() && !lastSpectrum_.empty())
            skim_->updateFromSpectrum(lastSpectrum_, sdrSpanHz_,
                                      qint64(centerHz_), loOffHz_);
#endif
        refreshSkim();
    });
    skimTick->start();
    connect(skim_, &SkimmerEngine::spotFound, this,
            [this](const QString& call, qint64 hz, int wpm) {
                statusBar()->showMessage(
                    QString("SKIM: %1 on %2 kHz%3")
                        .arg(call)
                        .arg(hz / 1000.0, 0, 'f', 1)
                        .arg(wpm > 0 ? QString("  %1 WPM").arg(wpm)
                                     : QString()),
                    8000);
            });

}

void MainWindow::openSkimView() {
    if (!skimView_) {
        skimView_ = new SkimViewWindow(
            skimStft_, skim_,
            [this](const QString& call, qint64 hz) {
                if (!logbook_.ready()) return QChar('?');
                return logbook_.status(call, LogbookIndex::bandForHz(hz));
            },
            [this](qint64& dial, int& loOff) {
                dial = qint64(centerHz_);
                loOff = loOffHz_;
            },
            toolWinParent(this));
        adoptToolWindow(skimView_);
        connect(skimView_, &SkimViewWindow::tuneTo, this,
                [this](qint64 hz, const QString& call) {
                    tuneAbsolute(uint64_t(hz));
                    if (!call.isEmpty()) {
                        if (cwWin_) cwWin_->setHisCall(call);
                        sendCqrLookup(call);
                    }
                });
    }
    skimView_->show();
    skimView_->raise();
    skimView_->activateWindow();
}

void MainWindow::setupDigiUi() {
    // "CNTST" button (the old DIGI slot — the operator never used the
    // fldigi window from here, and it now lives on the LOG right-click):
    // toggles contest mode. The label is deliberately short — the top
    // strip has ~2 px of slack against the width budget.
    auto* btn = new QToolButton(topStrip_);
    btn->setText("CNTST");
    btn->setCheckable(true);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(QString(kSideBtnStyle));
    btn->setToolTip("Contest mode: the waterfall hands its space to the "
                    "contest deck\n(entry, F-keys, super check, CW "
                    "read/type) and the spectrum's spot labels recolor\n"
                    "to contest value — red mult, blue workable, gray "
                    "worked, dim zero-points.\nPress again to get the "
                    "waterfall back. Normal LOG keeps working either way.");
    topLay2_->addSpacing(8);
    topLay2_->addWidget(btn);
    connect(btn, &QToolButton::toggled, this,
            [this](bool on) { toggleContestMode(on); });
    contestBtn_ = btn;
}

void MainWindow::setupRotorUi() {
    // "ROT" dropdown: antenna rotator through rotctld (:4533) — the rose is
    // the pointing device. Clicking a spot (or the rose) sets the target;
    // TURN sends it, or auto-follow turns on every point. The cyan needle
    // on the rose is the antenna's actual heading.
    auto* rotBtn = new QToolButton(topStrip_);
    rotBtn->setText("ROT ▾");
    rotBtn->setPopupMode(QToolButton::InstantPopup);
    rotBtn->setFocusPolicy(Qt::NoFocus);
    rotBtn->setStyleSheet(QString(kSideBtnStyle));
    rotBtn->setToolTip("Antenna rotator via rotctld :4533\n(run rotctld -m "
                       "<model> -r <device>; the compass rose shows the "
                       "antenna as a cyan needle)");
    auto* rotMenu = new QMenu(rotBtn);
    styleMenu(rotMenu);
    auto* rotOn = rotMenu->addAction("Connect to rotctld");
    rotOn->setCheckable(true);
    auto* rotFollow = rotMenu->addAction("Antenna follows the rose");
    rotFollow->setCheckable(true);
    rotFollow->setToolTip("Every rose point (incl. spot clicks) turns the "
                          "rotator immediately.\nOff: manual rose clicks "
                          "still turn; spot clicks point only —\n"
                          "Ctrl+click a callsign to tune AND turn.");
    rotMenu->addSeparator();
    auto* rotW = new QWidget;
    rotW->setStyleSheet(
        "QWidget { background: #141b24; color: #c8d4e0; font-size: 13px; }"
        "QLabel { color: #8fa3b8; font-weight: bold; font-size: 12px; }"
        "QSpinBox { background: #1c2430; color: #c8d4e0; border: 1px solid"
        " #2a3644; border-radius: 3px; padding: 2px 6px; }"
        "QPushButton { background: #2f6d9e; border: 1px solid #5db2f0;"
        " border-radius: 3px; padding: 5px 14px; font-weight: bold; }"
        "QPushButton:hover { background: #3a7fb5; }"
        "QPushButton#stop { background: #8a2727; border-color: #e05d5d; }");
    auto* rg = new QGridLayout(rotW);
    rg->setContentsMargins(14, 12, 14, 12);
    rg->setHorizontalSpacing(10);
    rg->setVerticalSpacing(9);
    auto* rotState = new QLabel("rotor: not connected", rotW);
    rg->addWidget(rotState, 0, 0, 1, 3);
    auto* rotTurn = new QPushButton("TURN to rose", rotW);
    rotTurn->setToolTip("Send the rotator to the rose's red pointer");
    rg->addWidget(rotTurn, 1, 0, 1, 2);
    auto* rotStop = new QPushButton("STOP", rotW);
    rotStop->setObjectName("stop");
    rg->addWidget(rotStop, 1, 2);
    rg->addWidget(new QLabel("MANUAL", rotW), 2, 0);
    auto* rotAz = new QSpinBox(rotW);
    rotAz->setRange(0, 359);
    rotAz->setSuffix("°");
    rotAz->setWrapping(true);
    rg->addWidget(rotAz, 2, 1);
    auto* rotGo = new QPushButton("GO", rotW);
    rg->addWidget(rotGo, 2, 2);
    auto* rotAct = new QWidgetAction(rotMenu);
    rotAct->setDefaultWidget(rotW);
    rotMenu->addAction(rotAct);
    rotBtn->setMenu(rotMenu);
    lightWhile(rotBtn, rotMenu, [this] { return rotor_.connected(); });
    topLay2_->addSpacing(8);
    topLay2_->addWidget(rotBtn);
    const auto rotRefresh = [this, rotState] {
        QString s = rotor_.connected()
            ? (rotor_.azimuth() >= 0.0
                   ? QString("rotor: %1°").arg(qRound(rotor_.azimuth()))
                   : QString("rotor: connected"))
            : QString("rotor: not connected");
        if (lastRoseBearing_ >= 0.0)
            s += QString("   target: %1 %2°")
                     .arg(lastRoseLabel_)
                     .arg(qRound(lastRoseBearing_));
        rotState->setText(s);
    };
    rotor_.configure();
    rotOn->setChecked(QSettings().value("rotor/enabled", false).toBool());
    rotFollow->setChecked(QSettings().value("rotor/track", false).toBool());
    rotor_.setActive(rotOn->isChecked());
    connect(rotOn, &QAction::toggled, this, [this, rotRefresh](bool on) {
        QSettings().setValue("rotor/enabled", on);
        rotor_.setActive(on);
        rotRefresh();
    });
    connect(rotFollow, &QAction::toggled, this, [](bool on) {
        QSettings().setValue("rotor/track", on);
    });
    connect(&rotor_, &RotorLink::azimuthChanged, this,
            [this, rotRefresh](double az) {
                rose_->setRotorAz(az);
                rotRefresh();
            });
    connect(&rotor_, &RotorLink::connectedChanged, this,
            [this, rotRefresh](bool on) {
                statusBar()->showMessage(
                    on ? (rotor_.direct() ? "rotor: DCU-3 answering"
                                          : "rotor: rotctld connected")
                       : "rotor: link lost",
                    4000);
                rotRefresh();
            });
    connect(rose_, &CompassRoseWidget::bearingChanged, this,
            [this, rotFollow, rotRefresh](double b, const QString& label) {
                lastRoseBearing_ = b;
                lastRoseLabel_ = label;
                // A MANUAL rose click is an order, not a suggestion —
                // it turns immediately (operator: "make it go after I set
                // it instead of opening the panel"). Spot points still
                // only turn with auto-follow on, or via Ctrl+click.
                const bool go = b >= 0.0 && rotor_.connected()
                    && (rotFollow->isChecked()
                        || label == QLatin1String("MAN"));
                if (go) {
                    rotor_.turnTo(b);
                    statusBar()->showMessage(
                        QString("rotor -> %1° (%2)").arg(qRound(b))
                            .arg(label), 4000);
                } else if (b >= 0.0 && label == QLatin1String("MAN")
                           && !rotor_.connected()) {
                    // A dead click looked like a broken rotor (live-found
                    // 2026-10-22: the ROT box had been left unticked).
                    statusBar()->showMessage(
                        "rotor: not connected (ROT menu → Connect)", 6000);
                }
                rotRefresh();
            });
    // Ctrl+click on a spot callsign: tune AND turn (optional per click —
    // never automatic; plain click stays tune-only).
    connect(pan_, &PanadapterWidget::spotTurnRequested, this,
            [this, rotRefresh](double b, const QString& call) {
                if (b < 0.0) {
                    statusBar()->showMessage(
                        QString("no bearing for %1 (location unknown)")
                            .arg(call), 4000);
                    return;
                }
                if (!rotor_.connected()) {
                    statusBar()->showMessage(
                        "rotor: not connected (ROT menu)", 4000);
                    return;
                }
                rotor_.turnTo(b);
                statusBar()->showMessage(
                    QString("rotor -> %1° (%2)").arg(qRound(b)).arg(call),
                    5000);
                rotRefresh();
            });
    connect(rotTurn, &QPushButton::clicked, this, [this, rotRefresh] {
        if (lastRoseBearing_ < 0.0) {
            statusBar()->showMessage(
                "rotor: point the rose first (click a spot or the rose)",
                4000);
            return;
        }
        rotor_.turnTo(lastRoseBearing_);
        statusBar()->showMessage(QString("rotor -> %1° (%2)")
                                     .arg(qRound(lastRoseBearing_))
                                     .arg(lastRoseLabel_), 4000);
        rotRefresh();
    });
    connect(rotStop, &QPushButton::clicked, this,
            [this] { rotor_.stop(); statusBar()->showMessage("rotor: STOP", 3000); });
    connect(rotGo, &QPushButton::clicked, this, [this, rotAz] {
        rotor_.turnTo(rotAz->value());
        statusBar()->showMessage(
            QString("rotor -> %1°").arg(rotAz->value()), 4000);
    });
    connect(rotMenu, &QMenu::aboutToShow, this, rotRefresh);

}

} // namespace ttc

namespace ttc {

// The skimmer decodes whenever it is switched on — the dial's mode has
// nothing to say about it. "Pause outside CW" (2026-10-14) was removed
// 2026-10-20: once the CW window follows the dial's BAND, working phone
// is precisely when the bank is earning its keep, and the operator's rule
// is simply "if I don't want skimmer decodes I turn the skimmer off".
void MainWindow::applySkimGate() {
    if (!skim_ || !skimEnable_) return;
    skim_->setEnabled(skimEnable_->isChecked());
}

} // namespace ttc

namespace ttc {

#ifdef HAVE_WDSP
namespace {
// RX ▾ settings live under sdrrx/*; the enums' values are stored, so
// append-only like every stored enum here.
SdrRx::Settings loadSdrRxSettings() {
    QSettings q;
    SdrRx::Settings s;
    s.volumeDb = float(q.value("sdrrx/volumeDb", s.volumeDb).toDouble());
    s.agc = SdrRx::Agc(std::clamp(q.value("sdrrx/agc", int(s.agc)).toInt(), 0, 4));
    s.nb = SdrRx::NoiseBlanker(std::clamp(q.value("sdrrx/nb", 0).toInt(), 0, 2));
    s.nr = SdrRx::NoiseReduction(std::clamp(q.value("sdrrx/nr", 0).toInt(), 0, 3));
    s.snb = q.value("sdrrx/snb", false).toBool();
    s.anf = q.value("sdrrx/anf", false).toBool();
    s.rnnoise = q.value("sdrrx/rnnoise", false).toBool();
    // The RX EQ is stored as the voice keyer's EQ JSON (same 8 slots, same
    // validation of slot types); never configured = flat and off.
    const QJsonDocument eq = QJsonDocument::fromJson(q.value("sdrrx/eq").toByteArray());
    const VoiceEqSettings v = eq.isObject()
        ? VoiceEqSettings::fromJson(eq.object())
        : VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
    s.eqOn = v.enabled;
    for (size_t i = 0; i < s.eq.size(); ++i) s.eq[i] = v.bands[i];
    s.gateOn = q.value("sdrrx/gateOn", false).toBool();
    s.gateThrDb = std::clamp(float(q.value("sdrrx/gateThrDb", s.gateThrDb).toDouble()), -70.f, -10.f);
    s.gateFloorDb = std::clamp(float(q.value("sdrrx/gateFloorDb", s.gateFloorDb).toDouble()), -40.f, 0.f);
    s.compOn = q.value("sdrrx/compOn", false).toBool();
    s.compThrDb = std::clamp(float(q.value("sdrrx/compThrDb", s.compThrDb).toDouble()), -50.f, 0.f);
    s.compRatio = std::clamp(float(q.value("sdrrx/compRatio", s.compRatio).toDouble()), 1.f, 6.f);
    s.compMakeupDb = std::clamp(float(q.value("sdrrx/compMakeupDb", s.compMakeupDb).toDouble()), 0.f, 18.f);
    s.mute = q.value("sdrrx/mute", false).toBool();
    s.boost = q.value("sdrrx/boost", false).toBool();
    s.rade = q.value("sdrrx/rade", false).toBool();
    return s;
}
void saveSdrRxSettings(const SdrRx::Settings& s) {
    QSettings q;
    q.setValue("sdrrx/volumeDb", s.volumeDb);
    q.setValue("sdrrx/agc", int(s.agc));
    q.setValue("sdrrx/nb", int(s.nb));
    q.setValue("sdrrx/nr", int(s.nr));
    q.setValue("sdrrx/snb", s.snb);
    q.setValue("sdrrx/anf", s.anf);
    q.setValue("sdrrx/rnnoise", s.rnnoise);
    VoiceEqSettings v = VoiceEqSettings::preset(VoiceEqSettings::Preset::Flat);
    v.enabled = s.eqOn;
    for (size_t i = 0; i < s.eq.size(); ++i) v.bands[i] = s.eq[i];
    q.setValue("sdrrx/gateOn", s.gateOn);
    q.setValue("sdrrx/gateThrDb", s.gateThrDb);
    q.setValue("sdrrx/gateFloorDb", s.gateFloorDb);
    q.setValue("sdrrx/compOn", s.compOn);
    q.setValue("sdrrx/compThrDb", s.compThrDb);
    q.setValue("sdrrx/compRatio", s.compRatio);
    q.setValue("sdrrx/compMakeupDb", s.compMakeupDb);
    q.setValue("sdrrx/mute", s.mute);
    q.setValue("sdrrx/boost", s.boost);
    q.setValue("sdrrx/rade", s.rade);
    q.setValue("sdrrx/eq", QJsonDocument(v.toJson()).toJson(QJsonDocument::Compact));
}
}  // namespace
#endif

// The SDR receiver demodulates what the Orion is set to: its mode, its
// filter edges (the same edgesFromRig the panadapter draws) and, in CW,
// the sidetone pitch.
void MainWindow::updateSdrRxDemod() {
#ifdef HAVE_WDSP
    if (!sdrRx_) return;
    int lo = 0, hi = 0;
    edgesFromRig(rigMode_, rigBwHz_, rigPbtHz_, lo, hi);
    sdrRx_->setDemod(rigMode_, lo, hi, QSettings().value("cw/pitchHz", 550).toInt());
#endif
}

// SDR ▾ → Receive audio (WDSP): the RSP2's receive audio through WDSP —
// Thetis's receiver and noise tools — on the PC's speakers/headset. The
// Orion's own audio is left alone (operator's choice); the SDR runs ~50-
// 100 ms behind it, so hearing both at once is an echo. A submenu, not a
// second-deck button: the button took the window's floor to 1905 px, past
// the 1850 budget (the deck had no room left).
void MainWindow::setupSdrRxUi(QMenu* sdrMenu) {
#ifndef HAVE_WDSP
    (void)sdrMenu;
    return;                                        // built without FFTW/WDSP
#else
    sdrRx_ = new SdrRx;
    connect(this, &QObject::destroyed, this, [rx = sdrRx_] { delete rx; });
    sdrRx_->retune(-double(loOffHz_));
    updateSdrRxDemod();
    applySdrRxSettings();

    // One entry in SDR ▾ opens a window that stays up while you listen
    // (the first cut was a submenu, which closed on every click).
    sdrMenu->addSeparator();
    QAction* open = sdrMenu->addAction("Receive audio (WDSP)…");
    open->setToolTip("SDR receive audio: the panadapter's IQ demodulated by WDSP\n"
                     "(Thetis's DSP) with its noise blankers and noise reduction,\n"
                     "on the PC speakers/headset.");
    // persist = the operator's own on/off (remembered for next start);
    // FreeDV mode switches it without touching that (live-found: a session
    // that ended inside FreeDV came back with the SDR audio on, loud).
    const auto setOn = [this, open](bool v, bool persist = true) -> bool {
        if (persist) QSettings().setValue("sdrrx/on", v);
        if (v) {
            if (!sdrRx_->start(std::string())) {
                if (persist) QSettings().setValue("sdrrx/on", false);
                statusBar()->showMessage("SDR RX: could not start pacat", 8000);
                return false;
            }
            statusBar()->showMessage("SDR receive audio on (WDSP) — PC speakers/headset", 5000);
        } else {
            sdrRx_->stop();
            statusBar()->showMessage("SDR receive audio off", 3000);
        }
        open->setText(v ? "Receive audio (WDSP) — ON…" : "Receive audio (WDSP)…");
        return v;
    };
    sdrRxSetOn_ = [setOn](bool v) { return setOn(v, false); };   // FreeDV's, unsaved
    connect(open, &QAction::triggered, this, [this, setOn] {
        if (!sdrRxDlg_) {
            auto* dlg = new SdrRxDialog(QSettings().value("sdrrx/on", false).toBool(),
                                        loadSdrRxSettings(), this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            sdrRxDlg_ = dlg;
            dlg->setStatusSource([this] {
                return sdrRx_->running()
                    ? QString("queue %1 ms   level %2 dB   %3 blocks dropped")
                          .arg(sdrRx_->backlogMs(), 0, 'f', 0)
                          .arg(sdrRx_->outputPeakDb(), 0, 'f', 0)
                          .arg(sdrRx_->droppedBlocks())
                    : QStringLiteral("off");
            });
            dlg->setLevelSource([this] { return sdrRx_->levels(); });
            connect(dlg, &SdrRxDialog::changed, this,
                    [this, setOn](bool on, const SdrRx::Settings& s) {
                saveSdrRxSettings(s);
                applySdrRxSettings();              // + FDV's overrides; next block
                if (fdvWin_) fdvWin_->setVolume(int(std::lround(s.volumeDb)), s.mute);
                if (on != sdrRx_->running()) {
                    const bool got = setOn(on);
                    if (sdrRxDlg_) sdrRxDlg_->setOn(got);
                }
            });
        }
        sdrRxDlg_->show();
        sdrRxDlg_->raise();
        sdrRxDlg_->activateWindow();
    });
    setupCopyAssistUi(sdrMenu);                    // its tap goes in before start
    if (QSettings().value("sdrrx/on", false).toBool()) setOn(true);

    // Silent while transmitting: the RSP2 hears our own carrier. TX context
    // is the meter-poll TX state, the TX monitor's own record of it, VK
    // playback and MOX/TUNE (radioTx_ follows those within a poll).
    auto* txMute = new QTimer(this);
    txMute->setInterval(50);
    connect(txMute, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool tx = radioTx_ || dvrTxPlayback_
            || now - txCtxMs_.load(std::memory_order_relaxed) < 300;
        sdrRx_->setMuted(tx);
        // TTC_RXDBG=1: where the SDR receive audio stops (mute cause, WDSP
        // level into RADE, RADE sync), every 2 s.
        static const bool dbg = qEnvironmentVariableIsSet("TTC_RXDBG");
        static qint64 lastDbg = 0;
        if (dbg && now - lastDbg > 2000) {
            lastDbg = now;
            const SdrRx::RadeStatus r = sdrRx_->radeStatus();
            std::fprintf(stderr, "[rxdbg] run %d rade %d muted %d (radioTx %d dvr %d txCtx %lld ms ago) | "
                                 "WDSP %.0f dBFS  in %.0f  out %.0f | sync %d snr %.1f off %.0f Hz\n",
                         int(sdrRx_->running()), int(sdrRx_->radeDecoding()), int(tx), int(radioTx_),
                         int(dvrTxPlayback_), (long long)(now - txCtxMs_.load()),
                         sdrRx_->modemPeakDb(), sdrRx_->inputPeakDb(), sdrRx_->outputPeakDb(),
                         int(r.sync), r.snrDb, r.freqOffsetHz);
        }
    });
    txMute->start();

    // RADE end-of-over callsigns to the status bar as they decode.
    auto* radeCalls = new QTimer(this);
    radeCalls->setInterval(500);
    connect(radeCalls, &QTimer::timeout, this, [this, seen = 0]() mutable {
        const SdrRx::RadeStatus r = sdrRx_->radeStatus();
        if (r.calls == seen) return;
        seen = r.calls;
        if (r.calls > 0 && !r.lastCall.empty())
            statusBar()->showMessage(QString("RADE: heard %1 (end of over, SNR %2 dB)")
                                         .arg(QString::fromStdString(r.lastCall))
                                         .arg(r.snrDb, 0, 'f', 0), 15000);
    });
    radeCalls->start();
#endif
}

// SDR ▾ → Copy Assist: speech-to-text on the SDR receive audio (whisper,
// on this PC). Its tap sits before the gate/EQ/compressor and the MUTE,
// so the operator can listen to the Orion and still read the SDR's copy.
void MainWindow::setupCopyAssistUi(QMenu* sdrMenu) {
#if !defined(HAVE_WDSP) || !defined(HAVE_WHISPER)
    (void)sdrMenu;
#else
    asr_ = std::make_shared<AsrEngine>();
    sdrRx_->setAsrTap([a = asr_](const float* x, int n, bool muted) { a->feed48k(x, n, muted); });
    QAction* open = sdrMenu->addAction("Copy Assist (speech to text)…");
    open->setToolTip("A running transcript of the SDR receive audio (whisper.cpp,\n"
                     "on this PC), callsigns clickable into the log");
    const auto setOn = [this](bool on, int threads) {
        QSettings q;
        q.setValue("asr/on", on);
        q.setValue("asr/threads", threads);
        asr_->stop();
        if (!on) {
            if (caWin_) caWin_->setStatus("off");
            return;
        }
        const QString model = q.value("asr/model", AsrEngine::defaultModelPath()).toString();
        QString err;
        if (!asr_->start(model, threads, &err)) {
            q.setValue("asr/on", false);
            if (caWin_) { caWin_->setOn(false); caWin_->setStatus(err); }
            statusBar()->showMessage("Copy Assist: " + err, 8000);
            return;
        }
        if (caWin_)
            caWin_->setStatus(sdrRx_->running()
                ? QStringLiteral("listening…")
                : QStringLiteral("listening — turn on SDR ▾ → Receive audio to feed it"));
    };
    connect(open, &QAction::triggered, this, [this, setOn] {
        if (!caWin_) {
            QSettings q;
            auto* w = new CopyAssistWindow(asr_->running(), q.value("asr/threads", 8).toInt(), this);
            w->setAttribute(Qt::WA_DeleteOnClose);
            caWin_ = w;
            w->setStatusFor([this](const QString& call, qint64 hz) {
                return logbook_.ready()
                    ? logbook_.status(call, LogbookIndex::bandForHz(hz)).toLatin1() : '?';
            });
            connect(w, &CopyAssistWindow::enableChanged, this, setOn);
            connect(w, &CopyAssistWindow::callClicked, this,
                    [this](const QString& c) { openLogWindow(c); });
            w->setStatus(asr_->running() ? "listening…" : "off");
        }
        caWin_->show();
        caWin_->raise();
        caWin_->activateWindow();
    });
    connect(asr_.get(), &AsrEngine::transcribed, this, [this](const AsrEngine::Result& r) {
        if (caWin_) caWin_->addResult(r);
    });
    // Prompt hints: the calls on the band map within 3 kHz of the dial,
    // nearest first, plus our own (it gets said a lot).
    auto* hints = new QTimer(this);
    hints->setInterval(2000);
    connect(hints, &QTimer::timeout, this, [this] {
        if (!asr_->running()) return;
        const qint64 dial = qint64(centerHz_);
        QVector<QPair<qint64, QString>> near;
        for (const SpotLabel& l : shownSpots_) {
            const qint64 d = std::llabs(l.hz - dial);
            if (d <= 3000) near.push_back({d, l.call});
        }
        std::sort(near.begin(), near.end());
        QStringList calls;
        for (const auto& n : near)
            if (!calls.contains(n.second) && calls.size() < 8) calls << n.second;
        if (const QString me = QSettings().value("station/callsign").toString(); !me.isEmpty()) calls << me;
        asr_->setHints(calls, dial);
        if (caWin_ && sdrRx_->running())
            caWin_->setStatus(QString("listening — %1 s queued; hints: %2")
                                  .arg(asr_->queuedSeconds(), 0, 'f', 0).arg(calls.join(' ')));
    });
    hints->start();
    QSettings q;
    if (q.value("asr/on", false).toBool()) setOn(true, q.value("asr/threads", 8).toInt());
#endif
}

// AUDIO ▾ → Winlink: radio email through Pat (getpat.io) + the ARDOP
// modem (ardopcf), both the operator's own programs. The console's part,
// single-master as everywhere: Pat tunes and keys THROUGH our rigctld on
// :4532 (ardopcf has no PTT of its own; Pat relays its PTT requests); the
// ARDOP modem shares the SignaLink through PipeWire (ALSA device
// "pipewire:NODE=<node>", tested: 12 kHz capture alongside the console)
// rather than grabbing it with plughw; and while Pat's session is up the
// console hands it the SignaLink (DIG on, LIVE/RADE/VK off and refused).
void MainWindow::setupWinlinkUi(QMenu* menu) {
    QMenu* wl = menu->addMenu("Winlink (Pat + ARDOP)");
    QAction* ardopAct = wl->addAction("Start the ARDOP modem (ardopcf) on the SignaLink");
    ardopAct->setCheckable(true);
    ardopAct->setToolTip("ardopcf on port 8515, SignaLink shared through PipeWire, no PTT of\n"
                         "its own (Pat keys through the console). Install: yay -S ardopcf-bin");
    connect(ardopAct, &QAction::toggled, this, [this, ardopAct](bool on) {
        const auto refuse = [this, ardopAct](const QString& why) {
            const QSignalBlocker b(ardopAct);
            ardopAct->setChecked(false);
            statusBar()->showMessage("ARDOP: " + why, 10000);
        };
        if (!on) {
            if (ardop_) {
                ardop_->disconnect(this);
                ardop_->terminate();
                if (!ardop_->waitForFinished(1500)) ardop_->kill();
                ardop_->deleteLater();
                ardop_ = nullptr;
            }
            statusBar()->showMessage("ARDOP modem stopped", 4000);
            return;
        }
        const QString exe = QStandardPaths::findExecutable("ardopcf");
        if (exe.isEmpty()) return refuse("ardopcf not installed (yay -S ardopcf-bin)");
        if (radioSink_.isEmpty() || radioSource_.isEmpty())
            return refuse("SignaLink not found in PipeWire");
        const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ardop";
        QDir().mkpath(logDir);
        const QString call = QSettings().value("station/callsign", "").toString().toUpper();
        if (call.isEmpty()) return refuse("set your callsign first (Setup → Station)");
        ardop_ = new QProcess(this);
        ardop_->setProcessChannelMode(QProcess::MergedChannels);
        ardop_->setStandardOutputFile(logDir + "/ardopcf-console.log");
        connect(ardop_, &QProcess::finished, this, [this, ardopAct](int code, QProcess::ExitStatus) {
            if (ardop_) { ardop_->deleteLater(); ardop_ = nullptr; }
            const QSignalBlocker b(ardopAct);
            ardopAct->setChecked(false);
            statusBar()->showMessage(QString("ARDOP modem exited (%1) — see its log in "
                                             "~/.local/share/n8mus/tentec-console/ardop").arg(code), 10000);
        });
        ardop_->start(exe, {"--logdir", logDir, "-G", "8514", "-H", "MYCALL " + call, "8515",
                            "pipewire:NODE=" + radioSource_, "pipewire:NODE=" + radioSink_});
        if (!ardop_->waitForStarted(3000)) {
            ardop_->deleteLater();
            ardop_ = nullptr;
            return refuse("ardopcf would not start");
        }
        statusBar()->showMessage("ARDOP modem up on :8515 (levels page: localhost:8514). "
                                 "Connect from Pat's web page.", 10000);
    });
    connect(wl->addAction("Open Pat (mailbox, connect) in the browser"), &QAction::triggered,
            this, [] { QDesktopServices::openUrl(QUrl("http://localhost:8080")); });
    connect(wl->addAction("Open the ARDOP level page"), &QAction::triggered,
            this, [] { QDesktopServices::openUrl(QUrl("http://localhost:8514")); });
    connect(wl->addAction("Pat settings for this station…"), &QAction::triggered, this, [this] {
        const QString call = QSettings().value("station/callsign", "").toString().toUpper();
        const QString grid = QSettings().value("station/grid", "").toString();
        const QString snippet = QString(
            "  \"mycall\": \"%1\",\n"
            "  \"locator\": \"%2\",\n"
            "  \"hamlib_rigs\": {\n"
            "    \"console\": {\"address\": \"127.0.0.1:4532\", \"network\": \"tcp\", \"VFO\": \"\"}\n"
            "  },\n"
            "  \"ardop\": {\n"
            "    \"addr\": \"localhost:8515\",\n"
            "    \"arq_bandwidth\": {\"Forced\": false, \"Max\": 500},\n"
            "    \"rig\": \"console\",\n"
            "    \"ptt_ctrl\": true,\n"
            "    \"beacon_interval\": 0,\n"
            "    \"cwid_enabled\": true\n"
            "  },\n").arg(call, grid);
        auto* d = new QDialog(this);
        d->setAttribute(Qt::WA_DeleteOnClose);
        d->setWindowTitle("Pat settings for this station");
        auto* lay = new QVBoxLayout(d);
        auto* note = new QLabel(
            "In Pat (<b>pat configure</b>, or its web page → Config) set these fields — the\n"
            "rig is this console's rigctld on :4532, so Pat tunes and keys through it.\n"
            "Your Winlink password goes in <b>secure_login_password</b> — enter it yourself.\n"
            "First time: <b>pat connect telnet</b> (internet, no radio) creates the account.\n"
            "ARDOP starts at 500 Hz (sure to fit the Orion's TX filter); 1000/2000 once it works.", d);
        note->setTextFormat(Qt::RichText);
        note->setWordWrap(true);
        lay->addWidget(note);
        auto* ed = new QPlainTextEdit(snippet, d);
        ed->setReadOnly(true);
        ed->setFont(QFont("monospace"));
        ed->setMinimumSize(520, 260);
        lay->addWidget(ed);
        auto* bb = new QDialogButtonBox(d);
        auto* copy = bb->addButton("Copy", QDialogButtonBox::ActionRole);
        bb->addButton(QDialogButtonBox::Close);
        connect(copy, &QPushButton::clicked, d, [snippet] { QApplication::clipboard()->setText(snippet); });
        connect(bb, &QDialogButtonBox::rejected, d, &QDialog::close);
        lay->addWidget(bb);
        d->show();
    });
    wl->addSeparator();
    QAction* watch = wl->addAction("Hand Pat the SignaLink during its sessions");
    watch->setCheckable(true);
    watch->setToolTip("Watches Pat's own web socket (localhost:8080): while a Winlink session\n"
                      "dials or is connected, DIG goes on and LIVE / RADE / VK / MOX stand down");
    watch->setChecked(QSettings().value("winlink/watch", true).toBool());
    connect(watch, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("winlink/watch", on);
        pat_.setEnabled(on);
    });
    wlLabel_ = new QLabel(this);
    wlLabel_->setStyleSheet("QLabel { color: #e8b04a; font-weight: bold; padding: 0 6px; }");
    wlLabel_->hide();
    statusBar()->addPermanentWidget(wlLabel_);
    connect(&pat_, &PatMonitor::sessionChanged, this, &MainWindow::onWinlinkSession);
    connect(&pat_, &PatMonitor::progressChanged, this, [this](const QString& t) {
        if (!winlinkActive_) return;
        wlLabel_->setText(QString("WINLINK %1%2").arg(pat_.remote(), t.isEmpty() ? QString() : "  " + t));
    });
    pat_.setEnabled(watch->isChecked());
}

void MainWindow::onWinlinkSession(bool active, const QString& remote) {
    if (active == winlinkActive_) return;
    if (active && fdvMode_) setFdvMode(false);     // Pat's session wins the SignaLink
    winlinkActive_ = active;
    if (active) {
        // The SignaLink is Pat's now: nothing of ours plays into it.
        stopVoicePlayback();
        winlinkStoppedLive_ = liveMic_;
        if (liveMic_) setLiveMic(false);           // RADE TX goes with it
        if (!digital_) {
            winlinkOwnsDig_ = true;
            setDigitalMode(true);                  // rear line input
        }
        wlLabel_->setText(QString("WINLINK %1").arg(remote.isEmpty() ? QStringLiteral("dialing…") : remote));
        wlLabel_->show();
        statusBar()->showMessage(QString("Winlink session %1 — the SignaLink is Pat's; LIVE/VK/MOX "
                                         "stand down").arg(remote), 8000);
    } else {
        if (winlinkOwnsDig_) {
            winlinkOwnsDig_ = false;
            setDigitalMode(false);
        }
        wlLabel_->hide();
        statusBar()->showMessage(winlinkStoppedLive_
            ? QString("Winlink session with %1 ended — LIVE was turned off for it; turn it back "
                      "on when you want it").arg(remote)
            : QString("Winlink session with %1 ended").arg(remote), 10000);
        winlinkStoppedLive_ = false;
    }
}

// Who owns the qso.freedv.org link: the spot view (SPOTS ▾ → FreeDV
// Reporter spots), and the FreeDV mode — its station list, and reporting
// when the window's "Report my station" is on (like freedv-gui, which
// reports only while it is started).
void MainWindow::updateFreedvLink() {
    const bool spotsWanted = spotsOnAct_ && spotsOnAct_->isChecked()
                          && fdvSpotsAct_ && fdvSpotsAct_->isChecked();
    QSettings q;
    const QString call = q.value("station/callsign", "").toString().toUpper();
    // Never report a station with no call (a fresh install has none).
    const bool report = fdvMode_ && !call.isEmpty()
                     && q.value("freedv/report", false).toBool();
    const QString grid = q.value("station/grid", "").toString();
    const QString sw = QStringLiteral("tentec-console " TTC_VERSION);
    freedv_.setReporting(report, call, grid, sw, true);
    psk_.setIdentity(call, grid, sw);
    psk_.setEnabled(report);
    freedv_.setEnabled(spotsWanted || fdvMode_);   // FDV's station list needs it
    if (fdvWin_) fdvWin_->setReportingLive(report, QString("%1 (%2)").arg(call, grid));
    if (report != fdvReportingNow_) {
        fdvReportingNow_ = report;
        statusBar()->showMessage(report
            ? QString("FreeDV: reporting as %1 (%2) to qso.freedv.org and PSK Reporter").arg(call, grid)
            : QStringLiteral("FreeDV: reporting off"), 6000);
    }
}

// The receiver as the operator set it (SDR ▾ → Receive audio), with the
// FreeDV mode's overrides on top while it runs: RADE decode, and none of
// the noise tools — NR/NB/ANF/SNB/RNNoise distort the modem. Never saved:
// leaving FDV re-applies the operator's own settings.
Mode MainWindow::fdvSideband(uint64_t hz) {
    // Below 10 MHz the voice convention (LSB) — FreeDV follows it too —
    // except 60 m, channelized USB. Live-found: "FreeDV is USB on every
    // band" was wrong; their own table says LSB on 160/80/40.
    const bool sixty = hz >= 5250000 && hz <= 5450000;
    return hz < 10000000 && !sixty ? Mode::LSB : Mode::USB;
}

void MainWindow::applySdrRxSettings() {
#ifdef HAVE_WDSP
    if (!sdrRx_) return;
    SdrRx::Settings s = loadSdrRxSettings();
    s.rade = fdvMode_;
    s.radeAnalog = fdvMode_ && fdvAnalog_;
    if (fdvMode_) {
        s.nb = SdrRx::NoiseBlanker::Off;
        s.nr = SdrRx::NoiseReduction::Off;
        s.snb = s.anf = s.rnnoise = false;
    }
    sdrRx_->setSettings(s);
#endif
}

void MainWindow::setFdvMode(bool on, bool keepMode) {
    if (on == fdvMode_) { panel_->showFdv(on); return; }
#if !defined(HAVE_WDSP) || !defined(HAVE_RADE)
    (void)keepMode;
    panel_->showFdv(false);
    statusBar()->showMessage("FreeDV: not in this build (needs WDSP + RADE)", 8000);
    return;
#else
    if (on) {
        if (winlinkActive_) {
            panel_->showFdv(false);
            statusBar()->showMessage("FreeDV refused: a Winlink session owns the SignaLink", 8000);
            return;
        }
        if (!sdrRx_ || !sdrRxSetOn_) {
            panel_->showFdv(false);
            statusBar()->showMessage("FreeDV needs the SDR receiver", 8000);
            return;
        }
        if (samActive_) {                          // one pseudo-mode at a time
            samActive_ = false;
            panel_->showSam(false);
            panel_->setSamLabel("SAM");
        }
        fdvSaved_ = {rigMode_, rigBwHz_, rigPbtHz_, txBwHz_, sdrRx_->running(), liveMic_};
        // Radio: the band's FreeDV sideband with a filter wide enough for
        // the modem (RADE sits at ~700-2300 Hz audio), TX filter likewise.
        fdvSb_ = fdvSideband(centerHz_);
        applyMode(fdvSb_);
        panel_->clearModeSelection();
        QTimer::singleShot(450, this, [this] {
            if (!fdvMode_) return;
            radio_->setBandwidthHz(Rx::Main, 2800);
            radio_->setPbtHz(Rx::Main, 0);
        });
        if (txBwHz_ < 2800) { radio_->setTxFilter(2800); txBar_->showTxFilter(2800); }
        // TX power is the operator's: FreeDV leaves it where he set it.
        // Transmit: voice LIVE makes way for RADE on the same path (DIG
        // comes with it: line input, mic + processor off).
        if (liveMic_) setLiveMic(false);
        fdvMode_ = true;
        setRadeTx(true);
        if (!radeTxOn_) {                          // couldn't arm (no SignaLink?):
            setFdvMode(false);                     // unwind — fdvMode_ is still true,
            return;                                // so this restores the radio
        }
        if (live_) live_->setRadeLevelDb(float(QSettings().value("fdv/txLevelDb", 0).toInt()));
        // Receive: the SDR decodes RADE.
        fdvAnalog_ = false;
        applySdrRxSettings();
        if (!sdrRx_->running()) sdrRxSetOn_(true);
        panel_->showFdv(true);
        updateFreedvLink();
        openFreeDvWindow();
        statusBar()->showMessage(QString("FreeDV: %1, wide filter, processor off. Key with MOX / space; "
                                 "your call goes out at the end of each over. (RADE runs a heavy "
                                 "duty cycle — mind the power.)").arg(fdvSb_ == Mode::LSB ? "LSB" : "USB"), 12000);
    } else {
        fdvMode_ = false;
        fdvAnalog_ = false;
        setRadeTx(false);                          // DIG goes back with LIVE
        if (fdvSaved_.live) setLiveMic(true);
        // Back to the Orion: the SDR audio goes off whatever it was before
        // (operator's call — outside FreeDV he listens to the radio) — and
        // FIRST, before the receiver leaves RADE, or the plain sideband
        // blasts out for a moment (live-found).
        if (sdrRx_->running()) sdrRxSetOn_(false);
        if (sdrRxDlg_) sdrRxDlg_->setOn(false);
        applySdrRxSettings();
        if (fdvSaved_.txBw != txBwHz_) {
            radio_->setTxFilter(fdvSaved_.txBw);
            txBar_->showTxFilter(fdvSaved_.txBw);
        }
        if (!keepMode) {
            // SSB comes back on the sideband for the band we're on NOW
            // (entered on 20 m USB, left on 40 m -> LSB: live-found USB
            // on 40); CW/AM/FM come back as they were.
            Mode back = fdvSaved_.mode;
            if (back == Mode::USB || back == Mode::LSB) back = fdvSideband(centerHz_);
            applyMode(back);
            panel_->showMode(back);
            QTimer::singleShot(450, this, [this, bw = fdvSaved_.bw, pbt = fdvSaved_.pbt] {
                radio_->setBandwidthHz(Rx::Main, bw);
                radio_->setPbtHz(Rx::Main, pbt);
            });
        }
        panel_->showFdv(false);
        updateFreedvLink();
        if (fdvWin_) fdvWin_->close();
        statusBar()->showMessage("FreeDV off — back on the Orion: mode for this band, filter, "
                                 "processor restored; SDR audio off", 8000);
    }
#endif
}

void MainWindow::openFreeDvWindow() {
    if (!fdvWin_) {
        auto* w = new FreeDvWindow(this);
        w->setAttribute(Qt::WA_DeleteOnClose);
        fdvWin_ = w;
        QSettings q;
        w->setMyGrid(q.value("station/grid", "").toString());
        w->setControls(fdvAnalog_, q.value("fdv/txLevelDb", 0).toInt(),
                       q.value("freedv/report", false).toBool(), q.value("fdv/message").toString());
        w->setStatusFor([this](const QString& call, qint64 hz) {
            return logbook_.ready() ? logbook_.status(call, LogbookIndex::bandForHz(hz)).toLatin1() : '?';
        });
        connect(w, &FreeDvWindow::analogChanged, this, [this](bool on) {
            fdvAnalog_ = on;
            applySdrRxSettings();
        });
#ifdef HAVE_WDSP
        {
            const SdrRx::Settings rs = loadSdrRxSettings();
            w->setVolume(int(std::lround(rs.volumeDb)), rs.mute);
        }
        connect(w, &FreeDvWindow::volumeChanged, this, [this](int db, bool mute) {
            SdrRx::Settings rs = loadSdrRxSettings();
            rs.volumeDb = float(db);
            rs.mute = mute;
            saveSdrRxSettings(rs);
            applySdrRxSettings();
            if (sdrRxDlg_) sdrRxDlg_->setVolumeMute(db, mute);
        });
#endif
        connect(w, &FreeDvWindow::txLevelChanged, this, [this](int db) {
            QSettings().setValue("fdv/txLevelDb", db);
            if (live_) live_->setRadeLevelDb(float(db));
        });
        connect(w, &FreeDvWindow::reportChanged, this, [this](bool on) {
            QSettings().setValue("freedv/report", on);
            updateFreedvLink();
        });
        connect(w, &FreeDvWindow::statusMessageSent, this, [this](const QString& t) {
            QSettings().setValue("fdv/message", t);
            freedv_.sendStatusMessage(t);
            statusBar()->showMessage(fdvReportingNow_ ? "FreeDV: message sent"
                                                      : "FreeDV: message saved — it goes out "
                                                        "once Report my station is on", 5000);
        });
        connect(w, &FreeDvWindow::qsyTo, this, [this](qint64 hz, const QString& call) {
            tuneAbsolute(uint64_t(hz));
            statusBar()->showMessage(QString("FreeDV: QSY to %1 on %2 kHz").arg(call)
                                         .arg(hz / 1000.0, 0, 'f', 1), 5000);
        });
        connect(w, &FreeDvWindow::lookupCall, this, [](const QString& c) {
            QDesktopServices::openUrl(QUrl("https://www.qrz.com/db/" + c));
        });
        connect(w, &FreeDvWindow::turnRotor, this, [this](double az) { rotor_.turnTo(az); });
        connect(w, &FreeDvWindow::requestQsy, this, [this](const QString& sid, const QString& call) {
            const qint64 hz = qint64(centerHz_);
            freedv_.requestQsy(sid, hz, QString("%1 kHz?").arg(hz / 1000.0, 0, 'f', 1));
            statusBar()->showMessage(QString("FreeDV: asked %1 to QSY to %2 kHz").arg(call)
                                         .arg(hz / 1000.0, 0, 'f', 1), 6000);
        });
        connect(w, &FreeDvWindow::logQso, this,
                [this](const QString& call, const QString& grid, qint64, int) {
            openLogWindow(call, QString(), grid);
            if (logWin_) logWin_->setModeHint("DIGITALVOICE");
        });
    }
    fdvWin_->show();
    fdvWin_->raise();
    fdvWin_->activateWindow();
}

namespace {
// The MAIN process running `exe` (by /proc/*/exe), or 0. Electron's helper
// processes (zygote, GPU, renderers) share the binary but carry --type=.
qint64 findRunning(const QString& exe) {
    const QString want = QFileInfo(exe).canonicalFilePath();
    if (want.isEmpty()) return 0;
    for (const QString& d : QDir("/proc").entryList(QDir::Dirs)) {
        bool num = false;
        const qint64 pid = d.toLongLong(&num);
        if (!num) continue;
        if (QFileInfo("/proc/" + d + "/exe").canonicalFilePath() != want) continue;
        QFile cl("/proc/" + d + "/cmdline");
        if (cl.open(QIODevice::ReadOnly) && cl.readAll().contains("--type=")) continue;
        return pid;
    }
    return 0;
}

// X11 only (this station runs Cinnamon on X): xdotool raises the windows.
void raisePid(qint64 pid) {
    if (pid <= 0 || QStandardPaths::findExecutable("xdotool").isEmpty()) return;
    QProcess::startDetached("xdotool", {"search", "--onlyvisible", "--pid",
                                        QString::number(pid), "windowactivate", "%@"});
}

// The real binaries (wsjtx is /usr/bin; GridTracker2's launcher script
// execs /opt/GridTracker2/gridtracker2.bin).
QString wsjtxExe() { return QStandardPaths::findExecutable("wsjtx"); }
QString gridTrackerLauncher() { return QStandardPaths::findExecutable("gridtracker2"); }
QString gridTrackerExe() {
    const QString l = gridTrackerLauncher();
    if (l.isEmpty()) return {};
    return QFileInfo(QFileInfo(l).canonicalFilePath()).absolutePath() + "/gridtracker2.bin";
}
} // namespace

// FT8 (MODE panel, under DIG): one press starts WSJT-X and GridTracker2,
// or brings them to the front when they were already running; the NEXT
// press closes them — WSJT-X through its own UDP Close (a clean exit:
// settings saved, jt9 gone), GridTracker with SIGTERM. Both are started
// DETACHED so a console restart never takes them down. Lit while WSJT-X
// is heard on the multicast feed (224.0.0.1:2237). It replaces cqrlog's
// remote-mode launch; the QSOs log here over the same feed.
void MainWindow::setupWsjtxUi() {
    connect(panel_, &ControlPanel::ft8Clicked, this, [this] {
        static bool closeNext = false;
        const qint64 wsPid = findRunning(wsjtxExe());
        const qint64 gtPid = findRunning(gridTrackerExe());
        if (!wsPid && !gtPid) closeNext = false;
        if ((wsPid || gtPid) && closeNext) {
            closeNext = false;
            // Already up and raised once: this press closes them.
            if (wsPid && !(wsjtx_ && wsjtx_->requestClose()))
                ::kill(pid_t(wsPid), SIGTERM);      // never heard: plain TERM
            if (gtPid) ::kill(pid_t(gtPid), SIGTERM);
            statusBar()->showMessage("FT8: closing WSJT-X and GridTracker2", 6000);
            return;
        }
        closeNext = true;
        QStringList started, missing;
        const auto launch = [&](const QString& exe, const QString& name, qint64 pid) {
            if (pid) { raisePid(pid); return; }
            if (exe.isEmpty() || !QProcess::startDetached(exe, {})) {
                missing << name;
                return;
            }
            started << name;
        };
        launch(wsjtxExe(), "WSJT-X", wsPid);
        launch(gridTrackerLauncher(), "GridTracker2", gtPid);
        QString msg = started.isEmpty() ? QStringLiteral("FT8: WSJT-X and GridTracker2 to the front"
                                                         " (press again to close them)")
                                        : "FT8: starting " + started.join(" + ");
        if (!missing.isEmpty()) msg += "  — not found: " + missing.join(", ");
        statusBar()->showMessage(msg, 8000);
    });
}

bool MainWindow::wsjtxAlive() const { return wsjtx_ && wsjtx_->alive(); }
bool MainWindow::wsjtxTransmitting() const { return wsjtx_ && wsjtx_->transmitting(); }

// WSJT-X owns the SignaLink while it runs, the way a Winlink session does:
// DIG on (rear line input) and LIVE off. Only the DIG it turned on itself
// is turned back off. TX refusals key off transmitting() at the moment.
void MainWindow::onWsjtxState(bool alive, bool transmitting) {
    panel_->showFt8(alive);
    if (alive && transmitting) stopVoicePlayback();
    if (alive && !wsjtxOwnsDig_ && !digital_ && !winlinkActive_) {
        if (liveMic_) setLiveMic(false);
        wsjtxOwnsDig_ = true;
        setDigitalMode(true);
        statusBar()->showMessage("WSJT-X is running — DIG on (the SignaLink is its)", 6000);
    } else if (alive && liveMic_) {
        setLiveMic(false);
    } else if (!alive && wsjtxOwnsDig_) {
        wsjtxOwnsDig_ = false;
        if (digital_ && !winlinkActive_) setDigitalMode(false);
        statusBar()->showMessage("WSJT-X closed — DIG off", 6000);
    }
}

} // namespace ttc
