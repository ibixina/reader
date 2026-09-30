#pragma once
#include "core/Types.h"
#include "pdf/PopplerBridge.h"
#include <QImage>
#include <QRect>
#include <QSize>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

class QPdfDocument;

namespace reader {

class PdfRenderer {
public:
    // Each capacity unit is the memory occupied by a 512x512 RGBA tile.
    explicit PdfRenderer(std::size_t tileCapacity = 192);
    ~PdfRenderer();
    void attach(QPdfDocument* doc, const DocumentId& id);
    void attachRaster(std::shared_ptr<PopplerBridge> raster, const DocumentId& id);
    void detach();

    using CancelToken = std::shared_ptr<const std::atomic<bool>>;
    using PageCallback = std::function<void(const QImage&)>;
    using TileCallback = std::function<void(const QRect&, const QImage&)>;

    // Cache reads never enter the PDF engine, so a prefetched page can paint
    // in its first frame without waiting for a queued callback.
    QImage cachedPage(int page, const QSize& pageSize) const;
    void requestPage(int page, const QSize& pageSize, PageCallback cb,
                     CancelToken cancelled = nullptr);
    static bool usesWholePage(const QSize& pageSize);
    void requestTiles(int page, int zoomBucket, const QSize& pageSize, int rotation,
                      int tileSize, PageCallback cb, QRect visibleRect = {},
                      CancelToken cancelled = nullptr, TileCallback tileCb = nullptr);
    static QSize previewSize(const QSize& pageSize);
    void requestPreview(int page, const QSize& pageSize, PageCallback cb,
                        CancelToken cancelled = nullptr);
    void prefetchPage(int page, const QSize& pageSize, int priority = -5);
    void setPrefetchWindow(int firstPage, int lastPage);

private:
    void requestImage(int page, const QSize& pageSize, PageCallback cb,
                      CancelToken cancelled, int priority);
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace reader
