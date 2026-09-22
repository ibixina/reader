#include "ui/PdfView.h"
#include "app/Application.h"
#include "document/DocumentModel.h"
#include "pdf/PdfSelection.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMetaObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPdfLink>
#include <QPdfLinkModel>
#include <QPdfPageNavigator>
#include <QPdfSelection>
#include <QPointer>
#include <QScrollBar>
#include <QResizeEvent>
#include <QShortcut>
#include <QSplitter>
#include <QTextEdit>
#include <QTimer>
#include <QTransform>
#include <QToolTip>
#include <QUuid>
#include <QVBoxLayout>
#include <QLabel>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

class PageWidget : public QLabel {
    Q_OBJECT
public:
    PageWidget(int page, const QSizeF& pageSize, std::shared_ptr<reader::PdfRenderer> renderer,
               reader::SelectionIndex* selIndex, double zoom, int rotation,
               QWidget* parent = nullptr)
        : QLabel(parent), page_(page), renderer_(std::move(renderer)),
          selIndex_(selIndex), zoom_(zoom), rotation_(rotation) {
        setObjectName(QString("pdfPage_%1").arg(page));
        pageWidth_ = pageSize.width();
        pageHeight_ = pageSize.height();
        setFixedSize(qMax(1, qRound(rotatedWidth() * zoom)), qMax(1, qRound(rotatedHeight() * zoom)));
        setCursor(Qt::IBeamCursor);
        setFocusPolicy(Qt::NoFocus);
    }
    void setHighlight(QRectF r) {
        highlight_ = r;
        update();
    }
    void setHoverHighlight(QRectF r) {
        hoverHighlight_ = r;
        update();
    }
    void clearHoverHighlight() {
        hoverHighlight_ = {};
        update();
    }
    // Ready-to-paint underline segments + fallback rects, all in points.
    // Pure data: no engine calls here or in paint (§7).
    void setAiLines(QList<QPair<QPointF, QPointF>> lines, QList<QRectF> fallback) {
        aiLines_ = std::move(lines);
        aiFallback_ = std::move(fallback);
        update();
    }
    void setLines(std::vector<reader::TextSpan> lines) { lines_ = std::move(lines); }
    int pageIndex() const { return page_; }
    void setUserOverlays(QList<QPair<QRectF, QString>> overlays) {
        userOverlays_ = std::move(overlays);
        update();
    }
    void setRotation(int rotation) {
        rotation_ = ((rotation % 360) + 360) % 360;
        setFixedSize(qMax(1, qRound(rotatedWidth() * zoom_)),
                     qMax(1, qRound(rotatedHeight() * zoom_)));
        cache_ = {};
        preview_ = {};
        previewFor_ = {};
        requestedSource_ = {};
        renderPending_ = false;
        previewPending_ = false;
        update();
    }
    void clearSelection() {
        precise_.clear();
        update();
    }
    // Widget scale: logical pixels per PDF point. Single source of truth
    // for both rendering and selection mapping (DPR-safe).
    double scale() const {
        return zoom_;
    }

signals:
    void dragSelected(int page, QRectF rectPoints, QString text);
    void clickedAt(int page, QPointF pointPoints);

protected:
    void paintEvent(QPaintEvent* ev) override {
        // Raster requests are asynchronous. Painting only blits an already
        // cached image and overlays, so scrolling never performs PDF work on
        // the UI thread (§6/§7). Poppler rasterizes glyphs for the exact
        // device pixel size, which measures sharper (3x edge energy at
        // 67.8%/100%/150%/200% zoom) than supersampling + bilinear
        // downscale, so no supersampling is applied here.
        qreal dpr = devicePixelRatioF();
        if (dpr <= 0) dpr = 1;
        const QSize sourcePx(qMax(1, qRound(pageWidth_ * zoom_ * dpr)),
                             qMax(1, qRound(pageHeight_ * zoom_ * dpr)));
        const QSize expectedPx =
            (rotation_ == 90 || rotation_ == 270) ? QSize(sourcePx.height(), sourcePx.width())
                                                  : sourcePx;
        QRect exposed = ev->region().boundingRect().intersected(rect());
        if (exposed.isEmpty()) exposed = rect();
        const QRect visibleSource = sourceRect(exposed, dpr).intersected(
            QRect(QPoint(0, 0), sourcePx));
        // Blur-up placeholder: a ~300px render lands in ~20ms so the page
        // is never blank while sharp tiles rasterize in parallel.
        if (preview_.isNull() || previewFor_ != sourcePx) {
            if (previewFor_ != sourcePx) {
                preview_ = {};
                previewFor_ = {};
            }
            if (!previewPending_ && renderer_ && cache_.isNull()) {
                previewPending_ = true;
                QPointer<PageWidget> guard(this);
                const int renderRotation = rotation_;
                renderer_->requestPreview(page_, sourcePx,
                                          [guard, sourcePx, renderRotation](QImage image) {
                                              if (!guard) return;
                                              guard->previewPending_ = false;
                                              if (image.isNull()) return;
                                              if (renderRotation != 0)
                                                  image = image.transformed(
                                                      QTransform().rotate(renderRotation),
                                                      Qt::SmoothTransformation);
                                              guard->preview_ = std::move(image);
                                              guard->previewFor_ = sourcePx;
                                              if (guard->cache_.isNull()) guard->update();
                                          });
            }
        }
        if ((cache_.isNull() || cache_.size() != expectedPx ||
             visibleSource != requestedSource_) &&
            !renderPending_ && renderer_) {
            renderPending_ = true;
            QPointer<PageWidget> guard(this);
            const int renderRotation = rotation_;
            renderer_->requestTiles(page_, static_cast<int>(std::lround(zoom_ * 1000)),
                                    sourcePx, renderRotation, 512,
                                    [guard, renderRotation, dpr](QImage image) {
                if (!guard) return;
                if (image.isNull()) {
                    guard->renderPending_ = false;
                    guard->requestedSource_ = {};
                    return;
                }
                if (renderRotation != 0)
                    // Multiples of 90 degrees are lossless: a fast transform
                    // avoids the resampling blur of smooth rotation.
                    image = image.transformed(QTransform().rotate(renderRotation),
                                              Qt::FastTransformation);
                guard->cache_ = std::move(image);
                guard->cache_.setDevicePixelRatio(dpr);
                guard->renderPending_ = false;
                guard->update();
            }, visibleSource);
            requestedSource_ = visibleSource;
        }
        QPainter p(this);
        p.fillRect(rect(), Qt::white);
        if (!cache_.isNull()) {
            p.drawImage(rect(), cache_);
        } else if (!preview_.isNull() && previewFor_ == sourcePx) {
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(rect(), preview_);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        }
        // User highlights: pale yellow wash, saved to SQLite.
        if (!userOverlays_.isEmpty()) {
            for (const auto& [r, kind] : userOverlays_) {
                const QRectF mapped = mapRect(r);
                if (kind == "underline") {
                    p.setPen(QPen(QColor(79, 140, 255), 2));
                    p.drawLine(mapped.bottomLeft(), mapped.bottomRight());
                } else if (kind == "strikethrough") {
                    p.setPen(QPen(QColor(224, 82, 82), 2));
                    p.drawLine(mapped.left(), mapped.center().y(), mapped.right(),
                               mapped.center().y());
                } else if (kind == "note" || kind == "bookmark") {
                    p.setPen(QPen(QColor(120, 80, 180), 2));
                    p.setBrush(QColor(180, 120, 220, 170));
                    p.drawEllipse(mapped.topLeft() + QPointF(-4, -4), 4, 4);
                } else {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(255, 224, 102, 110));
                    p.drawRect(mapped);
                }
            }
        }
        // AI emphasis (§5.9): precomputed red underlines; blocks without
        // word geometry fall back to a red outline.
        if (!aiLines_.isEmpty() || !aiFallback_.isEmpty()) {
            p.setPen(QPen(QColor(220, 30, 30), 2));
            for (const auto& [a, b] : aiLines_)
                p.drawLine(mapPoint(a), mapPoint(b));
            for (const QRectF& r : aiFallback_)
                p.drawRect(mapRect(r));
        }
        // Tight per-fragment highlight from the engine (§55): what you see
        // is exactly the text that became AI context.
        if (!precise_.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(60, 120, 255, 80));
            for (const QPolygonF& poly : precise_) {
                QPolygonF mapped;
                for (const QPointF& pt : poly) mapped << mapPoint(pt);
                p.drawPolygon(mapped);
            }
        }
        if (highlight_.isValid()) {
            p.fillRect(mapRect(highlight_), QColor(255, 200, 60, 110));
        }
        if (hoverHighlight_.isValid()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(40, 150, 220, 220), 2));
            p.drawRect(mapRect(hoverHighlight_));
        }
        (void)ev;
    }
    void mousePressEvent(QMouseEvent* ev) override {
        if (ev->button() != Qt::LeftButton) return;
        // Snapshot once per gesture: consistent even if the background
        // indexer lands mid-drag. Zero engine calls on this thread.
        snap_ = selIndex_ ? selIndex_->snapshot(page_) : reader::PageWords{};
        origin_ = ev->pos();
        QPointF pt = toPdf(QPointF(ev->pos()));
        if (snap_.ready) {
            anchorWord_ = wordAt(snap_.words, pt);
            anchorIsWord_ = anchorWord_ >= 0;
        } else {
            anchorWord_ = -1;
            anchorIsWord_ = false;
        }
        if (!anchorIsWord_) anchorLine_ = lineAt(pt);
        // Fresh press starts a new selection; release without drag clears.
        precise_.clear();
        liveText_.clear();
        update();
    }
    void mouseMoveEvent(QMouseEvent* ev) override {
        if (!(ev->buttons() & Qt::LeftButton)) return;
        if ((ev->pos() - origin_).manhattanLength() < 4) return;
        updateLive(ev->pos());
    }
    void mouseReleaseEvent(QMouseEvent* ev) override {
        if (ev->button() != Qt::LeftButton) return;
        if ((ev->pos() - origin_).manhattanLength() < 4) {
            // Plain click clears (standard reader behavior).
            precise_.clear();
            liveText_.clear();
            update();
            emit clickedAt(page_, toPdf(QPointF(ev->pos())));
            emit dragSelected(page_, QRectF(), QString());
            return;
        }
        updateLive(ev->pos());
        if (!liveText_.trimmed().isEmpty())
            emit dragSelected(page_, liveRect_, liveText_);
    }
    void mouseDoubleClickEvent(QMouseEvent* ev) override {
        reader::PageWords snap = selIndex_ ? selIndex_->snapshot(page_) : reader::PageWords{};
        if (!snap.ready) return;
        int idx = wordAt(snap.words, toPdf(QPointF(ev->pos())));
        if (idx < 0) return;
        const reader::WordBox& w = snap.words[idx];
        precise_ = {QPolygonF(w.rect)};
        liveText_ = w.text;
        liveRect_ = w.rect;
        update();
        emit dragSelected(page_, liveRect_, liveText_);
    }

