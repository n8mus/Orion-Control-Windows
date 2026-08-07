// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/SkimViewWindow.h"
#include "cw/SkimStft.h"
#include "cw/SkimmerEngine.h"

#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

namespace ttc {

namespace {
// Same "Enhanced" ramp as the panadapter (its table is file-local there;
// the two must read as one instrument).
struct Stop { float t; int r, g, b; };
const Stop kStops[] = {
    {0.00f,   0,   0,   0}, {0.12f,  25,   0,  60}, {0.25f,   0,  30, 160},
    {0.40f,   0, 120, 230}, {0.52f,   0, 200, 180}, {0.62f,  40, 210,  50},
    {0.72f, 230, 230,  40}, {0.82f, 255, 140,  20}, {0.92f, 255,  40,  30},
    {1.00f, 255, 255, 255},
};

// A "nice" tick step (1/2/5 x 10^k) giving ~6 divisions across the view.
double niceStep(double span) {
    const double raw = span / 6.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (raw <= m * mag) return m * mag;
    return 10.0 * mag;
}

constexpr int    kColsPerPx = 2;      // ~16 ms/px -> ~61 px/s scroll
constexpr size_t kHistCols  = 4096;   // ~34 s of raw columns for re-render
constexpr float  kRangeDb   = 45.0f;  // floor..top of the color ramp
constexpr float  kFloorPad  = 3.0f;   // keep the noise just above black
} // namespace

// All the work lives here; the dialog is a frame around it.
class SkimViewCanvas : public QWidget {
public:
    SkimViewCanvas(SkimStft* stft, SkimmerEngine* eng,
                   std::function<QChar(const QString&, qint64)> status,
                   std::function<void(qint64&, int&)> tuning,
                   std::function<void(qint64, const QString&)> onTune,
                   QWidget* parent)
        : QWidget(parent), stft_(stft), eng_(eng),
          status_(std::move(status)), tuning_(std::move(tuning)),
          onTune_(std::move(onTune)) {
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        for (int i = 0; i < 256; ++i) {                // palette LUT
            const float t = i / 255.0f;
            size_t k = 1;
            while (k < std::size(kStops) - 1 && kStops[k].t < t) ++k;
            const Stop &a = kStops[k - 1], &b = kStops[k];
            const float f = (t - a.t) / std::max(1e-6f, b.t - a.t);
            lut_[i] = qRgb(int(a.r + f * (b.r - a.r)),
                           int(a.g + f * (b.g - a.g)),
                           int(a.b + f * (b.b - a.b)));
        }
        drain_ = new QTimer(this);
        drain_->setInterval(33);
        connect(drain_, &QTimer::timeout, this,
                [this] { drainColumns(); });
        tune_ = new QTimer(this);
        tune_->setInterval(300);
        connect(tune_, &QTimer::timeout, this,
                [this] { checkTuning(); });
    }

    void start() {
        checkTuning();
        drain_->start();
        tune_->start();
    }
    void stop() {
        drain_->stop();
        tune_->stop();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (wfDirty_) rebuildImage();
        QPainter p(this);
        p.fillRect(rect(), QColor(5, 8, 12));
        if (!wfImg_.isNull()) p.drawImage(0, 0, wfImg_);
        drawScale(p);
        drawDial(p);
        drawLabels(p);
        drawHover(p);
        drawInfo(p);
    }

    void resizeEvent(QResizeEvent*) override { wfDirty_ = true; }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        pressPos_ = e->pos();
        pressView0_ = viewLo_;
        dragging_ = false;
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        hover_ = e->pos();
        if (e->buttons() & Qt::LeftButton) {
            if ((e->pos() - pressPos_).manhattanLength() > 4)
                dragging_ = true;
            if (dragging_ && height() > 0) {
                const double span = viewHi_ - viewLo_;
                const double dHz =
                    (e->pos().y() - pressPos_.y()) * span / height();
                setView(pressView0_ + dHz, pressView0_ + dHz + span);
            }
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || dragging_) return;
        // A label click tunes to the spot and carries the call; a trace
        // click tunes to the frequency under the cursor.
        for (const auto& L : labelHits_)
            if (L.rect.contains(e->pos())) {
                onTune_(L.hz, L.call);
                return;
            }
        const qint64 hz = hzOfY(e->pos().y());
        if (hz > 0) onTune_((hz + 5) / 10 * 10, QString());
    }

