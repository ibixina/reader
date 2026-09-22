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
    // 64 tiles x 512x512x4B ~= 64MB worst case (was 256 ~= 256MB).
    explicit PdfRenderer(std::size_t tileCapacity = 64);
    ~PdfRenderer();
    void attach(QPdfDocument* doc, const DocumentId& id);
    // The active reader uses the immutable Poppler raster source. It is
    // shared with worker requests and detached by generation, so paintEvent
    // never performs raster work and a reopen cannot publish stale pixels.
    void attachRaster(std::shared_ptr<PopplerBridge> raster, const DocumentId& id);
    void detach();

    using TileCallback = std::function<void(RenderKey, QImage)>;
    void requestTile(const RenderKey& key, double dpi, TileCallback cb);
    using PageCallback = std::function<void(const QImage&)>;
    void requestPage(int page, int zoomBucket, const QSize& size, int rotation, PageCallback cb);
    void requestTiles(int page, int zoomBucket, const QSize& pageSize, int rotation,
                      int tileSize, PageCallback cb, QRect visibleRect = {});
    // Fast blur-up placeholder: tiny render at highest priority so a fresh
    // page is never blank while sharp tiles land. Backed by kind=2 keys.
    void requestPreview(int page, const QSize& pageSize, PageCallback cb);
    // Warm the full-page cache for an adjacent page at low priority.
    // No callback: a later requestTiles slices from the cached full render.
    void prefetchPage(int page, const QSize& pageSize);
    void setTileSize(int px);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace reader
