#include "app/Application.h"
#include "ui/PdfView.h"
#include "academic_fixture.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTransform>
#include <functional>
#include <iostream>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; ++failures; } } while (0)

static bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) return true;
        QThread::msleep(2);
    }
    return predicate();
}

static bool matches(const QImage& actual, const QImage& expected) {
    if (actual.size() != expected.size() || actual.isNull()) return false;
    int differences = 0;
    for (int y = 0; y < actual.height(); y += 3)
        for (int x = 0; x < actual.width(); x += 3)
            if (std::abs(qGray(actual.pixel(x, y)) - qGray(expected.pixel(x, y))) > 8)
                ++differences;
    return differences < 5;
}

class PaintCounter : public QObject {
public:
    int paints = 0;
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) ++paints;
        return false;
    }
};

int main(int argc, char** argv) {
    QApplication qt(argc, argv);
    QTemporaryDir temporary;
    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                 : temporary.filePath("render-view.pdf");
    if (argc <= 1) reader_test::writeAcademicPdf(path.toStdString());
    auto raster = std::make_shared<PopplerBridge>();
    CHECK(raster->open(path));
    PopplerBridge reference;
    CHECK(reference.open(path));
    reader::Application app;
    PdfView view(&app);
    view.resize(800, 460);
    view.show();
    view.attachRaster(raster, path);
    const qreal dpr = view.devicePixelRatioF();
    auto sourceSize = [&](int page, double zoom) {
        const auto points = reference.pageSize(page);
        return QSize(qRound(points.width() * zoom * dpr), qRound(points.height() * zoom * dpr));
    };
    auto pageWidget = [&](int page) { return view.findChild<QWidget*>(QString("pdfPage_%1").arg(page)); };
    CHECK(!pageWidget(0)->geometry().intersects(pageWidget(1)->geometry()));
    QImage expected = reference.renderPage(0, sourceSize(0, 1.25));
    CHECK(waitFor([&] { return matches(pageWidget(0)->grab().toImage(), expected); }));

    // A settled page must stop scheduling renders and repainting itself.
    PaintCounter counter;
    pageWidget(0)->installEventFilter(&counter);
    QEventLoop idle;
    QTimer::singleShot(200, &idle, &QEventLoop::quit);
    idle.exec();
    CHECK(counter.paints <= 2);
    pageWidget(0)->removeEventFilter(&counter);

    // Exposing the lower half must use the same complete raster immediately.
    view.verticalScrollBar()->setValue(500);
    CHECK(matches(pageWidget(0)->grab().toImage(), expected));
    // Neighbor pages should display their prefetched raster in the first
    // frame, without waiting for a page or tile callback on the GUI queue.
    for (int page = 1; page < std::min(4, view.pageCount()); ++page) {
        const auto next = reference.renderPage(page, sourceSize(page, 1.25));
        view.goToPage(page);
        CHECK(matches(pageWidget(page)->grab().toImage(), next));
    }
    view.goToPage(0);
    int changes = 0;
    QObject::connect(&view, &PdfView::pageChanged, [&](int) { ++changes; });
    view.verticalScrollBar()->setValue(1500);
    CHECK(changes > 0); // Pixel navigation works before the text engine attaches.
    view.goToPage(0);

    // Retain visible content while the new zoom raster is still pending.
    view.setZoom(1.5);
    const auto duringZoom = pageWidget(0)->grab().toImage();
    int ink = 0;
    for (int y = 0; y < duringZoom.height(); y += 8)
        for (int x = 0; x < duringZoom.width(); x += 8)
            if (qGray(duringZoom.pixel(x, y)) < 200) ++ink;
    CHECK(ink > 20);
    expected = reference.renderPage(0, sourceSize(0, 1.5));
    CHECK(waitFor([&] { return matches(pageWidget(0)->grab().toImage(), expected); }));
    for (int rotation : {90, 180, 270, 0}) {
        view.setRotation(rotation);
        const auto rotated = expected.transformed(QTransform().rotate(rotation), Qt::FastTransformation);
        CHECK(waitFor([&] { return matches(pageWidget(0)->grab().toImage(), rotated); }));
    }

    if (argc <= 1) {
        // Large zooms render bounded viewport tiles. Check their pixel placement
        // against a direct Poppler crop, including device-pixel coordinates.
        view.setZoom(5.0);
        view.goToPage(0);
        view.verticalScrollBar()->setValue(0);
        view.horizontalScrollBar()->setValue(0);
        const QRect crop(0, 0, 400, 400);
        const QRect pixelCrop(0, 0, qRound(crop.width() * dpr), qRound(crop.height() * dpr));
        const auto tile = reference.renderTile(0, sourceSize(0, 5.0), pixelCrop);
        CHECK(waitFor([&] { return matches(pageWidget(0)->grab(crop).toImage(), tile); }));
        if (dpr > 1) {
            // A wide HiDPI viewport can contain more than 64 tiles; retaining
            // fewer than the viewport needs creates an endless repaint loop.
            view.resize(3000, 1800);
            view.verticalScrollBar()->setValue(0);
            view.horizontalScrollBar()->setValue(0);
            PaintCounter wide;
            pageWidget(0)->installEventFilter(&wide);
            QElapsedTimer settled;
            settled.start();
            int previousPaints = -1;
            CHECK(waitFor([&] {
                if (previousPaints != wide.paints) {
                    previousPaints = wide.paints;
                    settled.restart();
                }
                return settled.elapsed() > 200;
            }));
            CHECK(matches(pageWidget(0)->grab(crop).toImage(), tile));
            pageWidget(0)->removeEventFilter(&wide);
        }
    }
    if (!failures) std::cout << "RENDER VIEW PASSED dpr=" << dpr << " idle_paints=" << counter.paints << "\n";
    return failures ? 1 : 0;
}
