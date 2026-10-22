// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/PanGpuView.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <QtCore/qfloat16.h>
#include <array>
#include <rhi/qrhi.h>

#include "ui/PanadapterWidget.h"

namespace ttc {

namespace {
QShader loadShader(const char* name) {
    QFile f(QString(":/shaders/%1.qsb").arg(name));
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}
constexpr int kQuadUbo = 64 + 3 * 16;              // mat4 + 3 vec4 (std140)
constexpr int kLineUbo = 64 + 2 * 16;              // mat4 + 2 vec4
constexpr int kLabelStripPx = 160;                 // waterfall label strip width
// AetherSDR's dss_mesh layout (their SpectrumWidget): two exact-row layers
// per row, two triangles per column step, for the curtains and again for
// the ridge ribbons; UBO = 22 scalars + 2 pad + bgFill + 8 shadow bands +
// 8 shadow styles + shadow meta + one frequency frame per ring row.
constexpr int kDssFillVertsPerRow = (DssRenderer::kMeshCols - 1) * 6 * 2;
constexpr int kDssLineVertsPerRow = (DssRenderer::kMeshCols - 1) * 6 * 2;
constexpr int kDssMeshUboFloats = 96 + DssRenderer::kRows * 4;
constexpr int kDssMeshShadowSlices = 8;
constexpr int kDssMaxIncrementalUploadRows = 8;
static_assert(DssRenderer::kRows == 104,
              "dss_mesh.{vert,frag} declare rowFrames[104]; update both with DssRenderer::kRows");
// TTC_PANPROF=1: GPU frame cost on the CPU side (layer rebuilds + uploads
// + command recording), same cadence as the CPU path's [panprof].
struct GpuProf {
    bool on = qEnvironmentVariableIsSet("TTC_PANPROF");
    QElapsedTimer window;
    double renderMs = 0, layerMs = 0;
    int frames = 0, rebuilds = 0;
    QElapsedTimer gap;                             // render-to-render: GUI stalls
    double maxGapMs = 0;
    int stalls = 0;
    void add(double r, double l, bool rebuilt) {
        if (!on) return;
        if (!window.isValid()) window.start();
        if (gap.isValid()) {
            const double g = gap.nsecsElapsed() / 1e6;
            maxGapMs = std::max(maxGapMs, g);
            stalls += g > 50.0;
        }
        gap.restart();
        renderMs += r; layerMs += l; ++frames; rebuilds += rebuilt;
        if (window.elapsed() < 5000) return;
        const double secs = window.elapsed() / 1000.0;
        std::fprintf(stderr, "[panprof-gpu] %.1f fps | %.2f ms/frame on the CPU (layers %.2f, "
                             "%d rebuilds) | %.0f%% of a core | FFT %.1f/s, dropped %.1f/s, "
                             "shown %.1f/s | worst gap %.0f ms, %d gaps > 50 ms | max queue %d, max IQ backlog %d ms\n",
                     frames / secs, renderMs / frames, layerMs / frames, rebuilds,
                     100.0 * renderMs / (secs * 1000.0),
                     PanadapterWidget::profEmitted.exchange(0) / secs,
                     PanadapterWidget::profDropped.exchange(0) / secs,
                     PanadapterWidget::profTaken.exchange(0) / secs, maxGapMs, stalls,
                     PanadapterWidget::profMaxQueue.exchange(0), PanadapterWidget::profMaxIqMs.exchange(0));
        maxGapMs = 0;
        stalls = 0;
        renderMs = layerMs = 0; frames = rebuilds = 0;
        window.restart();
    }
};
GpuProf gGpuProf;
}  // namespace

PanGpuView::PanGpuView(PanadapterWidget* pan) : QRhiWidget(pan), pan_(pan) {
    setApi(QRhiWidget::Api::OpenGL);
    setAttribute(Qt::WA_TransparentForMouseEvents);   // the panadapter keeps its events
    setFocusPolicy(Qt::NoFocus);
}

PanGpuView::~PanGpuView() { freeAll(); }

void PanGpuView::releaseResources() { freeAll(); }

void PanGpuView::freeAll() {
    auto del = [](auto*& p) { delete p; p = nullptr; };
    for (Layer* l : {&under_, &mid_, &labels_, &top_, &cursor_}) {
        del(l->tex);
        del(l->srb);
        l->img = QImage();
        l->dirty = true;
    }
    del(overlayPipe_); del(wfPipe_); del(fillPipe_); del(linePipe_); del(widePipe_); del(dssPipe_);
    del(wfSrb_); del(fillSrb_); del(peakSrb_); del(traceSrb_); del(dssSrb_);
    del(quadVbo_); del(lineVbo_); del(wideVbo_); del(dssFillVbo_); del(dssLineVbo_); del(dssUbo_);
    for (auto*& u : ubo_) del(u);
    del(linear_); del(nearest_);
    del(hist_); del(palette_); del(cols_); del(dssHeight_); del(dssLut_);
    histBins_ = colsW_ = lineVboCap_ = wideVboCap_ = 0;
    dssReady_ = dssTried_ = false;
    dssHeadUploaded_ = -1;
    dssRowGenUploaded_ = dssLutToken_ = ~0ull;
    histReset_ = ~0ull;
    paletteId_ = -1;
    pipesBuilt_ = false;
    rhi_ = nullptr;
}

void PanGpuView::initialize(QRhiCommandBuffer*) {
    if (rhi_ != rhi()) { freeAll(); rhi_ = rhi(); }
    if (pipesBuilt_) return;
    QRhi* r = rhi_;
    // Quad: triangle strip, (ndc x, y, u, v); uv (0,0) = top-left.
    static const float quad[] = {-1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,
                                 -1.f,  1.f, 0.f, 0.f,  1.f,  1.f, 1.f, 0.f};
    quadVbo_ = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof quad);
    quadVbo_->create();
    for (int i = 0; i < uCount; ++i) {
        const int sz = (i == uPeak || i == uTrace) ? kLineUbo : kQuadUbo;
        ubo_[i] = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sz);
        ubo_[i]->create();
    }
    linear_ = r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    linear_->create();
    nearest_ = r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    nearest_->create();
    palette_ = r->newTexture(QRhiTexture::RGBA8, QSize(256, 1));
    palette_->create();
    quadUploaded_ = false;                         // render() uploads it
    // Placeholders so every binding is valid before the first data arrives.
    ensureHist(2);
    cols_ = r->newTexture(QRhiTexture::R32F, QSize(2, 1));
    cols_->create();
    colsW_ = 2;
    for (Layer* l : {&under_, &mid_, &labels_, &top_, &cursor_}) ensureLayer(*l, QSize(2, 2));
    pipesBuilt_ = true;
}

