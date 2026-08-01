// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/Voacap.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <cmath>

namespace ttc {

namespace {

// Radial sample grid. 24 azimuths x 17 ranges = 408 circuits; one
// voacapl process chews through them in a couple of seconds because the
// coefficients load once per process, not per circuit.
constexpr int kAzStep = 15;
constexpr int kAzCount = 360 / kAzStep;
constexpr int kDistKm[] = {250,  500,  750,  1000, 1250, 1500, 1750, 2000,
                           2500, 3000, 3500, 4000, 5000, 6000, 8000, 10000,
                           12000};
constexpr int kDistCount = int(sizeof(kDistKm) / sizeof(kDistKm[0]));

// The AppImage bundles voacapl (US-government public-domain core, so
// unlike the SDRplay lib it MAY be redistributed) plus a ready itshfbc
// tree. An operator's own installation always wins over the bundle.
QString voacaplPath() {
    QString p = QStandardPaths::findExecutable("voacapl");
    if (!p.isEmpty()) return p;
    for (const QString& c : {QDir::homePath() + "/.local/bin/voacapl",
                             QCoreApplication::applicationDirPath()
                                 + "/voacapl"})
        if (QFile::exists(c)) return c;
    return {};
}

bool copyTree(const QString& src, const QString& dst) {
    QDir().mkpath(dst);
    const QDir s(src);
    for (const QFileInfo& fi :
         s.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.isDir()) {
            if (!copyTree(fi.absoluteFilePath(), dst + "/" + fi.fileName()))
                return false;
        } else if (!QFile::exists(dst + "/" + fi.fileName())
                   && !QFile::copy(fi.absoluteFilePath(),
                                   dst + "/" + fi.fileName())) {
            return false;
        }
    }
    return true;
}

QString itshfbcDir() {
    // 1) the operator's own tree; 2) our writable working copy; 3) seed
    // that copy from the read-only tree bundled in the AppImage (voacapl
    // writes decks and results into <root>/run, so a mounted AppImage
    // can't be the root itself).
    const QString own = QDir::homePath() + "/itshfbc";
    if (QDir(own + "/run").exists()) return own;
    const QString mine = QStandardPaths::writableLocation(
                             QStandardPaths::AppDataLocation) + "/itshfbc";
    if (QDir(mine + "/run").exists()) return mine;
    const QString bundled = QCoreApplication::applicationDirPath()
                            + "/../share/tentec-console/itshfbc";
    if (QDir(bundled + "/run").exists() && copyTree(bundled, mine)
        && QDir(mine + "/run").exists())
        return mine;
    return {};
}

void forward(double lat1d, double lon1d, double azDeg, double km,
             double& lat2d, double& lon2d) {
    const double lat1 = lat1d * M_PI / 180.0, lon1 = lon1d * M_PI / 180.0;
    const double d = km / 6371.0, az = azDeg * M_PI / 180.0;
    const double lat2 = std::asin(std::sin(lat1) * std::cos(d)
                        + std::cos(lat1) * std::sin(d) * std::cos(az));
    const double lon2 = lon1
        + std::atan2(std::sin(az) * std::sin(d) * std::cos(lat1),
                     std::cos(d) - std::sin(lat1) * std::sin(lat2));
    lat2d = lat2 * 180.0 / M_PI;
    lon2d = std::fmod(lon2 * 180.0 / M_PI + 540.0, 360.0) - 180.0;
}

// VOACAP cards are fixed-column Fortran; replicate the sample deck's
// exact field widths (a stray character crashes the engine — live-found).
QString circuitCard(double la1, double lo1, double la2, double lo2) {
    auto f5 = [](double v, char p, char n) {
        return QString("%1%2").arg(std::abs(v), 5, 'f', 2).arg(v >= 0 ? p : n);
    };
    auto f9 = [](double v, char p, char n) {
        return QString("%1%2").arg(std::abs(v), 9, 'f', 2).arg(v >= 0 ? p : n);
    };
    return "CIRCUIT   " + f5(la1, 'N', 'S') + f9(lo1, 'E', 'W')
                        + f9(la2, 'N', 'S') + f9(lo2, 'E', 'W') + "  S     0";
}

} // namespace

Voacap::Voacap(QObject* parent) : QObject(parent) {}

QString Voacap::engineMissing() {
    if (voacaplPath().isEmpty())
        return QStringLiteral(
            "voacapl not found (the AppImage bundles it; source builds: "
            "github.com/jawatson/voacapl — configure "
            "--prefix=$HOME/.local; make; make install; makeitshfbc)");
    if (itshfbcDir().isEmpty())
        return QStringLiteral(
            "itshfbc data tree missing — run makeitshfbc once");
    return {};
}

