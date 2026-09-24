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
    explicit State(std::size_t capacity) : cache(capacity), pages(10) {}

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

void PdfRenderer::requestTiles(int page, int zoomBucket, const QSize& pageSize, int rotation,
                               int tileSize, PageCallback cb, QRect visibleRect,
                               CancelToken cancelled, TileCallback tileCb) {
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

    // Tracks completion of all requested tiles for the PageCallback, and
    // optionally delivers each tile progressively via TileCallback so the
    // widget paints incrementally without blanking existing pixels.
    struct Batch {
        explicit Batch(int count, PageCallback callback, TileCallback tileCallback)
            : remaining(count), total(count), callback(std::move(callback)),
              tileCallback(std::move(tileCallback)) {}
        std::atomic<int> remaining;
        const int total;
        std::atomic<int> failures{0};
        PageCallback callback;
        TileCallback tileCallback;
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
    auto batch = std::make_shared<Batch>(
        (lastCol - firstCol + 1) * (lastRow - firstRow + 1),
        std::move(cb), std::move(tileCb));
    const int normalizedRotation = ((rotation % 360) + 360) % 360;

    auto publish = [state, batch, generation](const QRect& rect, QImage tile) {
        if (generation != state->generation.load()) return;
        if (!tile.isNull() && batch->tileCallback) {
            // Progressive delivery: paint this tile immediately. The widget
            // composites into its persistent cache without blanking.
            enqueueOnGui([state, generation, batch, rect, tile = std::move(tile)]() mutable {
                if (generation == state->generation.load() && batch->tileCallback)
                    batch->tileCallback(rect, tile);
            });
        } else if (tile.isNull()) {
            batch->failures.fetch_add(1);
        }
        if (batch->remaining.fetch_sub(1) == 1) {
            // All tiles done: fire completion. Null only if every tile failed.
            const bool allFailed = batch->failures.load() == batch->total;
            enqueueOnGui([state, generation, batch, allFailed]() mutable {
                if (generation == state->generation.load() && batch->callback)
                    batch->callback(allFailed ? QImage{} : QImage(1, 1, QImage::Format_ARGB32));
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
    // below and slice from it. Viewport tiles get high priority so the
    // visible region paints before any background prefetch.
    constexpr std::size_t kDirectTileLimit = 4;
    if (missingTiles.size() <= kDirectTileLimit) {
        for (const auto& [key, rect] : missingTiles) {
            auto lease = std::make_shared<State::JobLease>(state);
            QThreadPool::globalInstance()->start(
                [state, lease, raster, generation, page, pageSize, key, rect,
                 publish, cancelled]() mutable {
                    if (generation != state->generation.load()) return;
                    if (cancelled && cancelled->load()) return;
                    QImage tile;
                    {
                        std::lock_guard<std::mutex> lock(state->rasterMutex);
                        tile = raster->renderTile(page, pageSize, rect);
                    }
                    if (generation != state->generation.load()) return;
                    if (cancelled && cancelled->load()) return;
                    if (!tile.isNull()) state->cache.put(key, tile);
                    publish(rect, std::move(tile));
                },
                20);
        }
        return;
    }

    // One worker job renders the full page once and slices every missing
    // tile from it. Concurrent batches for the same page/size coalesce onto
    // the in-flight render instead of each paying a full Poppler pass. The
    // cancel token is checked around every step: a page the reader scrolled
    // away from must never hold the raster lane for the page they are on.
    auto lease = std::make_shared<State::JobLease>(state);
    QThreadPool::globalInstance()->start(
        [state, lease, raster, generation, document, page, pageSize,
         missingTiles = std::move(missingTiles), publish, cancelled]() mutable {
            if (generation != state->generation.load()) return;
            if (cancelled && cancelled->load()) return;
            const PageRenderKey pageKey{document, page, pageSize.width(), pageSize.height()};
            QImage full;
            {
                std::unique_lock<std::mutex> lock(state->pageMutex);
                state->pageCv.wait(lock, [&] {
                    return state->pagesInflight.count(pageKey) == 0 ||
                           generation != state->generation.load() ||
                           (cancelled && cancelled->load());
                });
                if (generation != state->generation.load()) return;
                if (cancelled && cancelled->load()) return;
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
                if (cancelled && cancelled->load()) return;
                if (full.isNull()) {
                    for (const auto& [key, rect] : missingTiles) {
                        (void)key;
                        publish(rect, {});
                    }
                    return;
                }
            }
            // Slice and publish tiles one by one: each lands on the GUI
            // thread immediately so the widget paints progressively without
            // waiting for the entire visible rect to complete.
            for (const auto& [key, rect] : missingTiles) {
                if (generation != state->generation.load()) return;
                if (cancelled && cancelled->load()) return;
                QImage tile = full.copy(rect);
                if (!tile.isNull()) state->cache.put(key, tile);
                publish(rect, std::move(tile));
            }
        },
        15);
}

void PdfRenderer::requestPreview(int page, const QSize& pageSize, PageCallback cb,
                                 CancelToken cancelled) {
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
    // Legible draft, not a thumbnail: longest side ~900px renders in tens
    // of ms and upscales to something readable while sharp tiles land —
    // the old ~300px placeholder read as "low quality" during the wait.
    const int longest = std::max(pageSize.width(), pageSize.height());
    const int denom = std::max(1, longest / 900);
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
        [state, lease, raster, generation, key, small, cb = std::move(cb),
         cancelled]() mutable {
            if (generation != state->generation.load()) return;
            if (cancelled && cancelled->load()) return;
            QImage image;
            {
                std::lock_guard<std::mutex> lock(state->rasterMutex);
                image = raster->renderPage(key.page, small);
            }
            if (generation != state->generation.load()) return;
            if (cancelled && cancelled->load()) return;
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
        10);  // Below visible sharp tiles (20), above prefetch (5/-5)
}

void PdfRenderer::prefetchPage(int page, const QSize& pageSize, int priority) {
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
        priority);
}

} // namespace reader
