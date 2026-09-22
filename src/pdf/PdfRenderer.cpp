#include "pdf/PdfRenderer.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QPainter>
#include <QPdfDocument>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace reader {

struct PdfRenderer::State {
    explicit State(std::size_t capacity) : cache(capacity), pages(16) {}

    LruCache<RenderKey, QImage, RenderKeyHash> cache;
    // Full-page renders: one Poppler pass per (page, size); tiles slice
    // from it. Figure-heavy pages cost ~1s per render, so per-tile renders
    // would serialize to many seconds per page view.
    LruCache<PageRenderKey, QImage, PageRenderKeyHash> pages;
    std::mutex pageMutex;
    std::condition_variable pageCv;
    std::unordered_set<PageRenderKey, PageRenderKeyHash> pagesInflight;
    std::shared_ptr<PopplerBridge> raster;
    DocumentId document;
    int tileSize = 512;
    std::atomic<unsigned long> generation{0};
    std::atomic<unsigned int> activeJobs{0};
    std::mutex rasterMutex;
    std::mutex attachmentMutex;
    std::mutex jobsMutex;
    std::condition_variable jobsCv;

    struct JobLease {
        explicit JobLease(std::shared_ptr<State> owner) : state(std::move(owner)) {
            state->activeJobs.fetch_add(1);
        }
        ~JobLease() {
            if (state->activeJobs.fetch_sub(1) == 1) state->jobsCv.notify_all();
        }
        std::shared_ptr<State> state;
    };
};

namespace {
template <typename Fn>
void enqueueOnGui(Fn&& fn) {
    QPointer<QCoreApplication> app(QCoreApplication::instance());
    if (!app) return;
    QMetaObject::invokeMethod(
        app.data(), [app, callback = std::forward<Fn>(fn)]() mutable {
            if (app) callback();
        }, Qt::QueuedConnection);
}
} // namespace

PdfRenderer::PdfRenderer(std::size_t tileCapacity)
    : state_(std::make_shared<State>(tileCapacity)) {}

PdfRenderer::~PdfRenderer() {
    const auto state = state_;
    detach();
    std::unique_lock<std::mutex> lock(state->jobsMutex);
    state->jobsCv.wait(lock, [&] { return state->activeJobs.load() == 0; });
}

void PdfRenderer::setTileSize(int px) {
    if (px > 0) {
        std::lock_guard<std::mutex> lock(state_->attachmentMutex);
        state_->tileSize = px;
    }
}

void PdfRenderer::attach(QPdfDocument* doc, const DocumentId& id) {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->attachmentMutex);
    state->generation.fetch_add(1);
    // The legacy raw QPdfDocument API is retained for source compatibility,
    // but it is intentionally not submitted to worker threads: callers must
    // attach the shared Poppler raster backend used by the active reader.
    (void)doc;
    state->raster.reset();
    state->document = id;
    state->cache.clear();
    state->pages.clear();
}

void PdfRenderer::attachRaster(std::shared_ptr<PopplerBridge> raster,
                               const DocumentId& id) {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->attachmentMutex);
    state->generation.fetch_add(1);
    state->raster = std::move(raster);
    state->document = id;
    state->cache.clear();
    state->pages.clear();
}

void PdfRenderer::detach() {
    const auto state = state_;
    std::lock_guard<std::mutex> lock(state->attachmentMutex);
    state->generation.fetch_add(1);
    state->raster.reset();
    state->document.clear();
    state->cache.clear();
    state->pages.clear();
}