void Voacap::request(double latDeg, double lonDeg, double freqMHz,
                     int ssn, int powerW) {
    if (busy() || !engineMissing().isEmpty()) return;
    lat_ = latDeg; lon_ = lonDeg; freq_ = freqMHz;
    ssn_ = ssn; powerW_ = powerW;
    runDir_ = itshfbcDir();

    const QDateTime utc = QDateTime::currentDateTimeUtc();
    hourUt_ = utc.time().hour() == 0 ? 24 : utc.time().hour();

    QStringList deck;
    deck << "COMMENT    tentec-console VOACAP overlay"
         << "LINEMAX      55       number of lines-per-page"
         << "COEFFS    CCIR"
         << QString("TIME       %1%2    1    1")
                .arg(hourUt_, 4).arg(hourUt_, 5)
         << QString("MONTH      %1%2")
                .arg(utc.date().year())
                .arg(double(utc.date().month()), 5, 'f', 2)
         << QString("SUNSPOT    %1.").arg(ssn_)
         << "SYSTEM       1. 145. 3.00  90. 24.0 3.00 0.10"
         << "FPROB      1.00 1.00 1.00 0.00"
         << QString("ANTENNA       1    1    2   30     0.000"
                    "[samples/sample.00    ]  0.0  %1")
                .arg(std::max(0.001, powerW / 1000.0), 8, 'f', 4)
         << "ANTENNA       2    2    2   30     0.000"
            "[samples/sample.00    ]  0.0    0.0000"
         << QString("FREQUENCY %1 0.00 0.00 0.00 0.00 0.00 0.00 0.00 "
                    "0.00 0.00 0.00").arg(freqMHz, 5, 'f', 2)
         << "METHOD       30    0";
    for (int a = 0; a < kAzCount; ++a)
        for (int di = 0; di < kDistCount; ++di) {
            double la = 0.0, lo = 0.0;
            forward(latDeg, lonDeg, a * kAzStep, kDistKm[di], la, lo);
            deck << QString("LABEL     CONSOLE             a%1d%2")
                        .arg(a, 2, 10, QLatin1Char('0'))
                        .arg(di, 2, 10, QLatin1Char('0'))
                 << circuitCard(latDeg, lonDeg, la, lo)
                 << "EXECUTE";
        }
    deck << "QUIT";

    // voacapl resolves the input name relative to <root>/run — the deck
    // must land there (live-found: a deck at the root ran a stale file).
    QFile f(runDir_ + "/run/console.dat");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(deck.join('\n').toLatin1() + '\n');
    f.close();

    if (!proc_) {
        proc_ = new QProcess(this);
        connect(proc_, &QProcess::finished, this, &Voacap::onFinished);
    }
    proc_->start(voacaplPath(),
                 {runDir_, QStringLiteral("console.dat"),
                  QStringLiteral("console.out")});
}

void Voacap::onFinished(int code, QProcess::ExitStatus st) {
    if (code != 0 || st != QProcess::NormalExit) return;
    QFile f(runDir_ + "/run/console.out");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    // One "S DBW" row per circuit, in deck order: value #2 is our
    // frequency's column ("-" = no path). S9 = -103 dBW, 6 dB per S-unit.
    QVector<float> s(kAzCount * kDistCount, 0.0f);
    int idx = 0;
    QTextStream ts(&f);
    while (!ts.atEnd() && idx < s.size()) {
        const QString line = ts.readLine();
        if (!line.endsWith(QLatin1String("S DBW ")) &&
            !line.trimmed().endsWith(QLatin1String("S DBW")))
            continue;
        const QStringList tok =
            line.left(line.indexOf(QLatin1String("S DBW")))
                .split(' ', Qt::SkipEmptyParts);
        double sdbw = -999.0;
        if (tok.size() >= 2 && tok[1] != QLatin1String("-"))
            sdbw = tok[1].toDouble();
        s[idx++] = sdbw <= -400.0
            ? 0.0f
            : float(std::clamp(9.0 + (sdbw + 103.0) / 6.0, 0.0, 15.0));
    }
    if (idx < s.size() / 2) return;                 // parse went sideways

    // Reduce to contours: for each S threshold, the OUTERMOST range per
    // azimuth still at/above it (the classic coverage boundary; inner
    // skip-zone holes are collapsed — KE9NS draws the same simplification).
    static const float kLevels[] = {9.0f, 7.0f, 5.0f, 3.0f, 1.0f};
    QVector<PropContour> out;
    for (float lvl : kLevels) {
        PropContour c;
        c.sLevel = lvl;
        int hits = 0;
        for (int a = 0; a < kAzCount; ++a) {
            int best = -1;
            for (int di = 0; di < kDistCount; ++di)
                if (s[a * kDistCount + di] >= lvl) best = di;
            double km = 120.0;                      // no coverage: hug home
            if (best >= 0) {
                ++hits;
                km = kDistKm[best];
                if (best + 1 < kDistCount)          // soften the staircase
                    km = 0.5 * (km + kDistKm[best + 1]);
            }
            double la = 0.0, lo = 0.0;
            forward(lat_, lon_, a * kAzStep, km, la, lo);
            c.ll.append(QPointF(lo, la));
        }
        if (hits >= 6) out.append(c);
    }
    QString legend =
        QString("VOACAP %1 MHz  %2z  SSN %3  %4 W isotrope")
            .arg(freq_, 0, 'f', 1)
            .arg(hourUt_ % 24, 2, 10, QLatin1Char('0'))
            .arg(ssn_)
            .arg(powerW_);
    // Nothing above S1 anywhere: say so instead of drawing nothing — the
    // operator can't tell "band closed" from "overlay broken" otherwise.
    // (And VOACAP is an F2 model: sporadic-E summer openings never show.)
    if (out.isEmpty())
        legend += QStringLiteral("  — band closed (F2 model; Es not modeled)");
    emit ready(out, legend);
}

} // namespace ttc