private:
    static int wordAt(const std::vector<reader::WordBox>& words, const QPointF& pt) {
        for (int k = 0; k < (int)words.size(); ++k)
            if (words[k].rect.contains(pt)) return k;
        // Nearest word within a small tolerance (clicks on gaps).
        int best = -1;
        double bestDist = 100.0; // 10pt squared
        for (int k = 0; k < (int)words.size(); ++k) {
            const QRectF& r = words[k].rect;
            double dx = 0, dy = 0;
            if (pt.x() < r.left()) dx = r.left() - pt.x();
            else if (pt.x() > r.right()) dx = pt.x() - r.right();
            if (pt.y() < r.top()) dy = r.top() - pt.y();
            else if (pt.y() > r.bottom()) dy = pt.y() - r.bottom();
            double d = dx * dx + dy * dy;
            if (d < bestDist) {
                bestDist = d;
                best = k;
            }
        }
        return best;
    }
    int lineAt(const QPointF& pt) const {
        for (int k = 0; k < (int)lines_.size(); ++k) {
            const auto& b = lines_[k].bounds;
            if (QRectF(b.x, b.y, b.width, b.height).contains(pt)) return k;
        }
        // Nearest line: vertical distance weighs 4x (lines are wide).
        int best = -1;
        double bestDist = 1e30;
        for (int k = 0; k < (int)lines_.size(); ++k) {
            const auto& b = lines_[k].bounds;
            double dy = pt.y() < b.y ? (b.y - pt.y()) * 4
                        : pt.y() > b.y + b.height ? (pt.y() - b.y - b.height) * 4
                                                  : 0;
            double dx = pt.x() < b.x                               ? b.x - pt.x()
                        : pt.x() > b.x + b.width ? pt.x() - b.x - b.width
                                                 : 0;
            double d = dy + dx;
            if (d < bestDist && d < 40.0) {
                bestDist = d;
                best = k;
            }
        }
        return best;
    }
    void updateLive(const QPoint& widgetPos) {
        QPointF pt = toPdf(QPointF(widgetPos));
        if (snap_.ready && anchorIsWord_) {
            int cur = wordAt(snap_.words, pt);
            if (anchorWord_ < 0 || cur < 0) {
                precise_.clear();
                liveText_.clear();
                update();
                return;
            }
            // Index-ordered slice: exact reading order, tight word rects.
            int a = qMin(anchorWord_, cur), b = qMax(anchorWord_, cur);
            const auto& words = snap_.words;
            precise_.clear();
            double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            for (int k = a; k <= b; ++k) {
                const QRectF& r = words[k].rect;
                precise_.push_back(QPolygonF(r));
                x0 = qMin(x0, r.left());
                y0 = qMin(y0, r.top());
                x1 = qMax(x1, r.right());
                y1 = qMax(y1, r.bottom());
            }
            const auto& wa = words[a];
            const auto& wb = words[b];
            liveText_ =
                snap_.pageText.mid(wa.startIndex, wb.startIndex + wb.length - wa.startIndex);
            liveRect_ = QRectF(x0, y0, x1 - x0, y1 - y0);
            update();
            return;
        }
        // Line-granularity fallback (index not ready yet): proper reading
        // order from cached fragments, no engine involved.
        int cur = lineAt(pt);
        if (anchorLine_ < 0 || cur < 0) {
            precise_.clear();
            liveText_.clear();
            update();
            return;
        }
        int a = qMin(anchorLine_, cur), b = qMax(anchorLine_, cur);
        precise_.clear();
        QStringList parts;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (int k = a; k <= b; ++k) {
            const auto& lb = lines_[k].bounds;
            QRectF r(lb.x, lb.y, lb.width, lb.height);
            precise_.push_back(QPolygonF(r));
            parts.push_back(QString::fromStdString(lines_[k].text));
            x0 = qMin(x0, r.left());
            y0 = qMin(y0, r.top());
            x1 = qMax(x1, r.right());
            y1 = qMax(y1, r.bottom());
        }
        liveText_ = parts.join("\n");
        liveRect_ = QRectF(x0, y0, x1 - x0, y1 - y0);
        update();
    }

    QPointF toPdf(const QPointF& widget) const {
        QPointF p = widget / zoom_;
        switch (rotation_) {
            case 90: return {p.y(), pageHeight_ - p.x()};
            case 180: return {pageWidth_ - p.x(), pageHeight_ - p.y()};
            case 270: return {pageWidth_ - p.y(), p.x()};
            default: return p;
        }
    }
    QPointF mapPoint(const QPointF& pdf) const {
        QPointF p = pdf;
        switch (rotation_) {
            case 90: p = {pageHeight_ - pdf.y(), pdf.x()}; break;
            case 180: p = {pageWidth_ - pdf.x(), pageHeight_ - pdf.y()}; break;
            case 270: p = {pdf.y(), pageWidth_ - pdf.x()}; break;
            default: break;
        }
        return p * zoom_;
    }
    QRectF mapRect(const QRectF& r) const {
        QPolygonF poly;
        poly << mapPoint(r.topLeft()) << mapPoint(r.topRight())
             << mapPoint(r.bottomRight()) << mapPoint(r.bottomLeft());
        return poly.boundingRect();
    }
    QRect sourceRect(const QRect& widgetRect, qreal dpr) const {
        const QPointF a = toPdf(widgetRect.topLeft());
        const QPointF b = toPdf(widgetRect.topRight());
        const QPointF c = toPdf(widgetRect.bottomRight());
        const QPointF d = toPdf(widgetRect.bottomLeft());
        const qreal left = qMin(qMin(a.x(), b.x()), qMin(c.x(), d.x()));
        const qreal right = qMax(qMax(a.x(), b.x()), qMax(c.x(), d.x()));
        const qreal top = qMin(qMin(a.y(), b.y()), qMin(c.y(), d.y()));
        const qreal bottom = qMax(qMax(a.y(), b.y()), qMax(c.y(), d.y()));
        return QRect(qFloor(left * zoom_ * dpr), qFloor(top * zoom_ * dpr),
                     qCeil((right - left) * zoom_ * dpr),
                     qCeil((bottom - top) * zoom_ * dpr));
    }
    double rotatedWidth() const { return rotation_ == 90 || rotation_ == 270 ? pageHeight_ : pageWidth_; }
    double rotatedHeight() const { return rotation_ == 90 || rotation_ == 270 ? pageWidth_ : pageHeight_; }

    int page_;
    std::shared_ptr<reader::PdfRenderer> renderer_;
    reader::SelectionIndex* selIndex_;
    double zoom_;
    int rotation_ = 0;
    double pageWidth_ = 0;
    double pageHeight_ = 0;
    QPoint origin_;
    reader::PageWords snap_;
    int anchorWord_ = -1;
    int anchorLine_ = -1;
    bool anchorIsWord_ = false;
    std::vector<reader::TextSpan> lines_;
    QList<QPolygonF> precise_;
    QString liveText_;
    QRectF liveRect_;
    QRectF highlight_;
    QRectF hoverHighlight_;
    QList<QPair<QPointF, QPointF>> aiLines_;
    QList<QRectF> aiFallback_;
    QList<QPair<QRectF, QString>> userOverlays_;
    QImage cache_;
    QImage preview_;
    QSize previewFor_;
    QRect requestedSource_;
    bool renderPending_ = false;
    bool previewPending_ = false;
};

