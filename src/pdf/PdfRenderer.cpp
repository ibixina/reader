#include "pdf/PdfRenderer.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace reader {
namespace {
struct RenderKey {
    int page;
    QSize size;
    QRect tile; // Empty for a complete page.
    bool operator==(const RenderKey&) const = default;
};

struct RenderKeyHash {
    std::size_t operator()(const RenderKey& key) const noexcept {
        return qHashMulti(0, key.page, key.size.width(), key.size.height(),
                          key.tile.x(), key.tile.y(), key.tile.width(), key.tile.height());
    }
};

template <typename Fn>
void enqueueOnGui(Fn&& fn) {
    QPointer<QCoreApplication> app(QCoreApplication::instance());
    if (app) QMetaObject::invokeMethod(app.data(), std::forward<Fn>(fn), Qt::QueuedConnection);
}
} // namespace

struct PdfRenderer::State {
    explicit State(std::size_t capacity) : cacheBudget(capacity * 512 * 512 * 4) {
        // A shared Poppler document needs serialization. Keeping jobs queued
        // here, instead of blocking global-pool threads on a mutex, preserves
        // priorities and leaves the global pool available for other work.
        pool.setMaxThreadCount(1);
    }

    QImage cached(const RenderKey& key) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it == cache.end()) return {};
        order.splice(order.begin(), order, it->second);
        return it->second->second;
    }

    void store(const RenderKey& key, const QImage& image, unsigned long expectedGeneration) {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation.load() != expectedGeneration || image.isNull()) return;
        const auto bytes = static_cast<std::size_t>(image.sizeInBytes());
        if (bytes > cacheBudget) return;
        if (const auto it = cache.find(key); it != cache.end()) {
            cacheBytes -= static_cast<std::size_t>(it->second->second.sizeInBytes());
            order.erase(it->second);
            cache.erase(it);
        }
        while (cacheBytes + bytes > cacheBudget && !order.empty()) {
            cacheBytes -= static_cast<std::size_t>(order.back().second.sizeInBytes());
            cache.erase(order.back().first);
            order.pop_back();
        }
        order.emplace_front(key, image);
        cache.emplace(key, order.begin());
        cacheBytes += bytes;
    }

    void reset(std::shared_ptr<PopplerBridge> source) {
        std::lock_guard<std::mutex> lock(mutex);
        generation.fetch_add(1);
        pool.clear();
        raster = std::move(source);
        cache.clear();
        order.clear();
        cacheBytes = 0;
        prefetch.clear();
    }

    using CacheList = std::list<std::pair<RenderKey, QImage>>;
    std::mutex mutex;
    QThreadPool pool;
    std::shared_ptr<PopplerBridge> raster;
    std::atomic<unsigned long> generation{0};
    CacheList order;
    std::unordered_map<RenderKey, CacheList::iterator, RenderKeyHash> cache;
    const std::size_t cacheBudget;
    std::size_t cacheBytes = 0;
    std::unordered_map<RenderKey, std::shared_ptr<std::atomic<bool>>, RenderKeyHash> prefetch;
};

PdfRenderer::PdfRenderer(std::size_t tileCapacity)
    : state_(std::make_shared<State>(tileCapacity)) {}

PdfRenderer::~PdfRenderer() {
    detach();
    state_->pool.waitForDone();
}

void PdfRenderer::attach(QPdfDocument* doc, const DocumentId& id) {
    (void)doc;
    (void)id;
    state_->reset({});
}

void PdfRenderer::attachRaster(std::shared_ptr<PopplerBridge> raster, const DocumentId& id) {
    (void)id;
    state_->reset(std::move(raster));
}

void PdfRenderer::detach() {
    state_->reset({});
}

bool PdfRenderer::usesWholePage(const QSize& pageSize) {
    return qint64(pageSize.width()) * pageSize.height() <= 8 * 1024 * 1024;
}

QImage PdfRenderer::cachedPage(int page, const QSize& pageSize) const {
    return state_->cached({page, pageSize, {}});
}

void PdfRenderer::requestImage(int page, const QSize& pageSize, PageCallback cb,
                               CancelToken cancelled, int priority) {
    const auto state = state_;
    unsigned long generation;
    std::shared_ptr<PopplerBridge> raster;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        generation = state->generation.load();
        raster = state->raster;
    }
    auto obsolete = [state, generation, cancelled] {
        return generation != state->generation.load() || (cancelled && cancelled->load());
    };
    auto publish = [obsolete, cb = std::move(cb)](QImage image) {
        enqueueOnGui([obsolete, cb, image = std::move(image)] {
            if (!obsolete() && cb) cb(image);
        });
    };
    if (!raster || page < 0 || pageSize.isEmpty()) {
        publish({});
        return;
    }
    const RenderKey key{page, pageSize, {}};
    if (auto image = state->cached(key); !image.isNull()) {
        publish(std::move(image));
        return;
    }
    state->pool.start([state, raster, generation, key, obsolete, publish] {
        if (obsolete()) return;
        auto image = state->cached(key);
        if (image.isNull()) {
            image = raster->renderPage(key.page, key.size, obsolete);
            if (obsolete()) return;
            state->store(key, image, generation);
        }
        publish(std::move(image));
    }, priority);
}

