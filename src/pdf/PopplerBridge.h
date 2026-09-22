#pragma once
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <memory>
#include <poppler-qt6.h>

// Raster backend (§54): Poppler renders pages (QtPdf's rasterizer draws
// nothing in some builds), while QPdfDocument keeps serving text,
// selection geometry and outline (all verified working).
class PopplerBridge {
public:
    bool open(const QString& path);
    int pageCount() const;
    QSizeF pageSize(int page) const;
    QImage renderPage(int page, QSize px) const;
    // Render only the requested pixel rectangle from a page raster. The
    // rectangle is expressed in the unrotated full-page pixel coordinate
    // space, so callers can assemble tiles without rendering the page again.
    QImage renderTile(int page, QSize fullPx, QRect tilePx) const;

private:
    std::unique_ptr<Poppler::Document> doc_;
};