PdfView::PdfView(reader::Application* app, QWidget* parent)
    : QScrollArea(parent), app_(app), docGen_(std::make_shared<std::atomic<unsigned long>>(0)),
      selIndex_(std::make_shared<reader::SelectionIndex>()),
      renderer_(std::make_unique<reader::PdfRenderer>()) {
    setWidgetResizable(true);
    pageHost_ = new QWidget(this);
    pageHost_->setLayout(new QVBoxLayout(pageHost_));
    setWidget(pageHost_);
    verticalScrollBar()->setSingleStep(40);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { updateCurrentPage(); });
    // Neighbor-page pixel prefetch waits for a scroll pause: firing four
    // full-page renders on every scrollbar tick is what wedged the render
    // queue during fast scrolling.
    prefetchTimer_ = new QTimer(this);
    prefetchTimer_->setSingleShot(true);
    prefetchTimer_->setInterval(180);
    connect(prefetchTimer_, &QTimer::timeout, this, [this] {
        const int page = pendingPrefetchPage_;
        pendingPrefetchPage_ = -1;
        prefetchAround(page);
        prefetchPixelsAround(page);
    });
    // Explicit copy only (§13): selections feed AI context, never the
    // system clipboard unless the user presses Ctrl+C. Shortcuts don't
    // steal focus from the Ask box (§18).
    auto* copy = new QShortcut(QKeySequence::Copy, this);
    copy->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copy, &QShortcut::activated, this, [this] {
        auto ctx = app_->context.currentContext();
        QString text;
        if (!ctx.temporary.empty()) text = QString::fromStdString(ctx.temporary.front().extractedText);
        if (!text.isEmpty()) QGuiApplication::clipboard()->setText(text);
    });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WidgetWithChildrenShortcut);
    connect(esc, &QShortcut::activated, this, [this] {
        clearAllSelections();
        app_->context.clearCurrentSelection();
        selection_.reset();
    });
}

PdfView::~PdfView() = default;

int PdfView::pageCount() const {
    return pageCount_;
}

void PdfView::requestThumbnail(int page, const QSize& size,
                               std::function<void(const QImage&)> callback) {
    if (!renderer_ || page < 0 || page >= pageCount_ || size.isEmpty()) return;
    // Fit the page aspect inside the bounding box instead of stretching,
    // and render at 2x so icons stay sharp on HiDPI. The zoom bucket uses
    // the fitted width so distinct aspects don't collide in the cache.
    QSize fitted = size;
    const QSizeF points = pageSizes_[page];
    if (!points.isEmpty()) {
        const double scale = std::min(size.width() / points.width(),
                                      size.height() / points.height());
        fitted = QSize(qMax(1, qRound(points.width() * scale * 2)),
                       qMax(1, qRound(points.height() * scale * 2)));
    }
    renderer_->requestPage(page, -fitted.width(), fitted, 0,
                           [callback = std::move(callback)](const QImage& image) {
                               if (callback) callback(image);
                           });
}

