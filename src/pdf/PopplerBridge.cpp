#include "pdf/PopplerBridge.h"
#include <QPainter>
#include <QtGlobal>

bool PopplerBridge::open(const QString& path) {
    doc_ = Poppler::Document::load(path);
    if (!doc_ || doc_->isLocked() || doc_->numPages() <= 0) {
        doc_.reset();
        return false;
    }
    doc_->setRenderHint(Poppler::Document::Antialiasing, true);
    doc_->setRenderHint(Poppler::Document::TextAntialiasing, true);
    doc_->setRenderHint(Poppler::Document::TextHinting, true);
    doc_->setRenderHint(Poppler::Document::ThinLineSolid, true);
    return true;
}

int PopplerBridge::pageCount() const {
    return doc_ ? doc_->numPages() : 0;
}

QSizeF PopplerBridge::pageSize(int page) const {
    if (!doc_) return {};
    auto pg = doc_->page(page);
    return pg ? pg->pageSizeF() : QSizeF{};
}

QImage PopplerBridge::renderPage(int page, QSize px) const {
    if (!doc_ || px.isEmpty()) return {};
    auto pg = doc_->page(page);
    if (!pg) return {};
    QSizeF pts = pg->pageSizeF();
    if (pts.isEmpty()) return {};
    // DPI chosen so the raster matches the requested pixel size exactly.
    double dpiX = 72.0 * px.width() / pts.width();
    double dpiY = 72.0 * px.height() / pts.height();
    QImage img = pg->renderToImage(dpiX, dpiY);
    if (img.isNull() || img.size() == px) return img;
    // Poppler rounds to whole pixels, so fractional point sizes land 1px
    // off. A bilinear rescale of the whole page for 1px softens every
    // glyph; pad/crop instead to keep text sharp. Only genuinely
    // different sizes take a smooth rescale.
    if (qAbs(img.width() - px.width()) <= 2 && qAbs(img.height() - px.height()) <= 2) {
        QImage fixed(px, QImage::Format_ARGB32_Premultiplied);
        fixed.fill(Qt::white);
        QPainter painter(&fixed);
        painter.drawImage(0, 0, img);
        return fixed;
    }
    return img.scaled(px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QImage PopplerBridge::renderTile(int page, QSize fullPx, QRect tilePx) const {
    if (!doc_ || fullPx.isEmpty() || tilePx.isEmpty()) return {};
    auto pg = doc_->page(page);
    if (!pg) return {};
    QSizeF pts = pg->pageSizeF();
    if (pts.isEmpty()) return {};
    tilePx = tilePx.intersected(QRect(QPoint(0, 0), fullPx));
    if (tilePx.isEmpty()) return {};

    // Poppler's crop arguments are in pixels at the requested resolution.
    // Use the same anisotropic scale as renderPage and clamp the crop to the
    // raster's exact bounds at the edge, then normalize the result to the
    // requested tile dimensions.
    const double xres = 72.0 * fullPx.width() / pts.width();
    const double yres = 72.0 * fullPx.height() / pts.height();
    QImage image = pg->renderToImage(xres, yres, tilePx.x(), tilePx.y(),
                                     tilePx.width(), tilePx.height(),
                                     Poppler::Page::Rotate0);
    if (image.isNull() || image.size() == tilePx.size()) return image;
    if (qAbs(image.width() - tilePx.width()) <= 2 &&
        qAbs(image.height() - tilePx.height()) <= 2) {
        QImage fixed(tilePx.size(), QImage::Format_ARGB32_Premultiplied);
        fixed.fill(Qt::white);
        QPainter painter(&fixed);
        painter.drawImage(0, 0, image);
        return fixed;
    }
    return image.scaled(tilePx.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