    void wheelEvent(QWheelEvent* e) override {
        const double span = viewHi_ - viewLo_;
        const double f = e->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;
        double ns = std::clamp(span * f, 1500.0, displaySpan());
        const double hzC = double(hzOfY(int(e->position().y())));
        const double top = hzC + (viewHi_ - hzC) * ns / span;
        setView(top - ns, top);
        // Re-anchor an in-flight drag: the pan math works from the
        // press-time view, and a zoom mid-drag would make the next
        // mouse-move snap the view back (review-found lurch).
        pressPos_ = e->position().toPoint();
        pressView0_ = viewLo_;
        e->accept();
    }

private:
    struct LabelHit {
        QRect rect;
        qint64 hz;
        QString call;
    };

    // ---- frequency plumbing -------------------------------------------
    double displaySpan() const {
        // The FIR's skirts make the outer ~8% of the slice dishonest.
        return 0.92 * stft_->binHz() * SkimStft::kFft;
    }
    double yOfHz(double hz) const {
        return (viewHi_ - hz) / (viewHi_ - viewLo_) * height();
    }
    qint64 hzOfY(int y) const {
        if (height() <= 0) return 0;
        return qint64(std::llround(
            viewHi_ - double(y) * (viewHi_ - viewLo_) / height()));
    }
    double binOfHz(double hz) const {
        return SkimStft::kFft / 2.0
            + (hz - double(sliceCenterAbs_)) / stft_->binHz();
    }

    void setView(double lo, double hi) {
        const double half = displaySpan() / 2.0;
        const double lim0 = double(sliceCenterAbs_) - half;
        const double lim1 = double(sliceCenterAbs_) + half;
        const double span = std::min(hi - lo, lim1 - lim0);
        lo = std::clamp(lo, lim0, lim1 - span);
        viewLo_ = lo;
        viewHi_ = lo + span;
        wfDirty_ = true;
        update();
    }

    void checkTuning() {
        qint64 dial = 0;
        int loOff = 0;
        tuning_(dial, loOff);
        if (dial <= 0) return;
        dial_ = dial;
        const qint64 loAbs = dial + loOff;
        qint64 segLo = 0, segHi = 0;
        if (!SkimmerEngine::cwSegment(dial, segLo, segHi)) {
            segLo = dial - 12000;              // parked off-segment: follow
            segHi = dial + 12000;              // the dial with a ±12 k slice
        }
        double off = double((segLo + segHi) / 2 - loAbs);
        // Keep the slice inside the capture.
        const double lim = stft_->inputRate() / 2.0
            - stft_->binHz() * SkimStft::kFft / 2.0;
        off = std::clamp(off, -lim, lim);
        if (std::abs(off - sliceOff_) > 0.5 || loAbs != loAbs_) {
            const qint64 center = loAbs + qint64(std::llround(off));
            sliceOff_ = off;
            loAbs_ = loAbs;
            curGen_ = stft_->setViewOffset(off);
            accumN_ = 0;
            // Keep the history when only the LO moved under a fixed
            // slice (classic-mode tuning re-parks the LO on every dial
            // step): the stored columns are slice-center-relative and
            // still true. Without this, every wheel click blanked ~30 s
            // of waterfall (review-found).
            if (center != sliceCenterAbs_) {
                sliceCenterAbs_ = center;
                raw_.clear();
                wfDirty_ = true;
            }
            if (segLo != segLo_ || segHi != segHi_) {
                segLo_ = segLo;
                segHi_ = segHi;
                setView(double(segLo) - 500.0, double(segHi) + 500.0);
            }
        }
        update();                              // dial marker follows anyway
    }

    // ---- waterfall data -----------------------------------------------
    void drainColumns() {
        auto batch = stft_->takeColumns();
        std::vector<std::vector<uint8_t>> px;      // finished pixel columns
        for (auto& c : batch) {
            if (c.gen != curGen_) continue;    // pre-retune leftovers
            if (accumN_ == 0) accum_ = c.db;
            else
                for (size_t i = 0; i < accum_.size(); ++i)
                    accum_[i] = std::max(accum_[i], c.db[i]);
            raw_.push_back(std::move(c.db));
            if (raw_.size() > kHistCols) raw_.pop_front();
            if (++accumN_ >= kColsPerPx) {
                px.push_back(accum_);
                accumN_ = 0;
            }
            // The very first column must seed the floor or the opening
            // pixel columns paint against the -120 fallback — a bright
            // full-height stripe on any real band (review-found).
            if (floorDb_ < -998.0f || ++floorTick_ >= 8) {
                floorTick_ = 0;
                updateFloor(raw_.back());
            }
        }
        if (px.empty()) return;
        if (!wfDirty_ && !wfImg_.isNull()) {
            // One scroll for the whole batch (replay can free-run at many
            // times real speed), newest column at the right edge.
            const int w = wfImg_.width();
            const int k = std::min(int(px.size()), w);
            scrollLeft(k);
            for (int i = 0; i < k; ++i)
                renderPixelColumn(w - k + i,
                                  px[px.size() - size_t(k) + i].data());
        }
        update();
    }