void PdfView::requestAnchorImage(const reader::DocumentAnchor& anchor,
                                 std::function<void(const QImage&)> callback) {
    if (!callback || !renderer_ || anchor.page < 0 || anchor.page >= pageCount_ ||
        !anchor.bounds.valid()) {
        if (callback) callback({});
        return;
    }
    const QSizeF points = pageSizes_[anchor.page];
    if (points.isEmpty()) {
        callback({});
        return;
    }
    // Keep object evidence bounded while retaining enough resolution for a
    // figure/table card. Rendering is limited to tiles intersecting the crop.
    const double scale = std::min(2.0, 1400.0 / std::max(points.width(), points.height()));
    const QSize pageSize(qMax(1, qRound(points.width() * scale)),
                         qMax(1, qRound(points.height() * scale)));
    const QRect crop(qFloor(anchor.bounds.x * scale), qFloor(anchor.bounds.y * scale),
                     qCeil(anchor.bounds.width * scale), qCeil(anchor.bounds.height * scale));
    const QRect visible = crop.intersected(QRect(QPoint(0, 0), pageSize));
    if (visible.isEmpty()) {
        callback({});
        return;
    }
    renderer_->requestTiles(
        anchor.page, qRound(scale * 1000.0), pageSize, 0, 512,
        [callback = std::move(callback), visible](const QImage& page) mutable {
            if (page.isNull()) {
                callback({});
                return;
            }
            callback(page.copy(visible));
        },
        visible);
}

void PdfView::configureSemantic(const reader::EmbeddingProviderQtConfig& config) {
    std::lock_guard<std::mutex> lock(semanticState_->mutex);
    semanticState_->provider = std::make_shared<reader::EmbeddingProviderQt>(config);
}

std::optional<reader::SemanticSearchSnapshot> PdfView::semanticSnapshot() const {
    std::lock_guard<std::mutex> lock(semanticState_->mutex);
    if (!semanticState_->provider || !semanticState_->index.semanticReady())
        return std::nullopt;
    return reader::SemanticSearchSnapshot{semanticState_->index, semanticState_->provider};
}

void PdfView::requestReferenceImage(
    const reader::ContextReference& reference,
    std::function<void(std::optional<reader::ReferenceImage>)> callback) {
    if (!callback) return;
    if ((reference.type != reader::ReferenceType::Figure &&
         reference.type != reader::ReferenceType::Table) ||
        !reference.anchor.bounds.valid()) {
        callback(std::nullopt);
        return;
    }
    requestAnchorImage(reference.anchor, [callback = std::move(callback)](const QImage& image) mutable {
        if (image.isNull()) {
            callback(std::nullopt);
            return;
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
            callback(std::nullopt);
            return;
        }
        reader::ReferenceImage encoded;
        encoded.mimeType = "image/png";
        encoded.bytes.assign(bytes.cbegin(), bytes.cend());
        encoded.width = image.width();
        encoded.height = image.height();
        callback(std::move(encoded));
    });
}

void PdfView::loadSemanticCacheAsync(std::function<void(bool, const QString&)> callback) {
    std::shared_ptr<reader::EmbeddingProviderQt> provider;
    {
        std::lock_guard<std::mutex> lock(semanticState_->mutex);
        provider = semanticState_->provider;
    }
    if (!provider || app_->model.document.fileHash.empty()) {
        if (callback) callback(false, "Embedding cache is not configured for this paper.");
        return;
    }
    const auto state = semanticState_;
    const auto documentGeneration = docGen_;
    const unsigned long generation = documentGeneration->load();
    const reader::DocumentModel model = app_->model;
    QPointer<PdfView> guard(this);
    app_->searchPool.submit(
        [state, provider, documentGeneration, generation, model, guard,
         callback = std::move(callback)]() mutable {
            std::string error;
            bool loaded = false;
            if (generation == documentGeneration->load()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                loaded = state->index.loadSemanticCache(model, model.document.fileHash,
                                                        *provider, &error);
            } else {
                error = "document changed while loading semantic index";
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [guard, documentGeneration, generation, loaded,
                 error = QString::fromStdString(error), callback = std::move(callback)]() mutable {
                    if (!guard || generation != documentGeneration->load()) return;
                    if (callback) callback(loaded, error);
                },
                Qt::QueuedConnection);
        });
}

bool PdfView::semanticReady() const {
    std::lock_guard<std::mutex> lock(semanticState_->mutex);
    return semanticState_->provider && semanticState_->index.semanticReady();
}

void PdfView::buildSemanticIndexAsync(
    std::size_t maxBlocks, std::function<void(bool, const QString&)> callback) {
    std::shared_ptr<reader::EmbeddingProviderQt> provider;
    {
        std::lock_guard<std::mutex> lock(semanticState_->mutex);
        provider = semanticState_->provider;
    }
    if (!provider) {
        callback(false, "Configure an embedding endpoint before building the semantic index.");
        return;
    }
    const auto state = semanticState_;
    const auto documentGeneration = docGen_;
    const unsigned long generation = documentGeneration->load();
    const reader::DocumentModel model = app_->model;
    const bool persist = app_->state.settings.storeEmbeddingsLocally;
    const reader::CancellationToken token = semanticToken_;
    QPointer<PdfView> guard(this);
    app_->searchPool.submit(
        [state, provider, documentGeneration, generation, model, maxBlocks, token, persist, guard,
         callback = std::move(callback)]() mutable {
            bool ok = false;
            std::string error;
            if (generation != documentGeneration->load()) {
                error = "document changed while building semantic index";
            } else {
                std::lock_guard<std::mutex> lock(state->mutex);
                ok = state->index.buildSemantic(model, *provider, token, maxBlocks, &error,
                                                persist);
            }
            QCoreApplication* app = QCoreApplication::instance();
            if (!app) return;
            QMetaObject::invokeMethod(
                app,
                [guard, documentGeneration, generation, ok, error = QString::fromStdString(error),
                 callback = std::move(callback)]() mutable {
                    if (!guard) return;
                    if (generation != documentGeneration->load()) {
                        callback(false, "document changed while building semantic index");
                        return;
                    }
                    callback(ok, error);
                },
                Qt::QueuedConnection);
        },
        token);
}

