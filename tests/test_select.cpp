// Interaction test for PDF text selection: synthetic mouse drag selects
// engine text into AI context, double-click snaps to a whole word, plain
// click clears. Headless/offscreen.
#include <QApplication>
#include <QEventLoop>
#include <QLabel>
#include <QMouseEvent>
#include <QPdfDocument>
#include <QTimer>
#include <iostream>
#include <cstdlib>

#include "app/Application.h"
#include "ui/PdfView.h"
#include "../tests/sample_pdf.h"

static int failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures; \
        } \
    } while (0)

using namespace reader;

static void mousePress(QWidget& w, const QPoint& pos) {
    QMouseEvent ev(QEvent::MouseButtonPress, QPointF(pos), QPointF(pos), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &ev);
}

static void mouseMove(QWidget& w, const QPoint& pos) {
    QMouseEvent ev(QEvent::MouseMove, QPointF(pos), QPointF(pos), Qt::NoButton, Qt::LeftButton,
                   Qt::NoModifier);
    QApplication::sendEvent(&w, &ev);
}

static void mouseRelease(QWidget& w, const QPoint& pos) {
    QMouseEvent ev(QEvent::MouseButtonRelease, QPointF(pos), QPointF(pos), Qt::LeftButton,
                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &ev);
}

static void mouseDClick(QWidget& w, const QPoint& pos) {
    QMouseEvent ev(QEvent::MouseButtonDblClick, QPointF(pos), QPointF(pos), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &ev);
}

static std::string currentText(Application& app) {
    auto ctx = app.context.currentContext();
    if (ctx.temporary.empty()) return {};
    return ctx.temporary.front().extractedText;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const char* home = std::getenv("HOME");
    const QString pdfPath = QString::fromStdString(std::string(home ? home : "/tmp") +
                                                   "/qt_sample.pdf");
    writeSamplePdf(pdfPath.toStdString());
    Application rapp;
    PdfView view(&rapp);
    auto deleter = [](QPdfDocument* d) { d->deleteLater(); };
    std::shared_ptr<QPdfDocument> doc(new QPdfDocument, deleter);
    doc->load(pdfPath);
    CHECK(doc->status() == QPdfDocument::Status::Ready);
    view.attachDocument(doc, pdfPath);
    // Word index arrives on the background doc lane; wait for its explicit
    // readiness signal so selection assertions do not race worker startup.
    bool geometryReady = false;
    QObject::connect(&view, &PdfView::selectionGeometryReady, &app,
                     [&](int page) {
                         if (page == 0) geometryReady = true;
                     });
    // Reopen while the first geometry job is in flight. The old job owns
    // only shared document/index/generation state and must not dereference
    // the destroyed PdfView.
    view.prefetchAround(0);
    view.attachDocument(doc, pdfPath);
    view.prefetchAround(0);
    QEventLoop readyLoop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&view, &PdfView::selectionGeometryReady, &readyLoop,
                     [&](int page) {
                         if (page == 0) readyLoop.quit();
                     });
    QObject::connect(&timeout, &QTimer::timeout, &readyLoop, &QEventLoop::quit);
    timeout.start(5000);
    readyLoop.exec();
    CHECK(geometryReady);

    auto pages = view.findChildren<QLabel*>();
    CHECK(!pages.empty());
    QLabel* page = pages.front();
    {
        for (auto [s, l] : {std::pair<int,int>{0,6}, {10,5}, {30,8}}) {
            QPdfSelection q = doc->getSelectionAtIndex(0, s, l);
            std::cout << "atIndex(" << s << "," << l << ")=[" << q.text().toStdString()
                      << "] valid=" << q.isValid() << "\n";
        }
    }
    // Engine reports the sample lines at y≈59-105 Qt points (top-left
    // origin); widget scale is 1.25x.
    auto W = [&](float xPt, float yPt) { return QPoint(int(xPt * 1.25), int(yPt * 1.25)); };

    // Drag across the line: engine text lands in AI context.
    mousePress(*page, W(70, 60));
    mouseMove(*page, W(200, 75));
    mouseMove(*page, W(430, 95));
    mouseRelease(*page, W(430, 95));
    std::string sel = currentText(rapp);
    CHECK(sel.find("history encoder") != std::string::npos);

    // Highlight save -> overlay reload round-trip (the Highlight menu path).
    {
        rapp.model.document.id = "testdoc";
        auto ctx = rapp.context.currentContext();
        CHECK(!ctx.temporary.empty());
        reader::UserAnnotation ann;
        ann.id = "u-test-1";
        ann.anchor = ctx.temporary.front().anchor;
        ann.anchor.page = 0;
        ann.kind = "highlight";
        CHECK(rapp.annotations->saveAnnotation("testdoc", ann));
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == 1);
        view.refreshUserOverlays(); // must not crash; paints saved wash
        CHECK(rapp.annotations->annotationsFor("testdoc").front().kind == "highlight");
    }

    // Double-click on a body word snaps to exactly one whole word.
    // (Body words sit at y≈93-105; y=80 would be the inter-line gap.)
    bool wordOk = false;
    for (float x = 100; x < 500; x += 15) {
        mouseDClick(*page, W(x, 97));
        std::string w = currentText(rapp);
        if (!w.empty() && w.find(' ') == std::string::npos && w.size() > 2) {
            wordOk = true;
            break;
        }
    }
    CHECK(wordOk);

    // Plain click on empty area clears the context.
    mousePress(*page, W(600, 400));
    mouseRelease(*page, W(600, 400));
    CHECK(currentText(rapp).empty());

    // Renderer lifecycle: a queued result from the old document must be
    // discarded after a generation switch, and a failed backend must still
    // complete its request so a page cannot remain permanently pending.
    reader::PdfRenderer renderer(8);
    auto oldRaster = std::make_shared<PopplerBridge>();
    CHECK(oldRaster->open(pdfPath));
    renderer.attachRaster(oldRaster, "old-document");
    int staleCallbacks = 0;
    renderer.requestTiles(0, 1000, QSize(306, 396), 0, 128,
                          [&](const QImage&) { ++staleCallbacks; }, QRect(0, 0, 128, 128));
    renderer.attachRaster(std::make_shared<PopplerBridge>(), "new-document");
    QApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK(staleCallbacks == 0);

    bool failedCompleted = false;
    bool failedWasNull = false;
    renderer.requestTiles(0, 1000, QSize(306, 396), 0, 128,
                          [&](const QImage& image) {
                              failedCompleted = true;
                              failedWasNull = image.isNull();
                          }, QRect(0, 0, 128, 128));
    QEventLoop failureLoop;
    QTimer failureTimeout;
    failureTimeout.setSingleShot(true);
    QObject::connect(&failureTimeout, &QTimer::timeout, &failureLoop, &QEventLoop::quit);
    QTimer::singleShot(0, &failureLoop, [&] {
        if (failedCompleted) failureLoop.quit();
    });
    QObject::connect(&failureLoop, &QEventLoop::destroyed, &failureTimeout,
                     &QTimer::stop);
    failureTimeout.start(1000);
    while (!failedCompleted && failureTimeout.isActive()) failureLoop.processEvents();
    CHECK(failedCompleted);
    CHECK(failedWasNull);

    if (failures == 0) std::cout << "ALL SELECTION TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