    void updateFloor(const std::vector<uint8_t>& col) {
        // Median of a spread sample of the visible bins: the display AGC.
        int b0 = int(binOfHz(viewLo_)), b1 = int(binOfHz(viewHi_));
        b0 = std::clamp(b0, 0, SkimStft::kFft - 1);
        b1 = std::clamp(b1, b0 + 1, SkimStft::kFft);
        std::vector<uint8_t> s;
        s.reserve(256);
        const int step = std::max(1, (b1 - b0) / 256);
        for (int b = b0; b < b1; b += step) s.push_back(col[b]);
        if (s.size() < 8) return;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const float med =
            SkimStft::kDbMin + s[s.size() / 2] * SkimStft::kDbStep;
        if (floorDb_ < -998.0f) floorDb_ = med;    // first estimate: jump
        else floorDb_ += 0.05f * (med - floorDb_);
    }

    void scrollLeft(int k) {
        const int w = wfImg_.width();
        if (k >= w) return;                    // whole image gets repainted
        for (int y = 0; y < wfImg_.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(wfImg_.scanLine(y));
            std::memmove(line, line + k, size_t(w - k) * sizeof(QRgb));
        }
    }

    void renderPixelColumn(int x, const uint8_t* col) {
        const float floor = floorDb_ < -998.0f ? -120.0f : floorDb_;
        for (int y = 0; y < wfImg_.height(); ++y) {
            const auto& [b0, b1] = yBins_[size_t(y)];
            float v;
            if (b1 > b0) {
                uint8_t m = 0;
                for (int b = b0; b < b1; ++b) m = std::max(m, col[b]);
                v = SkimStft::kDbMin + m * SkimStft::kDbStep;
            } else {                           // zoomed past 1 bin/px
                const double fb = std::clamp(
                    binOfHz(viewHi_ - (y + 0.5) * (viewHi_ - viewLo_)
                                          / height()),
                    0.0, double(SkimStft::kFft - 1.001));
                const int b = int(fb);
                const float f = float(fb - b);
                v = SkimStft::kDbMin
                    + ((1.0f - f) * col[b] + f * col[b + 1])
                          * SkimStft::kDbStep;
            }
            const float t = (v - floor - kFloorPad) / kRangeDb;
            const int i = std::clamp(int(t * 255.0f), 0, 255);
            reinterpret_cast<QRgb*>(wfImg_.scanLine(y))[x] = lut_[i];
        }
    }

    void rebuildImage() {
        wfDirty_ = false;
        const int w = std::max(1, width()), h = std::max(1, height());
        wfImg_ = QImage(w, h, QImage::Format_RGB32);
        wfImg_.fill(QColor(5, 8, 12));
        yBins_.resize(size_t(h));
        for (int y = 0; y < h; ++y) {
            const double hzHi = viewHi_ - y * (viewHi_ - viewLo_) / h;
            const double hzLo = viewHi_ - (y + 1) * (viewHi_ - viewLo_) / h;
            int b0 = int(std::ceil(binOfHz(hzLo)));
            int b1 = int(std::ceil(binOfHz(hzHi)));
            b0 = std::clamp(b0, 0, SkimStft::kFft);
            b1 = std::clamp(b1, 0, SkimStft::kFft);
            yBins_[size_t(y)] = {b0, b1};
        }
        accumN_ = 0;
        const size_t groups = raw_.size() / kColsPerPx;
        const size_t useGroups = std::min(groups, size_t(w));
        size_t idx = raw_.size() - useGroups * kColsPerPx;
        int x = w - int(useGroups);
        std::vector<uint8_t> merged;
        for (size_t g = 0; g < useGroups; ++g, ++x) {
            merged = raw_[idx++];
            for (int k = 1; k < kColsPerPx; ++k, ++idx)
                for (size_t i = 0; i < merged.size(); ++i)
                    merged[i] = std::max(merged[i], raw_[idx][i]);
            renderPixelColumn(x, merged.data());
        }
    }