void PdfView::searchSemanticAsync(
    const std::string& query, std::size_t limit,
    std::function<void(std::vector<reader::VectorIndex::Hit>, const QString&)> callback) {
    std::shared_ptr<reader::EmbeddingProviderQt> provider;
    {
        std::lock_guard<std::mutex> lock(semanticState_->mutex);
        provider = semanticState_->provider;
        if (!provider || !semanticState_->index.semanticReady()) {
            callback({}, "Build the semantic index before searching.");
            return;
        }
    }
    const auto state = semanticState_;
    const auto documentGeneration = docGen_;
    const unsigned long generation = documentGeneration->load();
    const reader::DocumentModel model = app_->model;
    const reader::CancellationToken token = semanticToken_;
    QPointer<PdfView> guard(this);
    app_->searchPool.submit(
        [state, provider, documentGeneration, generation, model, query, limit, token, guard,
         callback = std::move(callback)]() mutable {
            std::vector<reader::VectorIndex::Hit> hits;
            std::string error;
            if (generation != documentGeneration->load()) {
                error = "document changed while searching";
            } else {
                std::lock_guard<std::mutex> lock(state->mutex);
                hits = state->index.querySemantic(model, query, *provider, limit, token, &error);
            }
            QCoreApplication* app = QCoreApplication::instance();
            if (!app) return;
            QMetaObject::invokeMethod(
                app,
                [guard, documentGeneration, generation, hits = std::move(hits),
                 error = QString::fromStdString(error), callback = std::move(callback)]() mutable {
                    if (!guard) return;
                    if (generation != documentGeneration->load()) {
                        callback({}, "document changed while searching");
                        return;
                    }
                    callback(std::move(hits), error);
                },
                Qt::QueuedConnection);
        },
        token);
}

void PdfView::clearAllSelections() {
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i)
        if (auto* w = qobject_cast<PageWidget*>(layout->itemAt(i)->widget())) w->clearSelection();
}

void PdfView::rebuildPages() {
    QLayout* layout = pageHost_->layout();
    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        delete item->widget();
        delete item;
    }
    if (pageCount_ == 0) return;
    int first = pageMode_ ? std::clamp(currentPage_, 0, pageCount_ - 1) : 0;
    int last = pageMode_ ? first + 1 : pageCount_;
    for (int i = first; i < last; ++i) {
        auto* w = new PageWidget(
            i, pageSizes_[i], std::shared_ptr<reader::PdfRenderer>(renderer_.get(),
                                                                  [](reader::PdfRenderer*) {}),
            selIndex_.get(), zoom_, rotation_, pageHost_);
        connect(w, &PageWidget::clickedAt, this, [this](int page, QPointF point) {
            suppressNextClickClear_ = false;
            const QPdfLink link = linkAt(page, point);
            if (link.isValid()) {
                suppressNextClickClear_ = true;
                emit linkActivated(link.page(), link.url().toString());
                if (link.url().isEmpty() && link.page() >= 0 && link.page() != page)
                    goToPage(link.page());
                return;
            }
            for (const auto& eq : app_->model.equations) {
                if (eq.page == page && QRectF(eq.bounds.x, eq.bounds.y, eq.bounds.width,
                                             eq.bounds.height)
                                            .contains(point)) {
                    auto anchor = reader::anchorForEquation(app_->model, eq);
                    suppressNextClickClear_ = true;
                    emit objectClicked(anchor, "equation");
                    return;
                }
            }
            for (const auto& fig : app_->model.figures) {
                if (fig.page == page && QRectF(fig.bounds.x, fig.bounds.y, fig.bounds.width,
                                               fig.bounds.height)
                                              .contains(point)) {
                    auto anchor = reader::anchorForFigure(app_->model, fig);
                    suppressNextClickClear_ = true;
                    emit objectClicked(anchor, "figure");
                    return;
                }
            }
            for (const auto& table : app_->model.tables) {
                if (table.page == page && QRectF(table.bounds.x, table.bounds.y,
                                                 table.bounds.width, table.bounds.height)
                                                    .contains(point)) {
                    auto anchor = reader::anchorForTable(app_->model, table);
                    suppressNextClickClear_ = true;
                    emit objectClicked(anchor, "table");
                    return;
                }
            }
            for (const auto& citation : app_->model.citations) {
                if (citation.page != page || !citation.block) continue;
                const auto* block = app_->model.findBlock(*citation.block);
                if (!block) continue;
                const QRectF bounds(block->bounds.x, block->bounds.y,
                                    block->bounds.width, block->bounds.height);
                if (bounds.contains(point)) {
                    suppressNextClickClear_ = true;
                    emit objectClicked(reader::anchorForCitation(app_->model, citation),
                                       "citation");
                    return;
                }
            }
        });
        connect(w, &PageWidget::dragSelected, this, [this](int page, QRectF rect, QString text) {
            if (text.trimmed().isEmpty()) {
                if (suppressNextClickClear_) {
                    suppressNextClickClear_ = false;
                    return;
                }
                // Plain click: clear AI context, keep reading.
                app_->context.clearCurrentSelection();
                selection_.reset();
                emit selectionChanged(reader::DocumentAnchor{});
                return;
            }
            reader::PdfSelection::Drag drag{page,
                                            {float(rect.x()), float(rect.y()),
                                             float(rect.width()), float(rect.height())}};
            reader::DocumentAnchor a =
                reader::PdfSelection::resolve(app_->model, drag, text.toStdString());
            reader::ContextReference reference;
            reference.type = reader::ReferenceType::TextSelection;
            reference.anchor = a;
            reference.displayName = "Selected paragraph · p." + std::to_string(page + 1);
            reference.extractedText = a.anchorText.substr(0, 500);
            app_->context.setCurrentSelection(std::move(reference));
            selection_ = a;
            emit selectionChanged(a);
            // Focus stays in the composer (§18): menu acts without focus.
            showSelectionMenu(a);
        });
        layout->addWidget(w);
    }
    distributeLines();
    refreshAiOverlays();
}

void PdfView::attachDocument(std::shared_ptr<QPdfDocument> doc, const QString& path) {
    docGen_->fetch_add(1);
    semanticToken_.cancel();
    semanticToken_ = reader::CancellationToken{};
    {
        std::lock_guard<std::mutex> lock(semanticState_->mutex);
        semanticState_->index = reader::VectorIndex{};
    }
    doc_ = std::move(doc);
    selIndex_->clear(); // stale geometry dies with the doc
    {
        std::lock_guard<std::mutex> lock(wordPendingMutex_);
        wordPending_.clear(); // queued jobs notice the generation bump
    }
    // Snapshot geometry once, up front: every later QPdfDocument query
    // serializes on the engine's internal mutex, which the background word
    // indexer can hold for ~1s per dense page. The scroll path below must
    // never touch doc_ again (backtrace-proven freeze).
    pageCount_ = doc_ ? doc_->pageCount() : 0;
    pageSizes_.assign(static_cast<std::size_t>(std::max(0, pageCount_)), QSizeF());
    for (int i = 0; i < pageCount_; ++i) pageSizes_[i] = doc_->pagePointSize(i);
    raster_ = std::make_shared<PopplerBridge>();
    if (!raster_->open(path)) raster_.reset();
    if (renderer_) {
        if (raster_) renderer_->attachRaster(raster_, path.toStdString());
        else renderer_->detach();
    }
    linkModel_ = std::make_unique<QPdfLinkModel>();
    linkModel_->setDocument(doc_.get());
    currentPage_ = 0;
    rebuildPages();
    prefetchPixelsAround(0);
}

