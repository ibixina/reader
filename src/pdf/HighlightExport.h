#pragma once
#include <QByteArray>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <vector>

namespace reader {

// One saved highlight row: unrotated PDF points, top-left origin — the same
// space as the reader's page sizes and selection geometry.
struct HighlightRow {
    int page = 0;
    QRectF rect;
};

// Overlay PDF for `qpdf --overlay`: same page count and MediaBoxes as the
// source, translucent yellow row rects in bottom-origin content space.
// Pages without rows get empty content. Empty when there is nothing to
// export or inputs are invalid. Hand-written bytes (no PDF library needed);
// numbers are C-locale formatted, objects sequentially numbered.
QByteArray buildHighlightOverlay(const std::vector<QSizeF>& pageSizes,
                                 const std::vector<HighlightRow>& rows);
// "<base> - highlighted.pdf" next to the source file.
QString highlightExportName(const QString& sourcePath);

// Full job for the Save action. Blocking worker body, never the UI thread:
// builds the overlay, merges it with `qpdf --overlay`, verifies the result
// (re-opens it, checks the page count), then moves it to destPath.
// The source file is never modified. "" on success, else a reason.
struct HighlightExportJob {
    QString srcPath;
    QString destPath;
    std::vector<QSizeF> pageSizes;
    std::vector<HighlightRow> rows;
};
QString embedHighlights(const HighlightExportJob& job);

} // namespace reader