void PdfRenderer::requestTile(const RenderKey& requestedKey, double dpi, TileCallback cb) {
    const auto state = state_;
    RenderKey key = requestedKey;
    key.kind = 0;
    std::shared_ptr<PopplerBridge> raster;
    unsigned long generation = 0;
    int tile = 0;
    {
        std::lock_guard<std::mutex> lock(state->attachmentMutex);
        key.document = state->document;
        raster = state->raster;
        generation = state->generation.load();
        tile = state->tileSize;
    }
    if (auto hit = state->cache.get(key)) {
        enqueueOnGui([state, generation, cb = std::move(cb), key, image = *hit]() mutable {
            if (generation == state->generation.load() && cb) cb(key, image);
        });
        return;
    }
    if (!raster) {
        enqueueOnGui([state, generation, cb = std::move(cb), key]() mutable {
            if (generation == state->generation.load() && cb) cb(key, {});
        });
        return;
    }

    auto lease = std::make_shared<State::JobLease>(state);
    QThreadPool::globalInstance()->start([state, lease, raster, generation, key, dpi,
                                          tile, cb = std::move(cb)]() mutable {
        if (generation != state->generation.load()) return;
        QImage page;
        if (raster) {
            const QSizeF points = raster->pageSize(key.page);
            if (points.isEmpty()) return;
            const double scale = dpi / 72.0;
            const QSize target(qMax(1, qRound(points.width() * scale)),
                               qMax(1, qRound(points.height() * scale)));
            std::lock_guard<std::mutex> lock(state->rasterMutex);
            page = raster->renderPage(key.page, target);
        }
        if (page.isNull() || generation != state->generation.load()) return;
        const int x = key.tileX * tile;
        const int y = key.tileY * tile;
        if (x < 0 || y < 0 || x >= page.width() || y >= page.height()) return;
        QImage cropped = page.copy(x, y, std::min(tile, page.width() - x),
                                   std::min(tile, page.height() - y));
        if (cropped.isNull() || generation != state->generation.load()) return;
        state->cache.put(key, cropped);
        enqueueOnGui([state, generation, cb = std::move(cb), key,
                      image = std::move(cropped)]() mutable {
            if (generation == state->generation.load() && cb) cb(key, image);
        });
    });
}

void PdfRenderer::requestPage(int page, int zoomBucket, const QSize& size, int rotation,
                              PageCallback cb) {
    const auto state = state_;
    if (page < 0 || size.isEmpty()) {
        enqueueOnGui([cb = std::move(cb)]() mutable {
            if (cb) cb({});
        });
        return;
    }
    RenderKey key;
    std::shared_ptr<PopplerBridge> raster;
    unsigned long generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->attachmentMutex);
        key = {state->document, page, zoomBucket, 0, 0, size.width(), size.height(),
               ((rotation % 360) + 360) % 360, 1};
        raster = state->raster;
        generation = state->generation.load();
    }
    if (auto hit = state->cache.get(key)) {
        enqueueOnGui([state, generation, cb = std::move(cb), image = *hit]() mutable {
            if (generation == state->generation.load() && cb) cb(image);
        });
        return;
    }
    if (!raster) {
        enqueueOnGui([state, generation, cb = std::move(cb)]() mutable {
            if (generation == state->generation.load() && cb) cb({});
        });
        return;
    }

    auto lease = std::make_shared<State::JobLease>(state);
    // Thumbnails and single-page previews yield to interactive tiles.
    QThreadPool::globalInstance()->start([state, lease, raster, generation, key, size,
                                          cb = std::move(cb)]() mutable {
        if (generation != state->generation.load()) return;
        QImage image;
        {
            std::lock_guard<std::mutex> lock(state->rasterMutex);
            image = raster->renderPage(key.page, size);
        }
        if (generation != state->generation.load()) return;
        if (image.isNull()) {
            enqueueOnGui([state, generation, cb = std::move(cb)]() mutable {
                if (generation == state->generation.load() && cb) cb({});
            });
            return;
        }
        state->cache.put(key, image);
        enqueueOnGui([state, generation, cb = std::move(cb), image = std::move(image)]() mutable {
            if (generation == state->generation.load() && cb) cb(image);
        });
    },
    -10);
}

