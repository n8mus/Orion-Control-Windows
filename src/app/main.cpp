// SPDX-License-Identifier: GPL-2.0-or-later
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#include "app/MainWindow.h"

namespace {

// Fatal-signal handler: a poor-man's stack trace onto stderr (which points
// at the session log when launched from a desktop icon — see below), so a
// tester's crash report carries evidence even before anyone reaches for
// coredumpctl. backtrace_symbols_fd is async-signal-safe; the fancy
// alternatives are not.
void fatalSignal(int sig) {
    const char* name = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGABRT ? "SIGABRT"
                       : sig == SIGFPE  ? "SIGFPE"
                                        : "signal";
    char head[64];
    const int n = snprintf(head, sizeof head, "\n=== FATAL %s ===\n", name);
    (void)!write(2, head, size_t(n));
    void* frames[48];
    const int cnt = backtrace(frames, 48);
    backtrace_symbols_fd(frames, cnt, 2);
    signal(sig, SIG_DFL);                  // fall through to the core dump
    raise(sig);
}

// Qt messages (qWarning/qCritical from Qt itself and the libraries) get a
// timestamp and land on stderr alongside our own fprintf diagnostics.
void qtMessage(QtMsgType, const QMessageLogContext&, const QString& msg) {
    fprintf(stderr, "[%s] %s\n",
            qPrintable(QDateTime::currentDateTime().toString("hh:mm:ss.zzz")),
            qPrintable(msg));
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("n8mus");     // QSettings (band memories etc.)
    QApplication::setApplicationName("tentec-console");
    QApplication::setWindowIcon(QIcon(":/icon.png"));

    // Session log: when stderr is NOT a terminal (desktop-icon launch),
    // redirect it to a per-session file so "it crashed / it printed
    // something" reports come with the actual output. Terminal launches
    // keep printing to the terminal (the README asks testers to run there).
    // Previous session is kept as console.log.1.
    if (!isatty(2) && !std::getenv("TTC_SELFTEST")) {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(dir);
        const QString log = dir + "/console.log";
        QFile::remove(log + ".1");
        QFile::rename(log, log + ".1");
        if (freopen(qPrintable(log), "w", stderr))
            setvbuf(stderr, nullptr, _IOLBF, 0);   // line-buffered: crash-safe
    }
    qInstallMessageHandler(qtMessage);
    signal(SIGSEGV, fatalSignal);
    signal(SIGABRT, fatalSignal);
    signal(SIGFPE, fatalSignal);
    fprintf(stderr, "USS Orion console starting  %s  (built %s %s)\n",
            qPrintable(QDateTime::currentDateTime().toString(Qt::ISODate)),
            __DATE__, __TIME__);

    // Pin the selftest font BEFORE any widget exists, so the layout-budget
    // number is the same ruler on every platform: Ubuntu's default metrics
    // measured 1841 where Arch measured 1808 for identical code (first CI
    // run). DejaVu Sans ships on both.
    if (std::getenv("TTC_SELFTEST"))
        QApplication::setFont(QFont("DejaVu Sans", 10));

    ttc::MainWindow w;
    w.resize(1360, 600);                   // room for the dual-VFO/routing strip
    if (std::getenv("TTC_MAXIMIZED")) {    // selftest: layout at full screen
        w.showMaximized();                 // (no WM headless: force the
        w.resize(w.screen()->size());      //  geometry so the grab is honest)
    } else {
        w.show();
    }

    // Headless self-test: TTC_SELFTEST=<seconds> runs the full pipeline then quits
    // cleanly (so the SDR device is released). Used for CI / no-display verification.
    // TTC_SCREENSHOT=<path.png> additionally grabs the window just before quitting
    // (works on the offscreen platform) for layout review without a display.
    if (const char* s = std::getenv("TTC_SELFTEST")) {
        QTimer::singleShot(atoi(s) * 1000, &app, [&app, &w] {
            // Width budget: the layout minimum is what decides whether the
            // window still fits a screen (and keeps its maximize button).
            fprintf(stderr, "[layout] window %dx%d  layout-min %dx%d\n",
                    w.width(), w.height(),
                    w.minimumSizeHint().width(), w.minimumSizeHint().height());
            if (const char* shot = std::getenv("TTC_SCREENSHOT"))
                w.grab().save(QString::fromLocal8Bit(shot));
            app.quit();
        });
    }

    return app.exec();
}