void PdfView::goToPage(int page) {
    if (page < 0 || page >= pageCount_) return;
    currentPage_ = page;
    if (pageMode_) rebuildPages();
    prefetchPixelsAround(page);
    if (QWidget* w = pageWidget(page)) {
        ensureWidgetVisible(w);
        emit pageChanged(page);
        emit viewStateChanged(currentPage_, verticalScrollBar()->value(), zoom_, rotation_);
    }
}

void PdfView::jumpToAnchor(const reader::DocumentAnchor& anchor, bool highlight) {
    goToPage(anchor.page);
    if (highlight) {
        pendingHighlight_ = anchor;
        hasHighlight_ = true;
        if (auto* w = qobject_cast<PageWidget*>(pageWidget(anchor.page))) {
                const auto& b = anchor.bounds;
                w->setHighlight(QRectF(b.x, b.y, b.width, b.height));
        }
    }
}

void PdfView::setHoverAnchor(const reader::DocumentAnchor& anchor) {
    clearHoverAnchor();
    hoverAnchor_ = anchor;
    if (auto* w = qobject_cast<PageWidget*>(pageWidget(anchor.page))) {
        const auto& b = anchor.bounds;
        w->setHoverHighlight(QRectF(b.x, b.y, b.width, b.height));
    }
}

void PdfView::clearHoverAnchor() {
    hoverAnchor_.reset();
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i)
        if (auto* w = qobject_cast<PageWidget*>(layout->itemAt(i)->widget()))
            w->clearHoverHighlight();
}

void PdfView::setZoom(double z) {
    if (!applyingFit_) fitMode_ = FitMode::None;
    zoom_ = std::clamp(z, 0.25, 8.0);
    app_->state.zoom = zoom_;
    rebuildPages();
    refreshAiOverlays();
    emit zoomChanged(zoom_);
    emit viewStateChanged(currentPage_, verticalScrollBar()->value(), zoom_, rotation_);
}

void PdfView::fitWidth() {
    fitMode_ = FitMode::Width;
    applyFitMode();
}

void PdfView::fitPage() {
    fitMode_ = FitMode::Page;
    applyFitMode();
}

void PdfView::applyFitMode() {
    if (pageCount_ == 0) return;
    QSizeF pts = pageSizes_[0];
    if (rotation_ == 90 || rotation_ == 270) pts.transpose();
    const double availableWidth = std::max(1, viewport()->width() - 28);
    const double availableHeight = std::max(1, viewport()->height() - 28);
    double fitted = zoom_;
    if (fitMode_ == FitMode::Width)
        fitted = availableWidth / pts.width();
    else if (fitMode_ == FitMode::Page)
        fitted = std::min(availableWidth / pts.width(), availableHeight / pts.height());
    else
        return;
    applyingFit_ = true;
    setZoom(fitted);
    applyingFit_ = false;
}

void PdfView::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    if (fitMode_ != FitMode::None)
        QTimer::singleShot(0, this, [this] { applyFitMode(); });
}

void PdfView::setRotation(int degrees) {
    int normalized = ((degrees % 360) + 360) % 360;
    normalized = (normalized / 90) * 90;
    if (rotation_ == normalized) return;
    rotation_ = normalized;
    if (fitMode_ != FitMode::None)
        applyFitMode();
    else {
        rebuildPages();
        refreshAiOverlays();
    }
    emit viewStateChanged(currentPage_, verticalScrollBar()->value(), zoom_, rotation_);
}

void PdfView::rotate(int quarterTurns) {
    setRotation(rotation_ + quarterTurns * 90);
}

void PdfView::setPageMode(bool enabled) {
    if (pageMode_ == enabled) return;
    pageMode_ = enabled;
    rebuildPages();
    emit viewStateChanged(currentPage_, verticalScrollBar()->value(), zoom_, rotation_);
}

reader::PdfViewState PdfView::captureState() const {
    reader::PdfViewState state;
    state.page = currentPage_;
    state.scrollY = verticalScrollBar()->value();
    state.zoom = zoom_;
    state.rotation = rotation_;
    state.pageMode = pageMode_;
    state.selection = selection_;
    return state;
}

void PdfView::restoreState(const reader::PdfViewState& state, bool highlight) {
    pageMode_ = state.pageMode;
    rotation_ = ((state.rotation % 360) + 360) % 360;
    zoom_ = std::clamp(state.zoom, 0.25, 8.0);
    currentPage_ = state.page;
    selection_ = state.selection;
    rebuildPages();
    goToPage(state.page);
    verticalScrollBar()->setValue(std::max(0, state.scrollY));
    if (highlight && state.selection) jumpToAnchor(*state.selection, true);
    emit viewStateChanged(currentPage_, verticalScrollBar()->value(), zoom_, rotation_);
}

std::vector<reader::TextIndex::Hit> PdfView::searchLiteral(const std::string& query,
                                                            std::size_t limit) {
    textIndex_.build(app_->model);
    return textIndex_.search(app_->model, query, limit);
}

std::vector<reader::VectorIndex::Hit> PdfView::searchSemantic(const std::string& query,
                                                              std::size_t limit) {
    std::lock_guard<std::mutex> lock(semanticState_->mutex);
    if (!semanticState_->provider || !semanticState_->index.semanticReady()) return {};
    return semanticState_->index.querySemantic(app_->model, query, *semanticState_->provider,
                                               limit, semanticToken_);
}

std::vector<reader::PdfOutlineEntry> PdfView::outlineEntries() const {
    std::vector<reader::PdfOutlineEntry> entries;
    if (!doc_) return entries;
    QPdfBookmarkModel model;
    model.setDocument(doc_.get());
    std::function<void(const QModelIndex&, int)> visit = [&](const QModelIndex& parent,
                                                              int level) {
        for (int row = 0; row < model.rowCount(parent); ++row) {
            const QModelIndex index = model.index(row, 0, parent);
            reader::PdfOutlineEntry entry;
            entry.title = model.data(index, int(QPdfBookmarkModel::Role::Title))
                              .toString()
                              .toStdString();
            entry.page = model.data(index, int(QPdfBookmarkModel::Role::Page)).toInt();
            entry.level = level;
            entries.push_back(std::move(entry));
            visit(index, level + 1);
        }
    };
    visit({}, 0);
    return entries;
}

QWidget* PdfView::pageWidget(int page) const {
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i) {
        auto* widget = qobject_cast<PageWidget*>(layout->itemAt(i)->widget());
        if (widget && widget->pageIndex() == page) return widget;
    }
    return nullptr;
}

QPdfLink PdfView::linkAt(int page, const QPointF& point) const {
    if (!doc_ || page < 0 || !linkModel_) return {};
    linkModel_->setPage(page);
    return linkModel_->linkAt(point);
}