    // ---- overlays ------------------------------------------------------
    void drawScale(QPainter& p) {
        p.setFont(QFont(QStringLiteral("monospace"), 9));
        const double step = niceStep(viewHi_ - viewLo_);
        double f = std::ceil(viewLo_ / step) * step;
        for (; f <= viewHi_; f += step) {
            const int y = int(yOfHz(f));
            p.setPen(QColor(255, 255, 255, 26));
            p.drawLine(44, y, width(), y);
            p.setPen(QColor(143, 163, 184));
            p.drawText(QRect(0, y - 8, 42, 16),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(f / 1000.0, 'f',
                                       step >= 1000.0 ? 0 : 1));
        }
    }

    void drawDial(QPainter& p) {
        if (dial_ <= 0) return;
        const int y = int(yOfHz(double(dial_)));
        if (y < -2 || y > height() + 2) return;
        p.setPen(QPen(QColor(255, 92, 70, 200), 1, Qt::DashLine));
        p.drawLine(0, y, width(), y);
        p.setPen(QColor(255, 92, 70));
        p.drawText(QRect(width() - 130, y - 16, 124, 14),
                   Qt::AlignRight | Qt::AlignBottom, QStringLiteral("▶ A"));
    }

    void drawLabels(QPainter& p) {
        labelHits_.clear();
        struct Row {
            double y;
            QString head;              // call (or "?" while hunting)
            QString tail;              // live decode text, CW Skimmer style
            QColor color;
            qint64 hz;
            QString call;
        };
        std::vector<Row> rows;
        const auto statusColor = [this](const QString& call, qint64 hz) {
            QColor col(205, 140, 255);                 // skimmer violet
            if (status_) switch (status_(call, hz).toLatin1()) {
                case 'N': col = QColor(255, 92, 70); break;
                case 'B': col = QColor(255, 190, 60); break;
                case 'W': col = QColor(130, 222, 140); break;
                case 'C': col = QColor(150, 162, 178); break;
            }
            return col;
        };
        // Live channels print their decode as it happens — the CW-Skimmer
        // behavior the operator expects; the confirmed call (once the
        // miner is sure) rides in front in worked-before color.
        QSet<QString> live;
        for (const auto& c : eng_->channelInfo()) {
            if (!c.active) continue;
            if (c.hz < viewLo_ || c.hz > viewHi_) continue;
            Row r;
            r.y = yOfHz(double(c.hz));
            r.hz = c.hz;
            r.call = c.call;
            r.tail = c.text.right(18);
            if (c.call.isEmpty()) {
                r.head = QStringLiteral("?");
                r.color = QColor(74, 90, 110);
            } else {
                r.head = c.call;
                r.color = statusColor(c.call, c.hz);
                live.insert(c.call);
            }
            rows.push_back(std::move(r));
        }
        // Spots whose channel moved on (kept 10 min): call + WPM, no text.
        for (const auto& s : eng_->spots()) {
            if (live.contains(s.call)) continue;
            if (s.hz < viewLo_ || s.hz > viewHi_) continue;
            QString head = s.call;
            if (s.wpm > 0) head += QStringLiteral(" ·%1").arg(s.wpm);
            rows.push_back({yOfHz(double(s.hz)), head, QString(),
                            statusColor(s.call, s.hz), s.hz, s.call});
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.y < b.y; });
        // De-collide downward so stacked stations stay readable; a thin
        // connector keeps a displaced label pointing at its trace.
        QFont fb(QStringLiteral("monospace"), 10, QFont::Bold);
        QFont fr(QStringLiteral("monospace"), 10);
        const QFontMetrics fmB(fb), fmR(fr);
        double nextFree = -1e9;
        for (auto& r : rows) {
            const double y = std::max(r.y, nextFree);
            nextFree = y + 14.0;
            if (y > height() + 8) break;
            const int hw = fmB.horizontalAdvance(r.head);
            const int tw = r.tail.isEmpty()
                ? 0 : fmR.horizontalAdvance(r.tail) + 6;
            const QRect box(width() - hw - tw - 26, int(y) - 7,
                            hw + tw + 8, 14);
            p.fillRect(box, QColor(5, 8, 12, 190));
            p.setPen(QPen(r.color.darker(140), 1));
            p.drawLine(box.right() + 2, box.center().y(),
                       width() - 4, int(r.y));
            p.setFont(fb);
            p.setPen(r.color);
            p.drawText(QRect(box.left() + 4, box.top(), hw, 14),
                       Qt::AlignLeft | Qt::AlignVCenter, r.head);
            if (tw > 0) {
                p.setFont(fr);
                p.setPen(QColor(126, 146, 166));
                p.drawText(QRect(box.left() + 4 + hw + 6, box.top(),
                                 tw - 6, 14),
                           Qt::AlignLeft | Qt::AlignVCenter, r.tail);
            }
            labelHits_.push_back({box, r.hz, r.call});
        }
    }

    void drawHover(QPainter& p) {
        if (!underMouse() || hover_.y() < 0) return;
        p.setPen(QColor(255, 255, 255, 60));
        p.drawLine(0, hover_.y(), width(), hover_.y());
        p.setPen(QColor(200, 212, 224));
        p.setFont(QFont(QStringLiteral("monospace"), 9));
        p.drawText(QPoint(48, hover_.y() - 4),
                   QString::number(hzOfY(hover_.y()) / 1000.0, 'f', 2)
                       + QStringLiteral(" kHz"));
    }

    void drawInfo(QPainter& p) {
        p.setFont(QFont(QStringLiteral("monospace"), 9));
        p.setPen(QColor(143, 163, 184));
        QString t;
        if (raw_.empty())
            t = QStringLiteral("waiting for IQ…");
        else
            t = QStringLiteral("%1–%2 kHz · %3 Hz/bin · %4 ms/px")
                    .arg(viewLo_ / 1000.0, 0, 'f', 1)
                    .arg(viewHi_ / 1000.0, 0, 'f', 1)
                    .arg(stft_->binHz(), 0, 'f', 0)
                    .arg(stft_->colSecs() * 1000.0 * kColsPerPx, 0, 'f', 0);
        p.drawText(QPoint(48, 14), t);
    }

    SkimStft* stft_;
    SkimmerEngine* eng_;
    std::function<QChar(const QString&, qint64)> status_;
    std::function<void(qint64&, int&)> tuning_;
    std::function<void(qint64, const QString&)> onTune_;

    QTimer* drain_ = nullptr;
    QTimer* tune_ = nullptr;

    // slice + view state (absolute Hz)
    qint64 dial_ = 0, loAbs_ = 0, sliceCenterAbs_ = 0;
    qint64 segLo_ = 0, segHi_ = 0;
    double sliceOff_ = 1e18;               // force the first retune
    uint32_t curGen_ = ~0u;
    double viewLo_ = 0.0, viewHi_ = 1.0;

    // raw column history + rendered image
    std::deque<std::vector<uint8_t>> raw_;
    std::vector<uint8_t> accum_;
    int accumN_ = 0;
    QImage wfImg_;
    bool wfDirty_ = true;
    std::vector<std::pair<int, int>> yBins_;
    float floorDb_ = -999.0f;
    int floorTick_ = 0;
    QRgb lut_[256];

    // interaction
    QPoint hover_{-1, -1};
    QPoint pressPos_;
    double pressView0_ = 0.0;
    bool dragging_ = false;
    std::vector<LabelHit> labelHits_;
};

SkimViewWindow::SkimViewWindow(
    SkimStft* stft, SkimmerEngine* eng,
    std::function<QChar(const QString&, qint64)> status,
    std::function<void(qint64&, int&)> tuning, QWidget* parent)
    : QDialog(parent), stft_(stft) {
    setWindowTitle("SKIM — waterfall");
    resize(920, 640);
    setMinimumSize(480, 320);
    setStyleSheet("QDialog { background: #05080c; }");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    canvas_ = new SkimViewCanvas(
        stft, eng, std::move(status), std::move(tuning),
        [this](qint64 hz, const QString& call) { emit tuneTo(hz, call); },
        this);
    lay->addWidget(canvas_, 1);
}

void SkimViewWindow::showEvent(QShowEvent* e) {
    stft_->setEnabled(true);
    canvas_->start();
    QDialog::showEvent(e);
}

void SkimViewWindow::hideEvent(QHideEvent* e) {
    canvas_->stop();
    stft_->setEnabled(false);
    QDialog::hideEvent(e);
}

} // namespace ttc
