// Export round-trip: hand-written overlay + `qpdf --overlay` bakes the
// saved highlight rows into a PDF copy; poppler re-renders it and the
// highlight pixels must sit exactly on the row rects. Skipped (exit 77)
// when qpdf is unavailable.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QSizeF>
#include <QStandardPaths>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "pdf/HighlightExport.h"
#include "pdf/PopplerBridge.h"
#include "../tests/sample_pdf.h"

static int failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures; \
        } \
    } while (0)

static bool washed(const QImage& shot, int x, int y) {
    if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) return false;
    const QRgb p = shot.pixel(x, y);
    if (qGray(p) < 150) return false; // glyph ink
    return qRed(p) > 200 && qBlue(p) < 230 && qRed(p) - qBlue(p) > 20;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (QStandardPaths::findExecutable("qpdf").isEmpty()) return 77;
    const char* home = std::getenv("HOME");
    const QString root =
        QString::fromStdString(std::string(home ? home : "/tmp") + "/export_probe");
    QDir().mkpath(root);
    const QString src = root + "/in.pdf";
    writeSamplePdf(src.toStdString());

    // Pure-builder checks: no rows / bad pages produce nothing.
    CHECK(reader::buildHighlightOverlay({}, {}).isEmpty());
    CHECK(reader::buildHighlightOverlay({QSizeF(612, 792)}, {}).isEmpty());
    CHECK(reader::highlightExportName("/a/b/paper.pdf") == "/a/b/paper - highlighted.pdf");

    auto raster = std::make_shared<PopplerBridge>();
    CHECK(raster->open(src));
    const int pages = raster->pageCount();
    CHECK(pages > 0);
    std::vector<QSizeF> sizes;
    for (int p = 0; p < pages; ++p) sizes.push_back(raster->pageSize(p));

    // Highlight the top text row of page 0 (points, top-left origin).
    const QRectF row(70, 55, 360, 50);
    const QByteArray overlay =
        reader::buildHighlightOverlay(sizes, {{0, row}});
    CHECK(!overlay.isEmpty());
    const QString overlayPath = root + "/overlay.pdf";
    {
        QFile f(overlayPath);
        CHECK(f.open(QIODevice::WriteOnly));
        CHECK(f.write(overlay) == overlay.size());
    }
    const QString out = root + "/out.pdf";
    CHECK(QProcess::execute("qpdf", {src, "--overlay", overlayPath, "--", out}) == 0);
    CHECK(QFile::exists(out));

    auto baked = std::make_shared<PopplerBridge>();
    CHECK(baked->open(out));
    CHECK(baked->pageCount() == pages);
    const QImage shot = baked->renderPage(0, QSize(306, 396));
    CHECK(!shot.isNull());
    // Row rect mapped into the render: every background pixel washed…
    const double sx = shot.width() / sizes.front().width();
    const double sy = shot.height() / sizes.front().height();
    int checked = 0;
    bool allWashed = true;
    // Inset by a pixel: edge rows/cols are half-covered by construction and
    // rasterize differently per engine; placement is proven by the interior.
    for (int x = int(row.left() * sx) + 1; x < int(row.right() * sx) - 1 && allWashed; ++x)
        for (int y = int(row.top() * sy) + 1; y < int(row.bottom() * sy) - 1; ++y) {
            const QRgb p = shot.pixel(x, y);
            // Glyph cores and their antialiased edges (wash blended over
            // gray) are not background: only pure-background pixels must
            // carry the wash.
            if (qGray(p) < 200) continue;
            ++checked;
            if (!washed(shot, x, y)) allWashed = false;
        }
    CHECK(checked > 20);
    CHECK(allWashed);
    // …and far outside the row the page stays clean.
    CHECK(!washed(shot, 10, shot.height() - 10));

    // End-to-end worker path (rotation check, merge, verify, deliver):
    // the source file must be byte-identical afterwards.
    const QByteArray before = [&] {
        QFile f(src);
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    }();
    const QString final = root + "/final.pdf";
    reader::HighlightExportJob job{src, final, sizes, {{0, row}}};
    CHECK(reader::embedHighlights(job).isEmpty());
    CHECK(QFile::exists(final));
    {
        QFile f(src);
        f.open(QIODevice::ReadOnly);
        CHECK(f.readAll() == before); // original never modified
    }
    auto delivered = std::make_shared<PopplerBridge>();
    CHECK(delivered->open(final));
    CHECK(delivered->pageCount() == pages);
    const QImage shot2 = delivered->renderPage(0, QSize(306, 396));
    CHECK(!shot2.isNull());
    CHECK(washed(shot2, int((row.left() + 10) * sx), int((row.top() + 10) * sy)));

    // In-place save (the Ctrl+S path): dest == src is replaced atomically
    // and no staging files are left behind.
    const QString live = root + "/live.pdf";
    CHECK(QFile::copy(src, live));
    reader::HighlightExportJob inplace{live, live, sizes, {{0, row}}};
    CHECK(reader::embedHighlights(inplace).isEmpty());
    auto saved = std::make_shared<PopplerBridge>();
    CHECK(saved->open(live));
    CHECK(saved->pageCount() == pages);
    const QImage shot3 = saved->renderPage(0, QSize(306, 396));
    CHECK(!shot3.isNull());
    CHECK(washed(shot3, int((row.left() + 10) * sx), int((row.top() + 10) * sy)));
    bool stagedLeft = false;
    for (const auto& entry : QDir(root).entryList(QDir::Files))
        if (entry.startsWith(".reader-hl-")) stagedLeft = true;
    CHECK(!stagedLeft);

    if (failures == 0) std::cout << "ALL EXPORT TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
