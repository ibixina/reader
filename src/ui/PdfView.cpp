#include "ui/PdfView.h"
#include "app/Application.h"
#include "document/DocumentModel.h"
#include "pdf/PdfSelection.h"
#include <QCoreApplication>
#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMetaObject>
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
#include <QPushButton>
#include <QScreen>
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
    void setHighlights(QList<QRectF> rows) {
        highlightRows_ = std::move(rows);
        update();
    }
    // Paint truth for the live selection: one rect per visual row, so a
    // multiline highlight never covers unselected text.
    QList<QRectF> selectionRows() const {
        QList<QRectF> rows;
        for (const QPolygonF& poly : precise_) rows.push_back(poly.boundingRect());
        return rows;
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
        // Continuous per-row highlight: what you see is exactly the text
        // that became AI context, with no gaps between words.
        if (!precise_.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(60, 120, 255, 80));
            for (const QPolygonF& poly : precise_) {
                QPolygonF mapped;
                for (const QPointF& pt : poly) mapped << mapPoint(pt);
                p.drawPolygon(mapped);
            }
        }
        if (!highlightRows_.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 200, 60, 110));
            for (const QRectF& r : highlightRows_) p.fillRect(mapRect(r), p.brush());
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
            anchorChar_ = anchorIsWord_ ? charOffset(snap_.words[anchorWord_], pt.x()) : 0;
        } else {
            anchorWord_ = -1;
            anchorChar_ = 0;
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
        return reader::snapWordIndex(words, pt);
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
    // Character offset of a point inside a word, proportional by count.
    // No per-glyph metrics exist off the engine thread; the fraction still
    // updates smoothly per mouse move, which is what makes selection feel
    // character-wise instead of word-wise.
    static int charOffset(const reader::WordBox& w, double x) {
        if (w.text.isEmpty() || w.rect.width() <= 0) return 0;
        const double frac = (x - w.rect.left()) / w.rect.width();
        return qBound(0, int(std::round(frac * double(w.text.size()))), w.text.size());
    }
    static double charEdgeX(const reader::WordBox& w, int chars) {
        if (w.text.isEmpty() || w.rect.width() <= 0) return w.rect.left();
        const int c = qBound(0, chars, w.text.size());
        return w.rect.left() + w.rect.width() * double(c) / double(w.text.size());
    }
    void updateLive(const QPoint& widgetPos) {
        QPointF pt = toPdf(QPointF(widgetPos));
        if (snap_.ready && anchorIsWord_) {
            const auto& words = snap_.words;
            int cur = wordAt(words, pt);
            int curChar = 0;
            if (cur < 0 && !words.empty()) {
                // Dragged past the text: clamp to the nearest document end
                // instead of dropping the whole selection.
                const QRectF& first = words.front().rect;
                if (pt.y() < first.center().y() ||
                    (pt.y() <= first.bottom() && pt.x() < first.left())) {
                    cur = 0;
                    curChar = 0;
                } else {
                    cur = (int)words.size() - 1;
                    curChar = words.back().text.size();
                }
            } else if (cur >= 0) {
                curChar = charOffset(words[cur], pt.x());
            }
            if (anchorWord_ < 0 || cur < 0) {
                precise_.clear();
                liveText_.clear();
                update();
                return;
            }
            // Order the two character-precise endpoints: updates land
            // per character, not per word.
            int aW = anchorWord_, aC = anchorChar_, bW = cur, bC = curChar;
            if (bW < aW || (bW == aW && bC < aC)) {
                std::swap(aW, bW);
                std::swap(aC, bC);
            }
            if (aW == bW && aC == bC) {
                precise_.clear();
                liveText_.clear();
                update();
                return;
            }
            const auto& wa = words[aW];
            const auto& wb = words[bW];
            const int from = wa.startIndex + qMin(aC, wa.text.size());
            const int to = wb.startIndex + qMin(bC, wb.text.size());
            liveText_ = snap_.pageText.mid(from, qMax(0, to - from));
            // One continuous rect per visual row: inter-word spaces stay
            // painted, so the highlight has no gaps.
            precise_.clear();
            double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
            int k = aW;
            while (k <= bW) {
                double rowTop = words[k].rect.top(), rowBot = words[k].rect.bottom();
                int j = k;
                while (j + 1 <= bW) {
                    const QRectF& n = words[j + 1].rect;
                    if (n.top() >= rowBot || n.bottom() <= rowTop) break;
                    ++j;
                    rowTop = qMin(rowTop, n.top());
                    rowBot = qMax(rowBot, n.bottom());
                }
                const double left =
                    (k == aW) ? charEdgeX(words[k], aC) : words[k].rect.left();
                const double right =
                    (j == bW) ? charEdgeX(words[j], bC) : words[j].rect.right();
                if (right > left) {
                    precise_.push_back(QPolygonF(QRectF(left, rowTop, right - left,
                                                         rowBot - rowTop)));
                    x0 = qMin(x0, left);
                    y0 = qMin(y0, rowTop);
                    x1 = qMax(x1, right);
                    y1 = qMax(y1, rowBot);
                }
                k = j + 1;
            }
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
    int anchorChar_ = 0;
    int anchorLine_ = -1;
    bool anchorIsWord_ = false;
    std::vector<reader::TextSpan> lines_;
    QList<QPolygonF> precise_;
    QString liveText_;
    QRectF liveRect_;
    QList<QRectF> highlightRows_;
    QRectF hoverHighlight_;
    QList<QPair<QRectF, QString>> userOverlays_;
    QImage cache_;
    QImage preview_;
    QSize previewFor_;
    QRect requestedSource_;
    bool renderPending_ = false;
    bool previewPending_ = false;
};

// Forward: geometric highlight identity, defined with the highlight actions.
static bool sameHighlightTarget(const reader::DocumentAnchor& a,
                                const reader::DocumentAnchor& b);

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
        selectionRows_.clear();
    });
}