void PdfView::keyPressEvent(QKeyEvent* event) {
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (mods.testFlag(Qt::ControlModifier) && event->key() == Qt::Key_F) {
        emit findRequested();
        event->accept();
        return;
    }
    if (mods.testFlag(Qt::ControlModifier) && event->key() == Qt::Key_K) {
        emit commandPaletteRequested();
        event->accept();
        return;
    }
    if (mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::ShiftModifier) &&
        event->key() == Qt::Key_A) {
        emit sidecarToggleRequested();
        event->accept();
        return;
    }
    if (mods.testFlag(Qt::ControlModifier) && event->key() >= Qt::Key_1 &&
        event->key() <= Qt::Key_3) {
        emit tabRequested(event->key() - Qt::Key_1);
        event->accept();
        return;
    }
    if (mods.testFlag(Qt::AltModifier) && event->key() == Qt::Key_Left) {
        emit historyBackRequested();
        event->accept();
        return;
    }
    if (mods.testFlag(Qt::AltModifier) && event->key() == Qt::Key_Right) {
        emit historyForwardRequested();
        event->accept();
        return;
    }
    if (mods == Qt::NoModifier) {
        if (event->key() == Qt::Key_J) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() +
                                          verticalScrollBar()->singleStep());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_K) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() -
                                          verticalScrollBar()->singleStep());
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_G) {
            if (pendingG_) {
                verticalScrollBar()->setValue(0);
                pendingG_ = false;
            } else if (event->text() == "G") {
                verticalScrollBar()->setValue(verticalScrollBar()->maximum());
            } else {
                pendingG_ = true;
            }
            event->accept();
            return;
        }
    }
    pendingG_ = false;
    QScrollArea::keyPressEvent(event);
}

void PdfView::updateCurrentPage() {
    if (!doc_ || pageHost_->layout()->count() == 0) return;
    QPoint center = viewport()->rect().center();
    QPoint inHost = pageHost_->mapFrom(viewport(), center);
    int bestPage = currentPage_;
    int bestDistance = std::numeric_limits<int>::max();
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i) {
        auto* widget = qobject_cast<PageWidget*>(layout->itemAt(i)->widget());
        if (!widget) continue;
        QRect geometry = widget->geometry();
        int distance = geometry.contains(inHost)
                           ? 0
                           : std::min(std::abs(inHost.y() - geometry.top()),
                                      std::abs(inHost.y() - geometry.bottom()));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestPage = widget->pageIndex();
        }
    }
    bool changed = bestPage != currentPage_;
    currentPage_ = bestPage;
    if (app_) {
        app_->state.page = currentPage_;
        app_->state.scrollY = verticalScrollBar()->value();
    }
    if (changed) {
        emit pageChanged(currentPage_);
        schedulePixelPrefetch(currentPage_);
    }
    emit viewStateChanged(currentPage_, verticalScrollBar()->value(), zoom_, rotation_);
}

void PdfView::refreshUserOverlays() {
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i)
        if (auto* w = qobject_cast<PageWidget*>(layout->itemAt(i)->widget()))
            w->setUserOverlays({});
    if (!app_->annotations || app_->model.document.id.empty()) return;
    std::unordered_map<int, QList<QPair<QRectF, QString>>> perPage;
    for (const auto& ann : app_->annotations->annotationsFor(app_->model.document.id)) {
        const auto& b = ann.anchor.bounds;
        if (b.valid())
            perPage[ann.anchor.page].push_back(
                {QRectF(b.x, b.y, b.width, b.height), QString::fromStdString(ann.kind)});
    }
    for (const auto& note : app_->annotations->notesFor(app_->model.document.id)) {
        const auto& b = note.anchor.bounds;
        if (b.valid())
            perPage[note.anchor.page].push_back(
                {QRectF(b.x, b.y, b.width, b.height), QStringLiteral("note")});
    }
    for (const auto& [page, rects] : perPage) {
        if (auto* w = qobject_cast<PageWidget*>(pageWidget(page))) w->setUserOverlays(rects);
    }
}

// Floating selection menu (§38): Highlight (H) | Ask a question (A) |
// Write a note (N). Single keys act while the menu is open; any other
// typed character dismisses the menu and is forwarded to the ask box via
// quickAskRequested so highlighting never blocks typing.
// Persistence is delegated to the existing AnnotationRepository.
// Non-modal popup: never blocks the reader (or headless tests).
void PdfView::showSelectionMenu(const reader::DocumentAnchor& anchor) {
    auto* menu = new QMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction* highlight = menu->addAction("Highlight (H)");
    QAction* ask = menu->addAction("Ask a question (A)");
    QAction* note = menu->addAction("Write a note (N)");
    const reader::DocumentId doc = app_->model.document.id;
    connect(highlight, &QAction::triggered, this, [this, anchor, doc] {
        reader::UserAnnotation ann;
        ann.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        ann.anchor = anchor;
        ann.kind = "highlight";
        ann.color = "#ffe066";
        if (app_->annotations && app_->annotations->saveAnnotation(doc, ann))
            refreshUserOverlays();
    });
    connect(ask, &QAction::triggered, this, [this] { emit quickAskRequested(QString()); });
    connect(note, &QAction::triggered, this, [this, anchor, doc] {
        bool ok = false;
        QString text = QInputDialog::getMultiLineText(nullptr, "Note",
                                                      "Note on selected passage:", QString(), &ok);
        if (ok && !text.trimmed().isEmpty() && app_->annotations) {
            reader::Note n;
            n.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            n.anchor = anchor;
            n.text = text.toStdString();
            n.createdAt = n.updatedAt = reader::nowMs();
            if (!app_->annotations->saveNote(doc, n))
                QToolTip::showText(QCursor::pos(), "Could not save note");
        }
    });
    menu->installEventFilter(this);
    QWidget* focusBeforeMenu = QApplication::focusWidget();
    menu->setFocusPolicy(Qt::NoFocus);
    menu->setAttribute(Qt::WA_ShowWithoutActivating);
    menu->popup(QCursor::pos());
    if (focusBeforeMenu)
        QTimer::singleShot(0, focusBeforeMenu, [focusBeforeMenu] {
            // Never pull focus out of a text input: the ask box owns typing.
            if (QWidget* now = QApplication::focusWidget();
                qobject_cast<QLineEdit*>(now) || qobject_cast<QTextEdit*>(now) ||
                qobject_cast<QPlainTextEdit*>(now))
                return;
            focusBeforeMenu->setFocus();
        });
}

