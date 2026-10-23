#pragma once
#include <QByteArray>
#include <QColor>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <string>
#include <vector>

namespace reader {

// One saved highlight row: unrotated PDF points, top-left origin — the same
// space as the reader's page sizes and selection geometry.
struct HighlightRow {
    int page = 0;
    QRectF rect;
    QColor color = QColor(0xff, 0xe0, 0x66);
};

// Overlay PDF for `qpdf --overlay`: same page count and MediaBoxes as the
// source, translucent row rects in each row's color in bottom-origin content space.
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

// Stable fingerprint of a set of rows (order-independent): an unchanged set
// skips the write. Empty rows give an empty signature.
QString highlightRowsSignature(const std::vector<HighlightRow>& rows);

// Autosave into the user's file without touching what the viewer shows.
// Every save starts from the untouched copy at pristinePath (made from path
// when missing, so the caller must only omit it while path is still
// untouched): path becomes pristine + rows, or the pristine bytes again
// when no rows remain. path is replaced by an atomic rename, so a viewer
// that already has the file open keeps reading its old bytes.
struct HighlightSaveJob {
    QString path;
    QString pristinePath;
    std::vector<QSizeF> pageSizes;
    std::vector<HighlightRow> rows;
};
struct HighlightSaveResult {
    QString error; // empty on success
    std::string savedHash;
    qint64 savedSize = 0;
    qint64 savedMtime = 0;
};
HighlightSaveResult saveHighlightsInto(const HighlightSaveJob& job);
// Where the untouched copy of a document lives.
QString pristineCopyPath(const std::string& documentId);

} // namespace reader