void PdfRenderer::requestTiles(int page, int zoomBucket, const QSize& pageSize, int rotation,
                               int tileSize, PageCallback cb, QRect visibleRect) {
    const auto state = state_;
    if (page < 0 || pageSize.isEmpty() || tileSize <= 0) {
        enqueueOnGui([cb = std::move(cb)]() mutable {
            if (cb) cb({});
        });
        return;
    }

    std::shared_ptr<PopplerBridge> raster;
    unsigned long generation = 0;
    DocumentId document;
    {
        std::lock_guard<std::mutex> lock(state->attachmentMutex);
        raster = state->raster;
        generation = state->generation.load();
        document = state->document;
    }
    if (!raster) {
        enqueueOnGui([state, generation, cb = std::move(cb)]() mutable {
            if (generation == state->generation.load() && cb) cb({});
        });
        return;
    }

    struct Batch {
        explicit Batch(QSize size, int count, PageCallback callback)
            : image(size, QImage::Format_ARGB32_Premultiplied), remaining(count),
              total(count),
              callback(std::move(callback)) {
            image.fill(Qt::white);
        }
        std::mutex mutex;
        QImage image;
        std::atomic<int> remaining;
        const int total;
        std::atomic<int> failures{0};
        PageCallback callback;
    };

    const int cols = (pageSize.width() + tileSize - 1) / tileSize;
    const int rows = (pageSize.height() + tileSize - 1) / tileSize;
    const QRect pageRect(QPoint(0, 0), pageSize);
    visibleRect = visibleRect.isValid() ? visibleRect.intersected(pageRect) : pageRect;
    if (visibleRect.isEmpty()) {
        enqueueOnGui([state, generation, cb = std::move(cb)]() mutable {
            if (generation == state->generation.load() && cb) cb({});
        });
        return;
    }
    const int firstCol = visibleRect.left() / tileSize;
    const int lastCol = visibleRect.right() / tileSize;
    const int firstRow = visibleRect.top() / tileSize;
    const int lastRow = visibleRect.bottom() / tileSize;
    auto batch = std::make_shared<Batch>(pageSize,
                                         (lastCol - firstCol + 1) * (lastRow - firstRow + 1),
                                         std::move(cb));
    const int normalizedRotation = ((rotation % 360) + 360) % 360;

    auto publish = [state, batch, generation](const QRect& rect, QImage tile) {
        if (generation != state->generation.load()) return;
        if (!tile.isNull()) {
            std::lock_guard<std::mutex> lock(batch->mutex);
            QPainter painter(&batch->image);
            painter.drawImage(rect.topLeft(), tile);
        } else {
            batch->failures.fetch_add(1);
        }
        if (batch->remaining.fetch_sub(1) == 1) {
            QImage image;
            {
                std::lock_guard<std::mutex> lock(batch->mutex);
                image = batch->failures.load() == batch->total ? QImage{} : batch->image;
            }
            enqueueOnGui([state, generation, batch, image = std::move(image)]() mutable {
                if (generation == state->generation.load() && batch->callback)
                    batch->callback(image);
            });
        }
    };

    std::vector<std::pair<RenderKey, QRect>> missingTiles;
    for (int row = firstRow; row <= lastRow && row < rows; ++row) {
        for (int col = firstCol; col <= lastCol && col < cols; ++col) {
            const QRect rect(col * tileSize, row * tileSize,
                             std::min(tileSize, pageSize.width() - col * tileSize),
                             std::min(tileSize, pageSize.height() - row * tileSize));
            const RenderKey key{document, page, zoomBucket, col, row, pageSize.width(),
                                pageSize.height(), normalizedRotation, 0};
            if (auto hit = state->cache.get(key)) {
                publish(rect, *hit);
                continue;
            }
            missingTiles.push_back({key, rect});
        }
    }
    if (missingTiles.empty()) return;

    // Small scroll steps expose 1-4 new tiles: render each crop directly
    // (~5x cheaper than a full page at 180dpi) instead of re-rasterizing
    // the whole page. Whole-page views still take one shared full render
    // below and slice from it.
    constexpr std::size_t kDirectTileLimit = 4;
    if (missingTiles.size() <= kDirectTileLimit) {
        for (const auto& [key, rect] : missingTiles) {
            auto lease = std::make_shared<State::JobLease>(state);
            QThreadPool::globalInstance()->start(
                [state, lease, raster, generation, page, pageSize, key, rect,
                 publish]() mutable {
                    if (generation != state->generation.load()) return;
                    QImage tile;
                    {
                        std::lock_guard<std::mutex> lock(state->rasterMutex);
                        tile = raster->renderTile(page, pageSize, rect);
                    }
                    if (generation != state->generation.load()) return;
                    if (!tile.isNull()) state->cache.put(key, tile);
                    publish(rect, std::move(tile));
                },
                10);
        }
        return;
    }

    // One worker job renders the full page once and slices every missing
    // tile from it. Concurrent batches for the same page/size coalesce onto
    // the in-flight render instead of each paying a full Poppler pass.
    auto lease = std::make_shared<State::JobLease>(state);
    QThreadPool::globalInstance()->start(
        [state, lease, raster, generation, document, page, pageSize,
         missingTiles = std::move(missingTiles), publish]() mutable {
            if (generation != state->generation.load()) return;
            const PageRenderKey pageKey{document, page, pageSize.width(), pageSize.height()};
            QImage full;
            {
                std::unique_lock<std::mutex> lock(state->pageMutex);
                state->pageCv.wait(lock, [&] {
                    return state->pagesInflight.count(pageKey) == 0 ||
                           generation != state->generation.load();
                });
                if (generation != state->generation.load()) return;
                if (auto hit = state->pages.get(pageKey)) {
                    full = *hit;
                } else {
                    state->pagesInflight.insert(pageKey);
                }
            }
            if (full.isNull()) {
                QImage rendered;
                {
                    std::lock_guard<std::mutex> lock(state->rasterMutex);
                    rendered = raster->renderPage(page, pageSize);
                }
                {
                    std::lock_guard<std::mutex> lock(state->pageMutex);
                    state->pagesInflight.erase(pageKey);
                    if (!rendered.isNull()) {
                        state->pages.put(pageKey, rendered);
                        full = rendered;
                    }
                    state->pageCv.notify_all();
                }
                if (generation != state->generation.load()) return;
                if (full.isNull()) {
                    for (const auto& [key, rect] : missingTiles) {
                        (void)key;
                        publish(rect, {});
                    }
                    return;
                }
            }
            for (const auto& [key, rect] : missingTiles) {
                if (generation != state->generation.load()) return;
                QImage tile = full.copy(rect);
                if (!tile.isNull()) state->cache.put(key, tile);
                publish(rect, std::move(tile));
            }
        },
        0);
}

