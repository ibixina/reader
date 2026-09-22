// Raster regression test: PopplerBridge must produce a bright (mostly
// white) page image for a real paper. Catches a dead raster backend like
// the all-black QPdfDocument::render output seen in this environment.
#include <QGuiApplication>
#include <QImage>
#include <cmath>
#include <cstdlib>
#include <iostream>

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

static double darkPercent(const QImage& img) {
    // Share of non-white pixels: catches antialiased glyphs too.
    long ink = 0, total = 0;
    for (int y = 0; y < img.height(); y += 3)
        for (int x = 0; x < img.width(); x += 3) {
            if (img.pixelColor(x, y).lightnessF() < 0.85) ++ink;
            ++total;
        }
    return 100.0 * ink / total;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const char* home = std::getenv("HOME");
    const std::string pdf = std::string(home ? home : "/tmp") + "/render_sample.pdf";
    writeSamplePdf(pdf);
    PopplerBridge bridge;
    CHECK(bridge.open(QString::fromStdString(pdf)));
    CHECK(bridge.pageCount() == 1);
    QImage img = bridge.renderPage(0, QSize(306, 396));
    CHECK(!img.isNull());
    CHECK(img.width() == 306 && img.height() == 396);
    double darkPct = darkPercent(img);
    std::cout << "dark%=" << darkPct << "\n";
    CHECK(darkPct < 30);
    CHECK(darkPct > 0.05); // not a blank page either: text must land
    QImage tile = bridge.renderTile(0, QSize(306, 396), QRect(0, 0, 100, 120));
    CHECK(!tile.isNull());
    CHECK(tile.size() == QSize(100, 120));
    CHECK(std::abs(tile.pixelColor(50, 50).lightnessF() -
                  img.pixelColor(50, 50).lightnessF()) < 0.05);
    if (failures == 0) std::cout << "ALL RENDER TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