PdfView::~PdfView() = default;

int PdfView::pageCount() const {
    return pageCount_;
}

std::vector<QSizeF> PdfView::pageSizes() const {
    return pageSizes_;
}

void PdfView::clearAllSelections() {
    QLayout* layout = pageHost_->layout();
    for (int i = 0; i < layout->count(); ++i)
        if (auto* w = qobject_cast<PageWidget*>(layout->itemAt(i)->widget())) w->clearSelection();
}

bool PdfView::isSelectionTooLarge(const QString& text) {
    return text.trimmed().size() > kMaxSelectionChars;
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
                selectionRows_.clear();
                emit selectionChanged(reader::DocumentAnchor{});
                return;
            }
            if (isSelectionTooLarge(text)) {
                // An accidental page-size drag (a whole algorithm plus two
                // sections) must not become context or a wash: drop the
                // drag, keep the previous selection, and say why.
                clearAllSelections();
                QToolTip::showText(
                    QCursor::pos(),
                    QString("Selection too large (%1 chars max) — drag a smaller passage.")
                        .arg(kMaxSelectionChars));
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
            selectionRows_.clear();
            if (QWidget* w = pageWidget(page))
                if (auto* pw = qobject_cast<PageWidget*>(w))
                    selectionRows_ = pw->selectionRows();
            selection_ = a;
            emit selectionChanged(a);
            // No popup: the selection only feeds AI context (§12). h/n/a
            // act on it from the window shortcuts; typing never leaves the
            // focused widget.
        });
        layout->addWidget(w);
    }
    distributeLines();
}