void PdfRenderer::requestPreview(int page, const QSize& pageSize, PageCallback cb) {
    const auto state = state_;
    if (page < 0 || pageSize.isEmpty()) {
        enqueueOnGui([cb = std::move(cb)]() mutable {
            if (cb) cb({});
        });
        return;
    }
    std::shared_ptr<PopplerBridge> raster;
    unsigned long generation = 0;
    DocumentId document;
    {
        std::lock_guard<std::mutex> lock(state->attachmentMutex);
        raster = state->raster;
        generation = state->generation.load();
        document = state->document;
    }
    // Tiny but legible: longest side ~300px renders in ~20ms and upscales
    // into a soft placeholder instead of a white page.
    const int longest = std::max(pageSize.width(), pageSize.height());
    const int denom = std::max(1, longest / 300);
    const QSize small(std::max(1, pageSize.width() / denom),
                      std::max(1, pageSize.height() / denom));
    RenderKey key{document, page, pageSize.width(), 0, 0, small.width(), small.height(), 0, 2};
    if (auto hit = state->cache.get(key)) {
        enqueueOnGui([state, generation, cb = std::move(cb), image = *hit]() mutable {
            if (generation == state->generation.load() && cb) cb(image);
        });
        return;
    }
    if (!raster) {
        enqueueOnGui([cb = std::move(cb)]() mutable {
            if (cb) cb({});
        });
        return;
    }
    auto lease = std::make_shared<State::JobLease>(state);
    QThreadPool::globalInstance()->start(
        [state, lease, raster, generation, key, small, cb = std::move(cb)]() mutable {
            if (generation != state->generation.load()) return;
            QImage image;
            {
                std::lock_guard<std::mutex> lock(state->rasterMutex);
                image = raster->renderPage(key.page, small);
            }
            if (generation != state->generation.load()) return;
            if (image.isNull()) {
                enqueueOnGui([state, generation, cb = std::move(cb)]() mutable {
                    if (generation == state->generation.load() && cb) cb({});
                });
                return;
            }
            state->cache.put(key, image);
            enqueueOnGui([state, generation, cb = std::move(cb), image = std::move(image)]() mutable {
                if (generation == state->generation.load() && cb) cb(image);
            });
        },
        20);
}

void PdfRenderer::prefetchPage(int page, const QSize& pageSize) {
    const auto state = state_;
    if (page < 0 || pageSize.isEmpty()) return;
    std::shared_ptr<PopplerBridge> raster;
    unsigned long generation = 0;
    DocumentId document;
    {
        std::lock_guard<std::mutex> lock(state->attachmentMutex);
        raster = state->raster;
        generation = state->generation.load();
        document = state->document;
    }
    if (!raster) return;
    const PageRenderKey pageKey{document, page, pageSize.width(), pageSize.height()};
    {
        std::lock_guard<std::mutex> lock(state->pageMutex);
        if (state->pages.get(pageKey) || state->pagesInflight.count(pageKey) != 0) return;
        state->pagesInflight.insert(pageKey);
    }
    auto lease = std::make_shared<State::JobLease>(state);
    QThreadPool::globalInstance()->start(
        [state, lease, raster, generation, pageKey, page, pageSize]() mutable {
            if (generation != state->generation.load()) {
                std::lock_guard<std::mutex> lock(state->pageMutex);
                state->pagesInflight.erase(pageKey);
                state->pageCv.notify_all();
                return;
            }
            QImage rendered;
            {
                std::lock_guard<std::mutex> lock(state->rasterMutex);
                rendered = raster->renderPage(page, pageSize);
            }
            {
                std::lock_guard<std::mutex> lock(state->pageMutex);
                state->pagesInflight.erase(pageKey);
                if (!rendered.isNull() && generation == state->generation.load())
                    state->pages.put(pageKey, rendered);
                state->pageCv.notify_all();
            }
        },
        -5);
}

} // namespace reader