void PdfRenderer::requestPage(int page, const QSize& pageSize, PageCallback cb,
                              CancelToken cancelled) {
    requestImage(page, pageSize, std::move(cb), std::move(cancelled), 15);
}

QSize PdfRenderer::previewSize(const QSize& pageSize) {
    const int longest = std::max(pageSize.width(), pageSize.height());
    const double scale = longest > 900 ? 900.0 / longest : 1.0;
    return {std::max(1, qRound(pageSize.width() * scale)),
            std::max(1, qRound(pageSize.height() * scale))};
}

void PdfRenderer::requestPreview(int page, const QSize& pageSize, PageCallback cb,
                                 CancelToken cancelled) {
    requestImage(page, pageSize.isEmpty() ? QSize{} : previewSize(pageSize),
                 std::move(cb), std::move(cancelled), 20);
}

void PdfRenderer::requestTiles(int page, int zoomBucket, const QSize& pageSize, int rotation,
                               int tileSize, PageCallback cb, QRect visibleRect,
                               CancelToken cancelled, TileCallback tileCb) {
    (void)zoomBucket;
    (void)rotation; // Tiles use unrotated page coordinates; the view rotates them.
    const QRect bounds(QPoint(0, 0), pageSize);
    visibleRect = visibleRect.isValid() ? visibleRect.intersected(bounds) : bounds;
    if (page < 0 || pageSize.isEmpty() || tileSize <= 0 || visibleRect.isEmpty()) {
        requestImage(-1, {}, std::move(cb), std::move(cancelled), 15);
        return;
    }
    if (usesWholePage(pageSize)) {
        requestPage(page, pageSize,
            [cb = std::move(cb), tileCb = std::move(tileCb), visibleRect, tileSize,
             pageSize, cancelled](const QImage& image) {
                if (!image.isNull() && tileCb) {
                    for (int y = visibleRect.top() / tileSize * tileSize;
                         y <= visibleRect.bottom(); y += tileSize) {
                        for (int x = visibleRect.left() / tileSize * tileSize;
                             x <= visibleRect.right(); x += tileSize) {
                            if (cancelled && cancelled->load()) return;
                            const QRect rect(x, y, std::min(tileSize, pageSize.width() - x),
                                             std::min(tileSize, pageSize.height() - y));
                            tileCb(rect, image.copy(rect));
                        }
                    }
                }
                if (cb) cb(image);
            }, cancelled);
        return;
    }

    const auto state = state_;
    unsigned long generation;
    std::shared_ptr<PopplerBridge> raster;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        generation = state->generation.load();
        raster = state->raster;
    }
    auto obsolete = [state, generation, cancelled] {
        return generation != state->generation.load() || (cancelled && cancelled->load());
    };
    // At large zooms, keep only viewport tiles instead of allocating a
    // potentially gigabyte-sized full-page image.
    state->pool.start([state, raster, generation, page, pageSize, visibleRect, tileSize,
                       cb = std::move(cb), tileCb = std::move(tileCb), obsolete] {
        if (obsolete()) return;
        bool any = false;
        for (int y = visibleRect.top() / tileSize * tileSize; y <= visibleRect.bottom(); y += tileSize) {
            for (int x = visibleRect.left() / tileSize * tileSize; x <= visibleRect.right(); x += tileSize) {
                if (obsolete()) return;
                const QRect rect(x, y, std::min(tileSize, pageSize.width() - x),
                                 std::min(tileSize, pageSize.height() - y));
                const RenderKey key{page, pageSize, rect};
                auto tile = state->cached(key);
                if (tile.isNull() && raster) {
                    tile = raster->renderTile(page, pageSize, rect, obsolete);
                    if (obsolete()) return;
                    state->store(key, tile, generation);
                }
                any = any || !tile.isNull();
                if (!tile.isNull() && tileCb) {
                    enqueueOnGui([obsolete, tileCb, rect, tile] {
                        if (!obsolete()) tileCb(rect, tile);
                    });
                }
            }
        }
        enqueueOnGui([obsolete, cb, any] {
            if (!obsolete() && cb) cb(any ? QImage(1, 1, QImage::Format_ARGB32) : QImage{});
        });
    }, 15);
}

void PdfRenderer::setPrefetchWindow(int firstPage, int lastPage) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto it = state_->prefetch.begin(); it != state_->prefetch.end();) {
        if (it->first.page < firstPage || it->first.page > lastPage) {
            it->second->store(true);
            it = state_->prefetch.erase(it);
        } else {
            ++it;
        }
    }
}

void PdfRenderer::prefetchPage(int page, const QSize& pageSize, int priority) {
    if (page < 0 || pageSize.isEmpty()) return;
    const auto state = state_;
    const RenderKey key{page, pageSize, {}};
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cache.count(key) || state->prefetch.count(key)) return;
        // A zoom change invalidates older speculative requests for this page.
        for (auto it = state->prefetch.begin(); it != state->prefetch.end();) {
            if (it->first.page == page && it->first.size != pageSize) {
                it->second->store(true);
                it = state->prefetch.erase(it);
            } else {
                ++it;
            }
        }
        state->prefetch.emplace(key, cancelled);
    }
    requestImage(page, pageSize, [state, key, cancelled](const QImage&) {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto it = state->prefetch.find(key);
        if (it != state->prefetch.end() && it->second == cancelled) state->prefetch.erase(it);
    }, cancelled, priority);
}

} // namespace reader