void PanGpuView::ensureLayer(Layer& l, const QSize& sz) {
    if (l.tex && l.tex->pixelSize() == sz) return;
    delete l.tex;
    delete l.srb;
    if (&l == &under_) { delete overlayPipe_; overlayPipe_ = nullptr; }   // made with this SRB
    l.tex = rhi_->newTexture(QRhiTexture::RGBA8, sz);
    l.tex->create();
    l.img = QImage(sz, QImage::Format_RGBA8888_Premultiplied);
    l.img.fill(Qt::transparent);
    l.dirty = true;
    const int ui = &l == &under_ ? uUnder : &l == &mid_ ? uMid : &l == &labels_ ? uLabels
                 : &l == &top_ ? uTop : uCursor;
    l.srb = rhi_->newShaderResourceBindings();
    l.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage
                                                        | QRhiShaderResourceBinding::FragmentStage,
                                                 ubo_[ui]),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                  l.tex, linear_)});
    l.srb->create();
}

void PanGpuView::ensureHist(int bins) {
    if (hist_ && histBins_ == bins) return;
    delete hist_;
    hist_ = rhi_->newTexture(QRhiTexture::R32F, QSize(bins, PanadapterWidget::kWfHistRows));
    hist_->create();
    histBins_ = bins;
    histReset_ = ~0ull;                            // full upload next frame
    delete wfSrb_;
    wfSrb_ = nullptr;
    delete wfPipe_;
    wfPipe_ = nullptr;
}

// AetherSDR's initDssMeshPipeline + the static mesh build from their
// initialize(). OpenGL: one pipeline draws curtains AND ribbons (their
// SharedFillPipeline — a separate, identically configured outline program
// gave flat/stale rows on Linux, live-found there).
bool PanGpuView::initDss(QRhiResourceUpdateBatch* u) {
    dssTried_ = true;
    QRhi* r = rhi_;
    if (!r->isTextureFormatSupported(QRhiTexture::RGBA16F, {})) {
        std::fprintf(stderr, "[pan] RGBA16F unsupported: no 3D (2D stays)\n");
        return false;
    }
    const QShader vs = loadShader("dss_mesh.vert"), fs = loadShader("dss_mesh.frag");
    if (!vs.isValid() || !fs.isValid()) return false;
    const int rows = DssRenderer::kVisibleRows;
    dssFillVbo_ = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                               rows * kDssFillVertsPerRow * 3 * sizeof(float));
    dssLineVbo_ = r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                               rows * kDssLineVertsPerRow * 3 * sizeof(float));
    dssUbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                           kDssMeshUboFloats * sizeof(float));
    if (!dssFillVbo_->create() || !dssLineVbo_->create() || !dssUbo_->create()) return false;
    dssHeight_ = r->newTexture(QRhiTexture::RGBA16F, QSize(DssRenderer::kCols, DssRenderer::kRows));
    dssLut_ = r->newTexture(QRhiTexture::RGBA8, QSize(256, 1));
    if (!dssHeight_->create() || !dssLut_->create()) return false;
    dssSrb_ = r->newShaderResourceBindings();
    dssSrb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, dssUbo_),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::VertexStage,
                                                  dssHeight_, nearest_),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                  dssLut_, linear_)});
    if (!dssSrb_->create()) return false;
    QRhiVertexInputLayout layout;
    layout.setBindings({{3 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});   // u, v, edge
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    dssPipe_ = r->newGraphicsPipeline();
    dssPipe_->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    dssPipe_->setVertexInputLayout(layout);
    dssPipe_->setTopology(QRhiGraphicsPipeline::Triangles);
    dssPipe_->setShaderResourceBindings(dssSrb_);
    dssPipe_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    dssPipe_->setTargetBlends({blend});
    if (!dssPipe_->create()) return false;

    // The static perspective grid, built once: height comes from the ring
    // texture per vertex, so pan/zoom never rebuild it.
    const int cols = DssRenderer::kMeshCols;
    std::vector<float> fill, line;
    fill.reserve(size_t(rows) * kDssFillVertsPerRow * 3);
    line.reserve(size_t(rows) * kDssLineVertsPerRow * 3);
    const auto vtx = [](std::vector<float>& m, float uu, float vv, float edge) {
        m.push_back(uu); m.push_back(vv); m.push_back(edge);
    };
    for (int rr = rows - 1; rr >= 0; --rr) {
        const float v = float(rr) / rows;          // 0 front .. ~1 back
        // Base first, overlay second (their exact-row crossfade layers).
        for (int layer = 0; layer < 2; ++layer) {
            const float fillTag = layer == 0 ? 0.0f : 2.0f;
            const float floorTag = layer == 0 ? 1.0f : 3.0f;
            const float ribbonLowerTag = layer == 0 ? -10.0f : -20.0f;
            const float ribbonUpperTag = layer == 0 ? -11.0f : -21.0f;
            for (int cc = 0; cc + 1 < cols; ++cc) {
                const float u0 = float(cc) / (cols - 1), u1 = float(cc + 1) / (cols - 1);
                vtx(fill, u0, v, fillTag); vtx(fill, u0, v, floorTag); vtx(fill, u1, v, fillTag);
                vtx(fill, u1, v, fillTag); vtx(fill, u0, v, floorTag); vtx(fill, u1, v, floorTag);
                vtx(line, u0, v, ribbonLowerTag); vtx(line, u0, v, ribbonUpperTag);
                vtx(line, u1, v, ribbonLowerTag); vtx(line, u1, v, ribbonLowerTag);
                vtx(line, u0, v, ribbonUpperTag); vtx(line, u1, v, ribbonUpperTag);
            }
        }
    }
    u->uploadStaticBuffer(dssFillVbo_, fill.data());
    u->uploadStaticBuffer(dssLineVbo_, line.data());
    dssHeadUploaded_ = -1;
    dssRowGenUploaded_ = dssLutToken_ = ~0ull;
    return dssReady_ = true;
}