void PdfView::attachDocument(std::shared_ptr<QPdfDocument> doc, const QString& path) {
    docGen_->fetch_add(1);
    doc_ = std::move(doc);
    selection_.reset(); // stale selection (and its rows) dies with the doc
    selectionRows_.clear();
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
            // Restoring the live selection reuses its exact rows; every
            // other anchor (equation, figure, chat source) is one rect.
            QList<QRectF> rows;
            if (selection_ && selection_->page == anchor.page && !selectionRows_.isEmpty() &&
                sameHighlightTarget(*selection_, anchor))
                rows = selectionRows_;
            else if (anchor.bounds.valid()) {
                const auto& b = anchor.bounds;
                rows = {QRectF(b.x, b.y, b.width, b.height)};
            }
            w->setHighlights(rows);
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
    selectionRows_.clear(); // exact rows are drag-time paint truth; re-drag to renew
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
    if (mods.testFlag(Qt::ControlModifier) && mods.testFlag(Qt::ShiftModifier) &&
        event->key() == Qt::Key_A) {
        emit sidecarToggleRequested();
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

// A highlight is identified by substantial geometric overlap on one
// page — never by exact text, pixel bounds or volatile block IDs. A
// sub-selection of a highlight (or a re-drag after zoom/re-extraction)
// still matches, while nearby distinct passages never do.
// Degenerate anchors without geometry fall back to page + text.
static double highlightOverlap(const reader::Rect& a, const reader::Rect& b) {
    if (!a.valid() || !b.valid()) return 0.0;
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.x + a.width, b.x + b.width);
    const float y1 = std::min(a.y + a.height, b.y + b.height);
    const float w = x1 - x0;
    const float h = y1 - y0;
    if (w <= 0 || h <= 0) return 0.0;
    const double inter = double(w) * double(h);
    const double minArea =
        std::min(double(a.width) * double(a.height), double(b.width) * double(b.height));
    return minArea > 0 ? inter / minArea : 0.0;
}

static bool sameHighlightTarget(const reader::DocumentAnchor& a,
                                const reader::DocumentAnchor& b) {
    if (a.page != b.page) return false;
    if (!a.bounds.valid() || !b.bounds.valid())
        return a.anchorText == b.anchorText;
    return highlightOverlap(a.bounds, b.bounds) >= 0.5;
}

bool PdfView::hasHighlightForCurrentSelection() const {
    if (!hasLiveSelection() || !app_ || !app_->annotations) return false;
    const reader::DocumentId doc = app_->model.document.id;
    if (doc.empty()) return false;
    for (const auto& ann : app_->annotations->annotationsFor(doc))
        if (ann.kind == "highlight" && sameHighlightTarget(ann.anchor, *selection_))
            return true;
    return false;
}

bool PdfView::highlightCurrentSelection() {
    if (!hasLiveSelection() || !app_ || !app_->annotations) return false;
    const reader::DocumentId doc = app_->model.document.id;
    if (doc.empty()) return false;
    // Apply exactly once: a repeat press is a no-op (removal is the
    // separate path below, which also cleans up legacy stacks).
    for (const auto& ann : app_->annotations->annotationsFor(doc))
        if (ann.kind == "highlight" && sameHighlightTarget(ann.anchor, *selection_))
            return false;
    // One annotation per visual row: a multiline highlight hugs the
    // selected rows instead of painting the whole bounding box.
    QList<QRectF> rows = selectionRows_;
    if (rows.isEmpty() && selection_->bounds.valid()) {
        const auto& b = selection_->bounds;
        rows = {QRectF(b.x, b.y, b.width, b.height)};
    }
    if (rows.isEmpty()) return false;
    for (const QRectF& row : rows) {
        reader::UserAnnotation ann;
        ann.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        ann.anchor = *selection_;
        ann.anchor.bounds = {float(row.x()), float(row.y()), float(row.width()),
                             float(row.height())};
        ann.kind = "highlight";
        ann.color = "#ffe066";
        if (!app_->annotations->saveAnnotation(doc, ann)) return false;
    }
    refreshUserOverlays();
    return true;
}

bool PdfView::removeHighlightForCurrentSelection() {
    if (!hasLiveSelection() || !app_ || !app_->annotations) return false;
    const reader::DocumentId doc = app_->model.document.id;
    if (doc.empty()) return false;
    bool removed = false;
    for (const auto& ann : app_->annotations->annotationsFor(doc))
        if (ann.kind == "highlight" && sameHighlightTarget(ann.anchor, *selection_)) {
            app_->annotations->deleteAnnotation(doc, ann.id);
            removed = true;
        }
    if (removed) refreshUserOverlays();
    return removed;
}

void PdfView::promptNoteForAnchor(const reader::DocumentAnchor& anchor) {
    if (anchor.anchorText.empty() || !app_ || !app_->annotations) return;
    const reader::DocumentId doc = app_->model.document.id;
    if (doc.empty()) return;
    // Small editor floating over the selection instead of a modal dialog:
    // click-away or Esc cancels, Save (or Ctrl+Enter) persists.
    if (noteEditor_) noteEditor_->close();
    auto* dialog = new QDialog(window(), Qt::Popup);
    dialog->setObjectName("noteEditor");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Note");
    auto* layout = new QVBoxLayout(dialog);
    auto* edit = new QTextEdit(dialog);
    edit->setObjectName("noteText");
    edit->setPlaceholderText("Note on selected passage…");
    edit->setMinimumSize(280, 90);
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(edit);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, edit, anchor, doc] {
        const QString text = edit->toPlainText();
        if (!text.trimmed().isEmpty() && app_ && app_->annotations) {
            reader::Note n;
            n.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            n.anchor = anchor;
            n.text = text.toStdString();
            n.createdAt = n.updatedAt = reader::nowMs();
            if (!app_->annotations->saveNote(doc, n))
                QToolTip::showText(QCursor::pos(), "Could not save note");
        }
        dialog->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto* saveShortcut = new QShortcut(QKeySequence("Ctrl+Return"), edit);
    connect(saveShortcut, &QShortcut::activated, buttons, [buttons] {
        if (auto* save = buttons->button(QDialogButtonBox::Save)) save->click();
    });
    dialog->resize(300, 150);
    dialog->move(noteEditorPos(anchor, dialog->size()));
    noteEditor_ = dialog;
    dialog->show();
    edit->setFocus(Qt::PopupFocusReason);
}

QPoint PdfView::noteEditorPos(const reader::DocumentAnchor& anchor, const QSize& size) const {
    QPoint anchorPos = QCursor::pos();
    if (QWidget* w = pageWidget(anchor.page)) {
        const auto& b = anchor.bounds;
        if (b.valid())
            anchorPos = w->mapToGlobal(QPoint(qRound(b.x + b.width / 2.0), qRound(b.y)));
    }
    QPoint pos(anchorPos.x() - size.width() / 2, anchorPos.y() - size.height() - 12);
    if (QScreen* screen = QGuiApplication::screenAt(anchorPos)) {
        const QRect available = screen->availableGeometry();
        pos.setX(std::clamp(pos.x(), available.left(),
                            std::max(available.left(), available.right() - size.width())));
        pos.setY(std::clamp(pos.y(), available.top(),
                            std::max(available.top(), available.bottom() - size.height())));
    }
    return pos;
}

bool PdfView::hasLiveSelection() const {
    return selection_ && !selection_->anchorText.empty();
}

bool PdfView::promptNoteForCurrentSelection() {
    if (!hasLiveSelection()) return false;
    promptNoteForAnchor(*selection_);
    return true;
}

bool PdfView::askAboutCurrentSelection() {
    if (!hasLiveSelection()) return false;
    emit quickAskRequested(QString());
    return true;
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
