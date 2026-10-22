// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef HAVE_GPU_PAN
// Built without the GPU panadapter (Qt < 6.7, or no ShaderTools/GuiPrivate):
// an inert stand-in so PanadapterWidget compiles the same; it is never made.
#include <QWidget>
namespace ttc {
class PanadapterWidget;
class PanGpuView : public QWidget {
public:
    explicit PanGpuView(QWidget* parent) : QWidget(parent) {}
};
}  // namespace ttc
#else
#include <QByteArray>
#include <QImage>
#include <QRhiWidget>
#include <QVector>
#include <cstdint>
#include <memory>
#include <vector>

class QRhi;
class QRhiBuffer;
class QRhiResourceUpdateBatch;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace ttc {

class PanadapterWidget;

// The panadapter on the GPU (QRhi over OpenGL here), AetherSDR's design: the
// waterfall lives on the card as a raw-dB float texture (the palette and the
// view window are shader parameters, so zoom/tune/contrast are free), the
// fill and trace are drawn per frame from one level per pixel column, and
// everything with text — grid, scales, VFOs, spots, the boxes — is the
// SAME QPainter code as the CPU paint, rendered into cached layer images
// only when something changed and composited by the GPU. Owned by, and
// stacked under the events of, PanadapterWidget; any failure falls back
// to the CPU paint (renderFailed).
class PanGpuView : public QRhiWidget {
    Q_OBJECT
public:
    explicit PanGpuView(PanadapterWidget* pan);
    ~PanGpuView() override;

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    struct Layer {
        QImage img;
        QRhiTexture* tex = nullptr;
        QRhiShaderResourceBindings* srb = nullptr;
        bool dirty = true;
    };
    void ensureLayer(Layer& l, const QSize& sz);
    void ensureHist(int bins);
    void freeAll();

    PanadapterWidget* pan_;
    QRhi* rhi_ = nullptr;
    QSize size_;
    // Geometry
    QRhiBuffer* quadVbo_ = nullptr;
    QRhiBuffer* lineVbo_ = nullptr;
    int lineVboCap_ = 0;
    QRhiBuffer* wideVbo_ = nullptr;                // wide/heat trace quads
    int wideVboCap_ = 0;
    // One uniform block per draw (they differ per frame).
    enum Ubo { uUnder, uWf, uFill, uMid, uLabels, uPeak, uTrace, uTop, uCursor, uCount };
    QRhiBuffer* ubo_[uCount] = {};
    QRhiSampler* linear_ = nullptr;
    QRhiSampler* nearest_ = nullptr;
    // Waterfall history + palette + per-column levels.
    QRhiTexture* hist_ = nullptr;
    int histBins_ = 0;
    uint64_t histSerial_ = 0, histReset_ = ~0ull;
    QRhiTexture* palette_ = nullptr;
    int paletteId_ = -1;
    QRhiTexture* cols_ = nullptr;
    int colsW_ = 0;
    // Layers (premultiplied QPainter images).
    Layer under_, mid_, labels_, top_, cursor_;
    QRect cursorDrawn_;                            // widget px, last frame's cursor
    // Pipelines
    QRhiGraphicsPipeline* overlayPipe_ = nullptr;
    QRhiGraphicsPipeline* wfPipe_ = nullptr;
    QRhiGraphicsPipeline* fillPipe_ = nullptr;
    QRhiGraphicsPipeline* linePipe_ = nullptr;
    QRhiGraphicsPipeline* widePipe_ = nullptr;
    QRhiShaderResourceBindings* wfSrb_ = nullptr;
    QRhiShaderResourceBindings* fillSrb_ = nullptr;
    QRhiShaderResourceBindings* peakSrb_ = nullptr;
    QRhiShaderResourceBindings* traceSrb_ = nullptr;
    // 3D stacked traces: AetherSDR's dss_mesh (src/ui/dss) — the RGBA16F
    // height ring (FFT dBm, coverage, overhang dBm, coverage), the "3D Gain"
    // palette LUT, the static mesh (curtains + ridge ribbons) and its UBO.
    // Built on the first 3D frame; RGBA16F missing = no 3D (2D stays).
    bool initDss(QRhiResourceUpdateBatch* u);
    QRhiTexture* dssHeight_ = nullptr;
    QRhiTexture* dssLut_ = nullptr;
    QRhiBuffer* dssFillVbo_ = nullptr;
    QRhiBuffer* dssLineVbo_ = nullptr;
    QRhiBuffer* dssUbo_ = nullptr;
    QRhiShaderResourceBindings* dssSrb_ = nullptr;
    QRhiGraphicsPipeline* dssPipe_ = nullptr;
    bool dssReady_ = false, dssTried_ = false;
    int dssHeadUploaded_ = -1;
    quint64 dssRowGenUploaded_ = ~0ull, dssLutToken_ = ~0ull;
    QByteArray dssRowScratch_, dssTexScratch_;
    bool pipesBuilt_ = false, quadUploaded_ = false;
};

} // namespace ttc
#endif  // HAVE_GPU_PAN