void PanGpuView::render(QRhiCommandBuffer* cb) {
    QElapsedTimer t;
    t.start();
    QRhi* r = rhi_;
    PanadapterWidget* p = pan_;
    const QSize px = renderTarget()->pixelSize();
    const qreal dpr = devicePixelRatioF();
    const int W = p->width(), H = p->height();
    const int hSpec = p->spectrumHeight();
    const int wfTop = hSpec + PanadapterWidget::kScaleBandH;
    const PanadapterWidget::PanView v = p->panView();
    QRhiResourceUpdateBatch* u = r->nextResourceUpdateBatch();
    if (!quadUploaded_) {
        static const float quad[] = {-1.f, -1.f, 0.f, 1.f,  1.f, -1.f, 1.f, 1.f,
                                     -1.f,  1.f, 0.f, 0.f,  1.f,  1.f, 1.f, 0.f};
        u->uploadStaticBuffer(quadVbo_, quad);
        quadUploaded_ = true;
    }

    // ---- layers: the CPU paint's own QPainter code, only when changed ----
    QElapsedTimer lt;
    lt.start();
    bool rebuilt = false;
    // The waterfall timestamps + "WF n dB" label live in the left edge only:
    // their layer is a strip, so its per-row redraw uploads ~1/12 of a
    // screen, not all of it.
    const int stripW = std::min(kLabelStripPx, W);
    for (Layer* l : {&under_, &mid_, &top_, &cursor_}) ensureLayer(*l, px);
    ensureLayer(labels_, QSize(int(stripW * dpr), px.height()));
    if (p->gpuDirtyUnder_) under_.dirty = true;
    if (p->gpuDirtyMid_) mid_.dirty = true;
    if (p->gpuDirtyTop_) top_.dirty = true;
    if (p->gpuDirtyLabels_) labels_.dirty = true;
    const bool cursorDirty = p->gpuDirtyCursor_ || cursor_.dirty;
    p->gpuDirtyUnder_ = p->gpuDirtyMid_ = p->gpuDirtyTop_ = p->gpuDirtyLabels_ = false;
    p->gpuDirtyCursor_ = false;
    auto paint = [&](Layer& l, auto&& fn) {
        if (!l.dirty) return;
        l.img.setDevicePixelRatio(dpr);
        l.img.fill(Qt::transparent);
        {
            QPainter qp(&l.img);
            qp.setFont(p->font());
            fn(qp);
        }
        u->uploadTexture(l.tex, l.img);
        l.dirty = false;
        rebuilt = true;
    };
    paint(under_, [&](QPainter& qp) { p->paintUnder(qp, hSpec); });
    paint(mid_, [&](QPainter& qp) { p->paintMid(qp, hSpec); });
    paint(labels_, [&](QPainter& qp) { p->paintWfLabels(qp, hSpec, v); });
    paint(top_, [&](QPainter& qp) { p->paintTop(qp, hSpec, v.n, false); });
    // The hover cursor: its own layer, cleared and re-uploaded only where it
    // was and is (a strip around the line + its readout), not the screen.
    if (cursorDirty) {
        cursor_.img.setDevicePixelRatio(dpr);
        const QRect old = cursorDrawn_;
        {
            QPainter qp(&cursor_.img);
            qp.setFont(p->font());
            if (cursor_.dirty) {                   // new image: clear it all
                qp.setCompositionMode(QPainter::CompositionMode_Source);
                qp.fillRect(QRect(0, 0, W, H), Qt::transparent);
            } else if (!old.isEmpty()) {
                qp.setCompositionMode(QPainter::CompositionMode_Source);
                qp.fillRect(old, Qt::transparent);
            }
            qp.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p->drawCursor(qp, hSpec);
        }
        cursorDrawn_ = p->cursorRect_;
        QRect up = (cursor_.dirty ? QRect(0, 0, W, H) : old.united(cursorDrawn_))
                       .intersected(QRect(0, 0, W, H));
        if (!up.isEmpty()) {
            const QRect pxr(int(up.x() * dpr), int(up.y() * dpr), int(std::ceil(up.width() * dpr)),
                            int(std::ceil(up.height() * dpr)));
            const QRect clip = pxr.intersected(cursor_.img.rect());
            QRhiTextureSubresourceUploadDescription d(cursor_.img);
            d.setSourceTopLeft(clip.topLeft());
            d.setSourceSize(clip.size());
            d.setDestinationTopLeft(clip.topLeft());
            u->uploadTexture(cursor_.tex, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
        }
        cursor_.dirty = false;
        rebuilt = true;
    }
    const double layerMs = lt.nsecsElapsed() / 1e6;

    // ---- palette (the CPU path's own colour map) ----
    if (paletteId_ != p->ds_.palette) {
        QImage pal(256, 1, QImage::Format_RGBA8888);
        for (int i = 0; i < 256; ++i) pal.setPixel(i, 0, p->mapColor(i / 255.0f) | 0xff000000u);
        u->uploadTexture(palette_, pal);
        paletteId_ = p->ds_.palette;
    }

    // ---- waterfall history: only the rows that arrived ----
    const bool haveWf = p->wfBins_ >= 2 && v.n >= 2 && H > wfTop && p->wfCount_ > 0;
    if (p->wfBins_ >= 2) {
        ensureHist(p->wfBins_);
        const int rows = PanadapterWidget::kWfHistRows;
        const int bins = p->wfBins_;
        uint64_t from = histSerial_;
        if (histReset_ != p->wfReset_ || p->wfSerial_ - histSerial_ > uint64_t(rows)
            || p->wfSerial_ < histSerial_) {
            from = p->wfSerial_ > uint64_t(rows) ? p->wfSerial_ - rows : 0;
            histReset_ = p->wfReset_;
        }
        QVarLengthArray<QRhiTextureUploadEntry, 16> entries;
        for (uint64_t s = from; s < p->wfSerial_; ++s) {
            const int idx = int(s % rows);
            QRhiTextureSubresourceUploadDescription d(&p->wfHist_[size_t(idx) * bins],
                                                      quint32(bins * sizeof(float)));
            d.setSourceSize(QSize(bins, 1));
            d.setDestinationTopLeft(QPoint(0, idx));
            entries.append(QRhiTextureUploadEntry(0, 0, d));
        }
        if (qEnvironmentVariableIsSet("TTC_PANDBG"))
            std::fprintf(stderr, "[pandbg] serial %llu count %d bins %d: uploading %d rows (from %llu)\n",
                         (unsigned long long)p->wfSerial_, p->wfCount_, bins, int(entries.size()),
                         (unsigned long long)from);
        if (!entries.isEmpty()) {
            QRhiTextureUploadDescription desc;
            desc.setEntries(entries.cbegin(), entries.cend());
            u->uploadTexture(hist_, desc);
        }
        histSerial_ = p->wfSerial_;
    }

    // ---- per-column levels (fill + trace) ----
    const std::vector<float> colT = p->columnLevels(v);
    const int cw = std::max(2, int(colT.size()));
    if (!cols_ || colsW_ != cw) {
        delete cols_;
        cols_ = r->newTexture(QRhiTexture::R32F, QSize(cw, 1));
        cols_->create();
        colsW_ = cw;
        delete fillSrb_;
        fillSrb_ = nullptr;
        delete fillPipe_;
        fillPipe_ = nullptr;
    }
    if (!colT.empty()) {
        QRhiTextureSubresourceUploadDescription d(colT.data(), quint32(colT.size() * sizeof(float)));
        d.setSourceSize(QSize(int(colT.size()), 1));
        u->uploadTexture(cols_, QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
    }

    // ---- 3D stacked traces: AetherSDR's per-frame dss_mesh work ----
    if (p->view3d_ && !dssTried_) initDss(u);
    const bool want3d = p->view3d_ && dssReady_ && v.n >= 2 && hSpec > 40;
    int dssRows = 0;
    if (want3d) {
        DssRenderer& dss = p->dss_;
        const float floorDbm = p->dss3dFloorDbm();
        const float rangeDb = std::round((p->dbMax_ - p->dbMin_) * 2.0f) / 2.0f;
        // Palette LUT: the console's palette (or the heat colours with TRACE
        // = Heat map) over the colour aperture, "3D Gain" as its gamma
        // (their dssStrengthToRgb: 4^((50-gain)/50)). Re-baked on change only.
        const int gain = std::clamp(p->ds_.dssGain, 0, 100);
        const quint64 token = (quint64(p->ds_.palette) * 131 + quint64(gain)) * 2
                              + (p->ds_.traceStyle == 1 ? 1 : 0);
        if (token != dssLutToken_) {
            const float gamma = std::pow(4.0f, (50.0f - gain) / 50.0f);
            QImage lut(256, 1, QImage::Format_RGBA8888);
            for (int i = 0; i < 256; ++i) {
                const float t = std::pow(i / 255.0f, gamma);
                const QColor c = p->ds_.traceStyle == 1 ? PanadapterWidget::heatColor(t)
                                                        : QColor::fromRgb(p->mapColor(t));
                lut.setPixelColor(i, 0, QColor(c.red(), c.green(), c.blue()));
            }
            u->uploadTexture(dssLut_, lut);
            dssLutToken_ = token;
        }
        // Height ring: the rows pushed since the last frame (one entry
        // each), or the whole texture after a big catch-up.
        const int cols = dss.cols(), rows = dss.rows();
        const int head = dss.headRing(), valid = dss.rowCount();
        const quint64 rowGeneration = dss.rowGeneration();
        if (valid > 0 && rowGeneration != dssRowGenUploaded_) {
            const quint64 since = (dssRowGenUploaded_ == ~0ull || rowGeneration < dssRowGenUploaded_)
                ? quint64(valid) : rowGeneration - dssRowGenUploaded_;
            int newRows = (dssHeadUploaded_ < 0 || since >= quint64(rows)) ? valid : int(since);
            newRows = std::clamp(newRows, 0, valid);
            constexpr int kCh = 4;
            const int rowBytes = cols * kCh * int(sizeof(qfloat16));
            const auto pack = [&](int ring, qfloat16* dst) {
                const float* src = dss.rowDataRing(ring);
                const quint8* cov = dss.rowCoverageRing(ring);
                const float* sup = dss.rowSupplementalDataRing(ring);
                const quint8* supCov = dss.rowSupplementalCoverageRing(ring);
                for (int c = 0; c < cols; ++c) {
                    dst[c * kCh] = qfloat16(src[c]);
                    dst[c * kCh + 1] = qfloat16(cov[c] != 0 ? 1.0f : 0.0f);
                    dst[c * kCh + 2] = qfloat16(sup[c]);
                    dst[c * kCh + 3] = qfloat16(supCov[c] != 0 ? 1.0f : 0.0f);
                }
            };
            if (newRows > kDssMaxIncrementalUploadRows) {
                dssTexScratch_.resize(rows * rowBytes);
                for (int ring = 0; ring < rows; ++ring)
                    pack(ring, reinterpret_cast<qfloat16*>(dssTexScratch_.data() + ring * rowBytes));
                QRhiTextureSubresourceUploadDescription d;
                d.setData(dssTexScratch_);
                d.setSourceSize(QSize(cols, rows));
                u->uploadTexture(dssHeight_, QRhiTextureUploadEntry(0, 0, d));
            } else {
                QVarLengthArray<QRhiTextureUploadEntry, kDssMaxIncrementalUploadRows> entries;
                for (int k = 0; k < newRows; ++k) {
                    const int ring = (head + k) % rows;
                    dssRowScratch_.resize(rowBytes);
                    dssRowScratch_.detach();       // each entry keeps its own bytes
                    pack(ring, reinterpret_cast<qfloat16*>(dssRowScratch_.data()));
                    QRhiTextureSubresourceUploadDescription d;
                    d.setData(dssRowScratch_);
                    d.setSourceSize(QSize(cols, 1));
                    d.setDestinationTopLeft(QPoint(0, ring));
                    entries.append(QRhiTextureUploadEntry(0, 0, d));
                    dssRowScratch_ = QByteArray();
                }
                if (!entries.isEmpty()) {
                    QRhiTextureUploadDescription desc;
                    desc.setEntries(entries.cbegin(), entries.cend());
                    u->uploadTexture(dssHeight_, desc);
                }
            }
            dssHeadUploaded_ = head;
            dssRowGenUploaded_ = rowGeneration;
        }
        // Their row-span easing toward the Span control's share of the
        // overhang the newest rows carry.
        double tc = 0, tbw = 0;
        p->view3dFrame(tc, tbw);
        {
            const float target = DssRenderer::rowSpanFactorFor(
                dss.newestSupplementalBandwidthMhz(tbw), tbw, p->ds_.dssSpan);
            float& f = p->dssRowSpanFactor_;
            f += 0.12f * (target - f);
            if (std::abs(target - f) < 0.002f) f = target;
        }
        const float rowOffset = (float(head) + 0.5f) / float(rows);
        const QColor bg(12, 16, 22);
        const float uboScalars[] = {
            rowOffset,
            floorDbm, rangeDb, 0.70f,                       // their m_dssZCurve
            DssRenderer::kBackWidthFrac, DssRenderer::kDepthSpanFrac,
            DssRenderer::kFrontMaxRidgeFrac, DssRenderer::kHaze,
            float(cols),
            float(tbw),
            0.0f, 1.0f,                                     // centre offset, row frames on
            p->dss3dScrollRows(),
            float(rows), 1.0f,                              // one row per push
            std::min(rangeDb, DssRenderer::kColorSpanDb),
            float(valid),
            float(DssRenderer::kVisibleRows),
            float(W * dpr), float(hSpec * dpr),
            p->dssRowSpanFactor_,
            float(DssRenderer::kMeshCols),
            0.0f, 0.0f,                                     // std140 hole before bgFill
            float(bg.redF()), float(bg.greenF()), float(bg.blueF()), 1.0f,
        };
        static_assert(std::size(uboScalars) == 28, "dss_mesh UBO scalar prefix");
        std::array<float, kDssMeshUboFloats> ubo{};
        std::copy(std::begin(uboScalars), std::end(uboScalars), ubo.begin());
        // Slice-shadow decals (their writeShadowSlot/appendShadow): each VFO
        // passband darkens the surface in place with a cue line at its dial,
        // in view units widened by the row overhang. Inactive (B) first,
        // active (A) last; off unless "3D passband shading".
        constexpr int kShadowBandsOffset = 28;
        constexpr int kShadowStylesOffset = kShadowBandsOffset + kDssMeshShadowSlices * 4;
        constexpr int kShadowMetaOffset = kShadowStylesOffset + kDssMeshShadowSlices * 4;
        int shadowCount = 0;
        if (p->ds_.dssShadow && W > 0) {
            const float margin = (p->dssRowSpanFactor_ - 1.0f) * 0.5f;
            const float lo = -margin, hi = 1.0f + margin;
            const auto unit = [&](int offHz) { return float(p->hzToX(offHz)) / float(W); };
            const auto slot = [&](int offHz, int pbLo, int pbHi, const QColor& c, bool active) {
                float bl = unit(offHz + pbLo), bh = unit(offHz + pbHi);
                if (bl > bh) std::swap(bl, bh);
                const float cue = unit(offHz);
                const bool bandVisible = !(bh < lo || bl > hi);
                const bool cueVisible = cue >= lo && cue <= hi;
                if ((!bandVisible && !cueVisible) || shadowCount >= kDssMeshShadowSlices) return;
                const int b = kShadowBandsOffset + shadowCount * 4;
                ubo[size_t(b)] = std::clamp(bl, lo, hi);
                ubo[size_t(b + 1)] = std::clamp(bh, lo, hi);
                ubo[size_t(b + 2)] = std::clamp(cue, lo, hi);
                ubo[size_t(b + 3)] = bandVisible ? (active ? 0.42f : 0.17f) : 0.0f;
                const int st = kShadowStylesOffset + shadowCount * 4;
                ubo[size_t(st)] = float(c.redF());
                ubo[size_t(st + 1)] = float(c.greenF());
                ubo[size_t(st + 2)] = float(c.blueF());
                ubo[size_t(st + 3)] = cueVisible ? (active ? 0.36f : 0.11f) : 0.0f;
                ++shadowCount;
            };
            // The console's marker colours (paintTop): red TX, green RX, blue sub.
            const QColor txRed(224, 93, 93), rxGreen(62, 207, 122), subBlue(93, 178, 240);
            if (p->vfoBVisible_ && p->vfoBHz_ && p->centerHz_) {
                const qint64 off = qint64(p->vfoBHz_) - qint64(p->centerHz_);
                if (std::llabs(off) < 10'000'000)
                    slot(int(off), p->vfoBLo_, p->vfoBHi_,
                         p->vfoBRole_ == 'T' ? txRed : p->vfoBRole_ == 'R' ? rxGreen : subBlue,
                         false);
            }
            const QColor aCol = p->vfoBRole_ == 'T' ? rxGreen
                              : p->vfoBRole_ == 'R' ? txRed : QColor(200, 80, 80);
            slot(0, p->pbLoHz_, p->pbHiHz_, aCol, true);
        }
        ubo[kShadowMetaOffset] = float(shadowCount);
        ubo[kShadowMetaOffset + 1] = p->ds_.dssShadow ? 1.0f : 0.0f;
        ubo[kShadowMetaOffset + 2] = float(W);
        constexpr int kRowFramesOffset = kShadowMetaOffset + 4;
        for (int age = 0; age < DssRenderer::kRows; ++age) {
            double rc = dss.rowCenterMhzAtAge(age), rbw = dss.rowBandwidthMhzAtAge(age);
            if (!std::isfinite(rc) || !std::isfinite(rbw) || rc <= 0.0 || rbw <= 0.0) {
                rc = tc;
                rbw = tbw;
            }
            const int b = kRowFramesOffset + age * 4;
            ubo[size_t(b)] = float(rc - tc);
            ubo[size_t(b + 1)] = float(rbw);
            const double sc = dss.rowSupplementalCenterMhzAtAge(age);
            const double sbw = dss.rowSupplementalBandwidthMhzAtAge(age);
            if (std::isfinite(sc) && std::isfinite(sbw) && sc > 0.0 && sbw > 0.0) {
                ubo[size_t(b + 2)] = float(sc - tc);
                ubo[size_t(b + 3)] = float(sbw);
            }
        }
        static_assert(kRowFramesOffset + DssRenderer::kRows * 4 == kDssMeshUboFloats,
                      "row frames fill the UBO");
        u->updateDynamicBuffer(dssUbo_, 0, sizeof(ubo), ubo.data());
        dssRows = dss.visibleRowCount();
    }

    // ---- trace + peak-hold vertices (widget pixels) ----
    std::vector<float> verts;
    int traceN = 0, peakN = 0;
    if (v.n >= 2) {
        if (p->ds_.peakHold && p->peaks_.size() == p->spectrum_.size()) {
            const int nv = v.binHi - v.binLo;
            const float range = p->dbMax_ - p->dbMin_;
            for (int i = 0; i < nv; ++i) {
                const float x = float(i) / float(nv - 1) * float(W);
                const float tt = (p->peaks_[size_t(v.binLo + i)] - p->dbMin_) / range;
                verts.push_back(x);
                verts.push_back(float(hSpec) * (1.f - std::clamp(tt, 0.f, 1.f)));
            }
            peakN = nv;
        }
        for (int x = 0; x < int(colT.size()); ++x) {
            verts.push_back(float(x) + 0.5f);
            verts.push_back(float(hSpec) * (1.f - colT[size_t(x)]));
        }
        traceN = int(colT.size());
    }
    // Wide or heat trace (AetherSDR look): one anti-aliased quad per segment,
    // extended half a width at each end so the joins close. The flat 1 px
    // classic line stays a plain line strip (the original look).
    const bool wide = p->ds_.traceStyle == 1 || p->ds_.lineWidth > 1.0f;
    std::vector<float> quads;
    if (wide && traceN > 1) {
        const float h = p->ds_.lineWidth * 0.5f + 0.5f;
        quads.reserve(size_t(traceN - 1) * 18);
        for (int i = 0; i + 1 < traceN; ++i) {
            const float ax = verts[size_t(2 * (peakN + i))], ay = verts[size_t(2 * (peakN + i) + 1)];
            const float bx = verts[size_t(2 * (peakN + i + 1))], by = verts[size_t(2 * (peakN + i + 1) + 1)];
            float dx = bx - ax, dy = by - ay;
            const float len = std::max(1e-4f, std::sqrt(dx * dx + dy * dy));
            dx /= len; dy /= len;
            const float nx = -dy * h, ny = dx * h;
            const float a0x = ax - dx * h, a0y = ay - dy * h, b0x = bx + dx * h, b0y = by + dy * h;
            const float q[18] = {a0x + nx, a0y + ny, 1.f,  a0x - nx, a0y - ny, -1.f,  b0x + nx, b0y + ny, 1.f,
                                 b0x + nx, b0y + ny, 1.f,  a0x - nx, a0y - ny, -1.f,  b0x - nx, b0y - ny, -1.f};
            quads.insert(quads.end(), q, q + 18);
        }
        const int need = int(quads.size() * sizeof(float));
        if (!wideVbo_ || wideVboCap_ < need) {
            delete wideVbo_;
            wideVboCap_ = need * 2;
            wideVbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, wideVboCap_);
            wideVbo_->create();
        }
        u->updateDynamicBuffer(wideVbo_, 0, need, quads.data());
    }
    const int needBytes = std::max(16, int(verts.size() * sizeof(float)));
    if (!lineVbo_ || lineVboCap_ < needBytes) {
        delete lineVbo_;
        lineVboCap_ = needBytes * 2;
        lineVbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, lineVboCap_);
        lineVbo_->create();
    }
    if (!verts.empty()) u->updateDynamicBuffer(lineVbo_, 0, int(verts.size() * sizeof(float)), verts.data());

    // ---- uniforms ----
    const QMatrix4x4 mvp = r->clipSpaceCorrMatrix();
    auto quadUbo = [&](int i, const float* p0, const float* p1, const float* p2) {
        float b[28] = {};
        std::memcpy(b, mvp.constData(), 64);
        if (p0) std::memcpy(b + 16, p0, 16);
        if (p1) std::memcpy(b + 20, p1, 16);
        if (p2) std::memcpy(b + 24, p2, 16);
        u->updateDynamicBuffer(ubo_[i], 0, kQuadUbo, b);
    };
    for (int i : {uUnder, uMid, uLabels, uTop, uCursor}) quadUbo(i, nullptr, nullptr, nullptr);
    const float wf0[4] = {float(v.binLo), float(v.binHi), float(px.width()),
                          float(std::max(1.0, (H - wfTop) * dpr))};
    const float wf1[4] = {float(p->wfSerial_ % PanadapterWidget::kWfHistRows),
                          float(p->wfCount_), float(PanadapterWidget::kWfHistRows), 0.f};
    const float wf2[4] = {p->wfDbMin_, p->wfDbMax_, 0.f, 0.f};
    quadUbo(uWf, wf0, wf1, wf2);
    // The fill shader indexes columns in WIDGET pixels (one level each).
    const float fl0[4] = {float(cw), float(hSpec), PanadapterWidget::kFillGamma,
                          float(p->ds_.traceStyle)};
    quadUbo(uFill, fl0, nullptr, nullptr);
    auto lineUbo = [&](int i, const QColor& c, float alpha) {
        float b[24] = {};
        std::memcpy(b, mvp.constData(), 64);
        b[16] = float(c.redF() * alpha); b[17] = float(c.greenF() * alpha);
        b[18] = float(c.blueF() * alpha); b[19] = alpha;
        b[20] = float(W); b[21] = float(H);
        b[22] = (i == uTrace && p->ds_.traceStyle == 1) ? 1.f : 0.f;   // heat colour
        b[23] = float(hSpec);
        u->updateDynamicBuffer(ubo_[i], 0, kLineUbo, b);
    };
    lineUbo(uPeak, QColor(235, 235, 245), 150.f / 255.f);
    lineUbo(uTrace, PanadapterWidget::traceColor(p->ds_.traceColor), 1.f);

    // ---- pipelines (lazily, once the render pass is known) ----
    QRhiRenderPassDescriptor* rp = renderTarget()->renderPassDescriptor();
    QRhiGraphicsPipeline::TargetBlend premul;
    premul.enable = true;
    premul.srcColor = QRhiGraphicsPipeline::One;
    premul.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    premul.srcAlpha = QRhiGraphicsPipeline::One;
    premul.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    QRhiVertexInputLayout quadLayout;
    quadLayout.setBindings({{4 * sizeof(float)}});
    quadLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0},
                              {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
    auto makePipe = [&](const char* vs, const char* fs, QRhiShaderResourceBindings* layoutSrb,
                        bool blend, bool lines) {
        QRhiGraphicsPipeline* gp = r->newGraphicsPipeline();
        gp->setShaderStages({{QRhiShaderStage::Vertex, loadShader(vs)},
                             {QRhiShaderStage::Fragment, loadShader(fs)}});
        if (lines && std::strcmp(vs, "pan_trace.vert") == 0) {
            QRhiVertexInputLayout tl;
            tl.setBindings({{3 * sizeof(float)}});
            tl.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
            gp->setVertexInputLayout(tl);
            gp->setTopology(QRhiGraphicsPipeline::Triangles);
        } else if (lines) {
            QRhiVertexInputLayout ll;
            ll.setBindings({{2 * sizeof(float)}});
            ll.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
            gp->setVertexInputLayout(ll);
            gp->setTopology(QRhiGraphicsPipeline::LineStrip);
        } else {
            gp->setVertexInputLayout(quadLayout);
            gp->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        }
        if (blend) gp->setTargetBlends({premul});
        gp->setShaderResourceBindings(layoutSrb);
        gp->setRenderPassDescriptor(rp);
        gp->create();
        return gp;
    };
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    if (!wfSrb_) {
        wfSrb_ = r->newShaderResourceBindings();
        wfSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uWf]),
                             QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, hist_, nearest_),
                             QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, palette_, linear_)});
        wfSrb_->create();
    }
    if (!fillSrb_) {
        fillSrb_ = r->newShaderResourceBindings();
        fillSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uFill]),
                               QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, cols_, nearest_),
                               QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, palette_, linear_)});
        fillSrb_->create();
    }
    if (!peakSrb_) {
        peakSrb_ = r->newShaderResourceBindings();
        peakSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uPeak])});
        peakSrb_->create();
        traceSrb_ = r->newShaderResourceBindings();
        traceSrb_->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo_[uTrace])});
        traceSrb_->create();
    }
    if (!overlayPipe_) overlayPipe_ = makePipe("pan_quad.vert", "pan_overlay.frag", under_.srb, true, false);
    if (!wfPipe_) wfPipe_ = makePipe("pan_quad.vert", "pan_waterfall.frag", wfSrb_, false, false);
    if (!fillPipe_) fillPipe_ = makePipe("pan_quad.vert", "pan_fill.frag", fillSrb_, true, false);
    if (!linePipe_) linePipe_ = makePipe("pan_line.vert", "pan_line.frag", traceSrb_, true, true);
    if (!widePipe_) widePipe_ = makePipe("pan_trace.vert", "pan_trace.frag", traceSrb_, true, true);

    // ---- the frame, bottom to top ----
    cb->beginPass(renderTarget(), QColor(12, 16, 22), {1.0f, 0}, u);
    const QRhiCommandBuffer::VertexInput quadIn(quadVbo_, 0);
    const QRhiViewport full(0, 0, float(px.width()), float(px.height()));
    auto overlay = [&](Layer& l) {
        cb->setGraphicsPipeline(overlayPipe_);
        cb->setViewport(full);
        cb->setShaderResources(l.srb);
        cb->setVertexInput(0, 1, &quadIn);
        cb->draw(4);
    };
    // QRhi viewports are bottom-left based.
    auto region = [&](int top, int height) {
        return QRhiViewport(0, float(px.height() - (top + height) * dpr), float(px.width()),
                            float(height * dpr));
    };
    overlay(under_);
    if (haveWf) {
        cb->setGraphicsPipeline(wfPipe_);
        cb->setViewport(region(wfTop, H - wfTop));
        cb->setShaderResources(wfSrb_);
        cb->setVertexInput(0, 1, &quadIn);
        cb->draw(4);
    }
    if (want3d) {
        // Back to front: the deepest rows that have no data yet are skipped.
        // The ribbons follow on the SAME pipeline (see initDss).
        if (dssRows > 0) {
            const int skipped = DssRenderer::kVisibleRows - dssRows;
            cb->setGraphicsPipeline(dssPipe_);
            cb->setShaderResources(dssSrb_);
            cb->setViewport(region(0, hSpec));
            const QRhiCommandBuffer::VertexInput fillIn(dssFillVbo_, 0);
            cb->setVertexInput(0, 1, &fillIn);
            cb->draw(quint32(dssRows * kDssFillVertsPerRow), 1,
                     quint32(skipped * kDssFillVertsPerRow), 0);
            const QRhiCommandBuffer::VertexInput lineIn(dssLineVbo_, 0);
            cb->setVertexInput(0, 1, &lineIn);
            cb->draw(quint32(dssRows * kDssLineVertsPerRow), 1,
                     quint32(skipped * kDssLineVertsPerRow), 0);
        }
    } else if (p->ds_.fillTrace && v.n >= 2 && hSpec > 1 && !colT.empty()) {
        cb->setGraphicsPipeline(fillPipe_);
        cb->setViewport(region(0, hSpec));
        cb->setShaderResources(fillSrb_);
        cb->setVertexInput(0, 1, &quadIn);
        cb->draw(4);
    }
    overlay(mid_);
    cb->setGraphicsPipeline(overlayPipe_);
    cb->setViewport(QRhiViewport(0, 0, float(stripW * dpr), float(px.height())));
    cb->setShaderResources(labels_.srb);
    cb->setVertexInput(0, 1, &quadIn);
    cb->draw(4);
    if (traceN > 1 && !want3d) {
        const QRhiCommandBuffer::VertexInput lineIn(lineVbo_, 0);
        cb->setGraphicsPipeline(linePipe_);
        cb->setViewport(full);
        cb->setVertexInput(0, 1, &lineIn);
        if (peakN > 1) {
            cb->setShaderResources(peakSrb_);
            cb->draw(quint32(peakN));
        }
        if (!quads.empty()) {
            const QRhiCommandBuffer::VertexInput wideIn(wideVbo_, 0);
            cb->setGraphicsPipeline(widePipe_);
            cb->setVertexInput(0, 1, &wideIn);
            cb->setShaderResources(traceSrb_);
            cb->draw(quint32(quads.size() / 3));
        } else {
            cb->setShaderResources(traceSrb_);
            cb->draw(quint32(traceN), 1, quint32(peakN));
        }
    }
    overlay(top_);
    overlay(cursor_);
    cb->endPass();
    gGpuProf.add(t.nsecsElapsed() / 1e6, layerMs, rebuilt);
    // 3D scrolls continuously: draw again at the next display refresh.
    if (want3d) update();
}

} // namespace ttc