bool PdfView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* menu = qobject_cast<QMenu*>(watched);
        auto* key = static_cast<QKeyEvent*>(event);
        if (menu && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                          Qt::MetaModifier))) {
            if (key->key() == Qt::Key_H || key->key() == Qt::Key_A ||
                key->key() == Qt::Key_N) {
                for (QAction* action : menu->actions()) {
                    const QString text = action->text();
                    if ((key->key() == Qt::Key_H && text.startsWith("Highlight")) ||
                        (key->key() == Qt::Key_A && text.startsWith("Ask")) ||
                        (key->key() == Qt::Key_N && text.startsWith("Write"))) {
                        action->trigger();
                        return true;
                    }
                }
            } else if (!key->text().isEmpty()) {
                menu->close();
                emit quickAskRequested(key->text());
                return true;
            }
        }
    }
    return QScrollArea::eventFilter(watched, event);
}
    });
    connect(bookmark, &QAction::triggered, this, [this, anchor] {
        emit bookmarkRequested(anchor);
    });
    connect(region, &QAction::triggered, this, [this, anchor] {
        // Capture from the PDF raster source on the renderer lane. A widget
        // screenshot can contain overlays, stale tiles, or a scaled preview.
        requestAnchorImage(anchor, [this, anchor](const QImage& image) {
            if (!image.isNull()) emit regionCaptured(anchor, image);
        });
    });
    connect(ask, &QAction::triggered, this, [this] { emit askAiRequested(); });
    connect(copy, &QAction::triggered, this, [anchor] {
        QGuiApplication::clipboard()->setText(QString::fromStdString(anchor.anchorText));
    });
    QWidget* focusBeforeMenu = QApplication::focusWidget();
    menu->setFocusPolicy(Qt::NoFocus);
    menu->setAttribute(Qt::WA_ShowWithoutActivating);
    menu->popup(QCursor::pos());
    if (focusBeforeMenu)
        QTimer::singleShot(0, focusBeforeMenu, [focusBeforeMenu] { focusBeforeMenu->setFocus(); });
}

void PdfView::refreshAiOverlays() {
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i)
        if (auto* w = qobject_cast<PageWidget*>(layout->itemAt(i)->widget()))
            w->setAiLines({}, {});
    if (!app_->analysis) return;
    // Group cached line fragments per page once (no engine calls here).
    std::unordered_map<int, std::vector<const reader::TextSpan*>> linesByPage;
    for (const auto& span : app_->model.lineSpans) linesByPage[span.page].push_back(&span);
    // Honor the per-type emphasis toggles (§5.9); AI stays subtle and off
    // for methods by default.
    const auto& st = app_->state.settings;
    std::unordered_map<int, QList<QPair<QPointF, QPointF>>> linesOut;
    std::unordered_map<int, QList<QRectF>> fallbackOut;
    for (const auto& an : app_->analysis->annotations) {
        bool show = st.showImportant;
        if (an.type == "definition") show = st.showDefinitions;
        else if (an.type == "result") show = st.showResults;
        else if (an.type == "limitation") show = st.showLimitations;
        else if (an.type == "method") show = st.showMethods;
        if (!show) continue;
        const reader::TextBlock* b = app_->model.findBlock(an.blockId);
        if (!b || !b->bounds.valid()) continue;
        const auto& bb = b->bounds;
        QRectF block(bb.x, bb.y, bb.width, bb.height);
        bool lined = false;
        auto it = linesByPage.find(b->page);
        if (it != linesByPage.end()) {
            for (const reader::TextSpan* span : it->second) {
                const auto& sb = span->bounds;
                QRectF frag(sb.x, sb.y, sb.width, sb.height);
                QRectF hit = frag.intersected(block);
                if (hit.width() < 4 || hit.height() < 1) continue;
                linesOut[b->page].push_back(
                    {QPointF(hit.left(), hit.bottom() + 1.0), QPointF(hit.right(), hit.bottom() + 1.0)});
                lined = true;
            }
        }
        if (!lined) fallbackOut[b->page].push_back(block);
    }
    for (const auto& [page, lines] : linesOut) {
        QList<QRectF> fb;
        auto fit = fallbackOut.find(page);
        if (fit != fallbackOut.end()) fb = fit->second;
        if (auto* w = qobject_cast<PageWidget*>(pageWidget(page))) w->setAiLines(lines, fb);
    }
    for (const auto& [page, rects] : fallbackOut) {
        if (linesOut.count(page)) continue;
        if (auto* w = qobject_cast<PageWidget*>(pageWidget(page)))
            w->setAiLines({}, rects);
    }
}

void PdfView::schedulePrefetchAround(int page) {
    if (page < 0 || page >= pageCount_ || !prefetchTimer_) return;
    latestPrefetchPage_.store(page);
    pendingPrefetchPage_ = page;
    prefetchTimer_->start();
}

void PdfView::schedulePixelPrefetch(int page) {
    schedulePrefetchAround(page);
}

void PdfView::prefetchAround(int page) {
    if (page < 0 || page >= pageCount_) return;
    const auto generation = docGen_;
    const unsigned long gen = generation->load();
    std::shared_ptr<QPdfDocument> doc = doc_;
    const auto index = selIndex_;
    QPointer<PdfView> view(this);
    for (int p = page - 2; p <= page + 2; ++p) {
        if (p < 0 || p >= pageCount_ || index->ready(p)) continue;
        {
            std::lock_guard<std::mutex> lock(wordPendingMutex_);
            if (!wordPending_.insert(p).second) continue; // already queued
        }
        // Serialized doc lane: pdfium is used from one thread at a time.
        // Each page costs hundreds of per-word engine queries (~1s), so a
        // job that waited out a fast scroll checks at start whether its
        // page is still near the reader before burning that second.
        app_->docLane.submit([doc, index, generation, view, p, gen, this] {
            const auto release = [this, p] {
                std::lock_guard<std::mutex> lock(wordPendingMutex_);
                wordPending_.erase(p);
            };
            if (gen != generation->load() || index->ready(p)) {
                release();
                return;
            }
            if (view && std::abs(p - view->latestPrefetchPage_.load()) > 3) {
                release();
                return;
            }
            QString text;
            auto boxes = reader::buildWordBoxes(*doc, p, text);
            release();
            if (gen != generation->load()) return;
            reader::PageWords pw;
            pw.pageText = std::move(text);
            pw.words = std::move(boxes);
            pw.ready = true;
            index->store(p, std::move(pw));
            if (auto* gui = QCoreApplication::instance())
                QMetaObject::invokeMethod(gui, [view, p] {
                    if (view) emit view->selectionGeometryReady(p);
                }, Qt::QueuedConnection);
        });
    }
}

void PdfView::prefetchPixelsAround(int page) {
    if (!renderer_ || page < 0 || page >= pageCount_) return;
    qreal dpr = devicePixelRatioF();
    if (dpr <= 0) dpr = 1;
    // Next pages first: scrolling down is the common case. Low priority
    // jobs that only warm the full-page cache; the visible page's tiles
    // always win the thread pool.
    const int order[] = {page + 1, page - 1, page + 2, page - 2};
    for (int p : order) {
        if (p < 0 || p >= pageCount_ || p == page) continue;
        const QSizeF pts = pageSizes_[p];
        if (pts.isEmpty()) continue;
        renderer_->prefetchPage(p, QSize(qMax(1, qRound(pts.width() * zoom_ * dpr)),
                                         qMax(1, qRound(pts.height() * zoom_ * dpr))));
    }
}

void PdfView::distributeLines() {
    std::unordered_map<int, std::vector<reader::TextSpan>> byPage;
    for (const auto& span : app_->model.lineSpans) byPage[span.page].push_back(span);
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i)
        if (auto* w = qobject_cast<PageWidget*>(layout->itemAt(i)->widget())) {
            auto it = byPage.find(w->pageIndex());
            w->setLines(it == byPage.end() ? std::vector<reader::TextSpan>{}
                                           : std::move(it->second));
        }
}

#include "PdfView.moc"
