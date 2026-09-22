// Render stress regression test: hammers PdfRenderer the way real scrolling
// does (concurrent tile/preview/prefetch bursts across pages, then a document
// switch mid-flight). Static single-render tests all passed while the app
// crashed on scroll, because nothing exercised concurrent rendering, cache
// coalescing, or generation guards. This test fails on hangs, lost callbacks
// and crashes instead.
#include <QGuiApplication>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "pdf/PdfRenderer.h"
#include "pdf/PopplerBridge.h"

static int failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures; \
        } \
    } while (0)

static void writePagesPdf(const std::string& path, int pages) {
    std::vector<std::string> objs;
    objs.push_back("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj");
    std::string kids;
    for (int i = 0; i < pages; ++i) kids += std::to_string(3 + 2 * i) + " 0 R ";
    objs.push_back("2 0 obj << /Type /Pages /Kids [" + kids + "] /Count " +
                   std::to_string(pages) + " >> endobj");
    const int fontObj = 3 + 2 * pages;
    for (int i = 0; i < pages; ++i) {
        objs.push_back(std::to_string(3 + 2 * i) + " 0 obj << /Type /Page /Parent 2 0 R "
                       "/MediaBox [0 0 612 792] /Contents " +
                       std::to_string(4 + 2 * i) + " 0 R /Resources << /Font << /F1 " +
                       std::to_string(fontObj) + " 0 R >> >> >> endobj");
        std::string stream = "BT /F1 18 Tf 72 720 Td (Page " + std::to_string(i + 1) +
                             " Neural Posterior Estimation) Tj ET\n"
                             "BT /F1 12 Tf 72 690 Td 15 TL (3.2 Posterior Estimation We use a "
                             "recurrent history encoder for amortization [12].) Tj ET";
        objs.push_back(std::to_string(4 + 2 * i) + " 0 obj << /Length " +
                       std::to_string(stream.size()) + " >> stream\n" + stream +
                       "\nendstream endobj");
    }
    objs.push_back(std::to_string(fontObj) +
                   " 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj");
    std::string out = "%PDF-1.4\n";
    std::vector<std::size_t> offs;
    for (auto& o : objs) {
        offs.push_back(out.size());
        out += o + "\n";
    }
    std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char buf[32];
    for (auto o : offs) {
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
        out += buf;
    }
    out += "trailer << /Size " + std::to_string(objs.size() + 1) +
           " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF";
    std::ofstream f(path, std::ios::binary);
    f << out;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const char* home = std::getenv("HOME");
    const std::string pdf = std::string(home ? home : "/tmp") + "/render_stress.pdf";
    writePagesPdf(pdf, 6);

    auto raster = std::make_shared<PopplerBridge>();
    CHECK(raster->open(QString::fromStdString(pdf)));
    CHECK(raster->pageCount() == 6);
    reader::PdfRenderer renderer;
    renderer.attachRaster(raster, pdf); // id = path, exactly like the app

    const QSize pageSize(765, 990); // ~1.25x letter, like the reader at default zoom
    std::atomic<int> invocations{0};
    std::atomic<int> nulls{0};
    auto counting = [&](const QImage& image) {
        invocations.fetch_add(1);
        if (image.isNull()) nulls.fetch_add(1);
    };

    // Scroll storm: three overlapping waves over all pages, mixing previews,
    // full tiles, partial tiles and prefetches.
    int expect = 0;
    for (int wave = 0; wave < 3; ++wave) {
        for (int p = 0; p < 6; ++p) {
            renderer.requestPreview(p, pageSize, counting);
            ++expect;
            renderer.requestTiles(p, 1250, pageSize, 0, 512, counting);
            ++expect;
            if (wave == 0) {
                renderer.requestTiles(p, 1250, pageSize, 0, 512, counting,
                                      QRect(0, 0, 512, 512));
                ++expect;
            }
            renderer.prefetchPage(p, pageSize);
        }
    }

    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(60000);
    QTimer poller;
    poller.setInterval(50);
    QObject::connect(&poller, &QTimer::timeout, [&] {
        if (invocations.load() >= expect) loop.quit();
    });
    poller.start();
    loop.exec();

    CHECK(invocations.load() == expect);
    CHECK(nulls.load() == 0);

    // Document switch mid-flight: stale callbacks must be dropped, the fresh
    // wave must still be delivered, and nothing may crash.
    renderer.detach();
    renderer.attachRaster(raster, pdf);
    std::atomic<int> fresh{0};
    renderer.requestTiles(0, 1250, pageSize, 0, 512,
                          [&](const QImage&) { fresh.fetch_add(1); });
    QEventLoop loop2;
    QTimer watchdog2;
    watchdog2.setSingleShot(true);
    QObject::connect(&watchdog2, &QTimer::timeout, &loop2, &QEventLoop::quit);
    watchdog2.start(30000);
    QTimer poller2;
    poller2.setInterval(50);
    QObject::connect(&poller2, &QTimer::timeout, [&] {
        if (fresh.load() >= 1) loop2.quit();
    });
    poller2.start();
    loop2.exec();
    CHECK(fresh.load() == 1);

    if (failures == 0) std::cout << "ALL RENDER STRESS TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
