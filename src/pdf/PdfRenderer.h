#pragma once
#include "core/LruCache.h"
#include "core/Types.h"
#include "pdf/PopplerBridge.h"
#include <QImage>
#include <QRect>
#include <QSize>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

class QPdfDocument;
class QImage;

namespace reader {

// Tile renderer decoupled from analysis (§6): viewport -> visible tiles ->
// cache hit ? display : background render. AI/indexing threads never share
// the critical rendering path.
struct RenderKey {
    DocumentId document;
    int page = 0;
    int zoomBucket = 0;
    int tileX = 0, tileY = 0;
    int pixelWidth = 0, pixelHeight = 0;
    int rotation = 0;
    int kind = 0; // 0 = tile, 1 = assembled page/thumbnail
    bool operator==(const RenderKey& o) const {
        return document == o.document && page == o.page && zoomBucket == o.zoomBucket &&
               tileX == o.tileX && tileY == o.tileY && pixelWidth == o.pixelWidth &&
               pixelHeight == o.pixelHeight && rotation == o.rotation && kind == o.kind;
    }
};

struct RenderKeyHash {
    std::size_t operator()(const RenderKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.document);
        const auto mix = [&h](std::size_t value) {
            h ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
                 (h << 6) + (h >> 2);
        };
        mix(std::hash<int>{}(k.page));
        mix(std::hash<int>{}(k.zoomBucket));
        mix(std::hash<int>{}(k.tileX));
        mix(std::hash<int>{}(k.tileY));
        mix(std::hash<int>{}(k.pixelWidth));
        mix(std::hash<int>{}(k.pixelHeight));
        mix(std::hash<int>{}(k.rotation));
        mix(std::hash<int>{}(k.kind));
        return h;
    }
};

// Key for the full-page render cache: one Poppler render per (page, size)
// serves every tile, instead of one render per tile.
struct PageRenderKey {
    DocumentId document;
    int page = 0;
    int pixelWidth = 0, pixelHeight = 0;
    bool operator==(const PageRenderKey& o) const {
        return document == o.document && page == o.page && pixelWidth == o.pixelWidth &&
               pixelHeight == o.pixelHeight;
    }
};

struct PageRenderKeyHash {
    std::size_t operator()(const PageRenderKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.document);
        const auto mix = [&h](std::size_t value) {
            h ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
                 (h << 6) + (h >> 2);
        };
        mix(std::hash<int>{}(k.page));
        mix(std::hash<int>{}(k.pixelWidth));
        mix(std::hash<int>{}(k.pixelHeight));
        return h;
    }
};

class QtPdfEngineBridge;

class PdfRenderer {
public:
    // 192 tiles x 512x512x4B ~= 192MB worst case: enough that scrolling a
    // long paper at DPR 2 (12 tiles/page) keeps ~16 pages resident instead
    // of ~5, so going back does not re-render. Widget-level caches hold the
    // immediate neighborhood only; this is the working-set cache.
    explicit PdfRenderer(std::size_t tileCapacity = 192);
    ~PdfRenderer();
    void attach(QPdfDocument* doc, const DocumentId& id);
    // The active reader uses the immutable Poppler raster source. It is
    // shared with worker requests and detached by generation, so paintEvent
    // never performs raster work and a reopen cannot publish stale pixels.
    void attachRaster(std::shared_ptr<PopplerBridge> raster, const DocumentId& id);
    void detach();

    // Cooperative cancellation for queued/running raster jobs: the widget
    // sets the flag when the request is superseded (scrolled away, zoomed,
    // evicted) so the single raster lane never backs up behind pages the
    // reader has already left. Jobs check the token before and after the
    // render; a cancelled job publishes nothing.
    using CancelToken = std::shared_ptr<const std::atomic<bool>>;

    using PageCallback = std::function<void(const QImage&)>;
    // Progressive per-tile delivery: fired as each tile completes so the
    // widget paints incrementally without blanking existing pixels. The
    // PageCallback still fires once when all requested tiles are done.
    using TileCallback = std::function<void(const QRect& rect, const QImage& tile)>;
    void requestTiles(int page, int zoomBucket, const QSize& pageSize, int rotation,
                      int tileSize, PageCallback cb, QRect visibleRect = {},
                      CancelToken cancelled = nullptr, TileCallback tileCb = nullptr);
    // Fast blur-up placeholder: a sub-1000px render at high priority so a
    // fresh page shows a legible draft while sharp tiles land. Backed by
    // kind=2 keys.
    void requestPreview(int page, const QSize& pageSize, PageCallback cb,
                        CancelToken cancelled = nullptr);
    // Warm the full-page cache for a page the reader is heading toward.
    // No callback: a later requestTiles slices from the cached full render.
    // priority goes straight to the worker pool: the page the reader will
    // see next must not wait behind unrelated work.
    void prefetchPage(int page, const QSize& pageSize, int priority = -5);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace reader
