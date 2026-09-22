// Interaction test for PDF text selection: synthetic mouse drag selects
// engine text into AI context, double-click snaps to a whole word, plain
// click clears. Headless/offscreen.
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QLabel>
#include <QMouseEvent>
#include <QPdfDocument>
#include <QPushButton>
#include <QTextEdit>
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

    // Highlight toggles on the live selection: the first press applies it
    // exactly once, repeats never stack washes, and pressing h again
    // removes it. Starts from a clean slate: the round-trip annotation
    // above geometrically covers every later drag on this page.
    {
        CHECK(rapp.annotations->deleteAnnotation("testdoc", "u-test-1"));
        mousePress(*page, W(70, 60));
        mouseMove(*page, W(150, 70));
        mouseMove(*page, W(250, 80));
        mouseRelease(*page, W(250, 80));
        CHECK(!currentText(rapp).empty());
        const std::size_t before = rapp.annotations->annotationsFor("testdoc").size();
        CHECK(!view.hasHighlightForCurrentSelection());
        CHECK(view.highlightCurrentSelection());
        CHECK(view.hasHighlightForCurrentSelection());
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before + 1);
        CHECK(!view.highlightCurrentSelection()); // already applied: no stack
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before + 1);
        CHECK(view.removeHighlightForCurrentSelection());
        CHECK(!view.hasHighlightForCurrentSelection());
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before);
        CHECK(!view.removeHighlightForCurrentSelection());
    }

    // Removal survives re-extraction: block IDs may churn, but the same
    // passage (page + text + overlapping geometry) still matches.
    {
        CHECK(!currentText(rapp).empty());
        auto ctx = rapp.context.currentContext();
        CHECK(!ctx.temporary.empty());
        const std::size_t before = rapp.annotations->annotationsFor("testdoc").size();
        reader::UserAnnotation stale;
        stale.id = "u-stale-block";
        stale.anchor = ctx.temporary.front().anchor;
        stale.anchor.block = "block-from-older-extraction";
        stale.kind = "highlight";
        CHECK(rapp.annotations->saveAnnotation("testdoc", stale));
        CHECK(view.hasHighlightForCurrentSelection());
        CHECK(view.removeHighlightForCurrentSelection());
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before);
    }

    // The note editor floats over the selection instead of blocking in a
    // modal dialog: filling it and saving persists a real note.
    {
        CHECK(!currentText(rapp).empty());
        const std::size_t notesBefore = rapp.annotations->notesFor("testdoc").size();
        CHECK(view.promptNoteForCurrentSelection());
        QDialog* editor = view.findChild<QDialog*>("noteEditor");
        CHECK(editor && editor->isVisible());
        auto* text = editor ? editor->findChild<QTextEdit*>("noteText") : nullptr;
        auto* buttons =
            editor ? editor->findChild<QDialogButtonBox*>() : nullptr;
        CHECK(text && buttons);
        if (text) text->setPlainText("why does this matter?");
        if (auto* save = buttons ? buttons->button(QDialogButtonBox::Save) : nullptr)
            save->click();
        CHECK(rapp.annotations->notesFor("testdoc").size() == notesBefore + 1);
        CHECK(rapp.annotations->notesFor("testdoc").back().text == "why does this matter?");
    }

    // A sub-part of a highlight still matches: re-highlighting is a
    // no-op and removal clears the covering wash instead of stacking.
    {
        mousePress(*page, W(70, 60));
        mouseMove(*page, W(150, 70));
        mouseMove(*page, W(250, 80));
        mouseRelease(*page, W(250, 80));
        CHECK(!currentText(rapp).empty());
        const std::size_t before = rapp.annotations->annotationsFor("testdoc").size();
        CHECK(view.highlightCurrentSelection());
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before + 1);
        mousePress(*page, W(100, 65));
        mouseMove(*page, W(180, 72));
        mouseRelease(*page, W(180, 72));
        CHECK(!currentText(rapp).empty());
        CHECK(view.hasHighlightForCurrentSelection());
        CHECK(!view.highlightCurrentSelection()); // overlaps: no second wash
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before + 1);
        // A legacy stack over the same passage goes in one removal: every
        // highlight under the selection is cleared, exact or not.
        {
            auto sub = rapp.context.currentContext();
            CHECK(!sub.temporary.empty());
            reader::UserAnnotation stacked;
            stacked.id = "u-stacked";
            stacked.anchor = sub.temporary.front().anchor;
            stacked.kind = "highlight";
            CHECK(rapp.annotations->saveAnnotation("testdoc", stacked));
            CHECK(rapp.annotations->annotationsFor("testdoc").size() == before + 2);
        }
        CHECK(view.removeHighlightForCurrentSelection());
        CHECK(rapp.annotations->annotationsFor("testdoc").size() == before);
    }

    // Character-wise, gap-free highlight: a partial-word drag selects a
    // strict substring (word-granular code would yield the whole word),
    // and the painted wash covers the space between words with no gaps.
    {
        QString pageText;
        auto boxes = reader::buildWordBoxes(*doc, 0, pageText);
        const auto findWord = [&](const char* t) {
            return std::find_if(boxes.begin(), boxes.end(), [&](const reader::WordBox& w) {
                return w.text == t;
            });
        };
        auto history = findWord("history");
        auto encoder = findWord("encoder");
        CHECK(history != boxes.end());
        CHECK(encoder != boxes.end());
        if (history != boxes.end() && encoder != boxes.end()) {
            const double z = 1.25; // default widget scale, rotation 0
            const double midY = history->rect.center().y();
            // Press at the word start, stop ~40% through the same word.
            mousePress(*page, QPoint(int((history->rect.left() + 1) * z), int(midY * z)));
            mouseMove(*page, QPoint(int((history->rect.left() + history->rect.width() * 0.4) * z),
                                    int(midY * z)));
            mouseRelease(*page, QPoint(int((history->rect.left() + history->rect.width() * 0.4) * z),
                                       int(midY * z)));
            const std::string part = currentText(rapp);
            CHECK(!part.empty());
            CHECK(part.size() < history->text.size());
            CHECK(history->text.startsWith(QString::fromStdString(part)));
            // Span two words: every background pixel in the inter-word gap
            // must carry the highlight wash (blue tint over white).
            mousePress(*page, QPoint(int(history->rect.center().x() * z), int(midY * z)));
            mouseMove(*page, QPoint(int(encoder->rect.center().x() * z), int(midY * z)));
            mouseRelease(*page, QPoint(int(encoder->rect.center().x() * z), int(midY * z)));
            CHECK(!currentText(rapp).empty());
            const QImage shot = page->grab().toImage();
            const int gx0 = int(history->rect.right() * z);
            const int gx1 = int(encoder->rect.left() * z);
            const int gy = int(midY * z);
            int checked = 0;
            bool gapWashed = gx1 - gx0 >= 2;
            for (int x = gx0; x <= gx1 && gapWashed; ++x) {
                if (x < 0 || gy < 0 || x >= shot.width() || gy >= shot.height()) {
                    gapWashed = false;
                    break;
                }
                const QRgb p = shot.pixel(x, gy);
                if (qGray(p) < 150) continue; // glyph ink: not background
                ++checked;
                if (!(qBlue(p) - qRed(p) > 25 && qBlue(p) > 200)) gapWashed = false;
            }
            CHECK(checked > 0);
            CHECK(gapWashed);
        }
    }

    // Multiline highlight hugs the selected rows: pressing h on a drag
    // spanning two lines saves one annotation per row, so unselected text
    // on the first/last row stays unpainted (no bounding-box block).
    {
        QString pageText;
        auto boxes = reader::buildWordBoxes(*doc, 0, pageText);
        const auto findWord = [&](const char* t) {
            return std::find_if(boxes.begin(), boxes.end(), [&](const reader::WordBox& w) {
                return w.text == t;
            });
        };
        auto posterior = findWord("Posterior"); // first line
        auto recurrent = findWord("recurrent"); // second line
        auto history = findWord("history");     // second line
        CHECK(posterior != boxes.end());
        CHECK(recurrent != boxes.end());
        CHECK(history != boxes.end());
        if (posterior != boxes.end() && recurrent != boxes.end() &&
            history != boxes.end()) {
            const double z = 1.25;
            const std::size_t before = rapp.annotations->annotationsFor("testdoc").size();
            mousePress(*page, QPoint(int(posterior->rect.center().x() * z),
                                     int(posterior->rect.center().y() * z)));
            mouseMove(*page, QPoint(int(history->rect.center().x() * z),
                                     int(history->rect.center().y() * z)));
            mouseRelease(*page, QPoint(int(history->rect.center().x() * z),
                                       int(history->rect.center().y() * z)));
            CHECK(!currentText(rapp).empty());
            CHECK(view.highlightCurrentSelection());
            CHECK(view.hasHighlightForCurrentSelection());
            CHECK(rapp.annotations->annotationsFor("testdoc").size() == before + 2);
            const QImage shot = page->grab().toImage();
            auto userWashed = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= shot.width() || y >= shot.height()) return false;
                const QRgb p = shot.pixel(x, y);
                if (qGray(p) < 150) return false; // glyph ink
                return qBlue(p) < 225;           // yellow wash over white
            };
            // Unselected prefix of the first row stays clean.
            const int preX = int((posterior->rect.left() - 4) * z);
            const int preY = int(posterior->rect.center().y() * z);
            CHECK(!userWashed(preX, preY));
            // Selected gap on the second row is washed.
            const int gx = int((recurrent->rect.right() + history->rect.left()) / 2 * z);
            const int gy = int(history->rect.center().y() * z);
            CHECK(userWashed(gx, gy));
            CHECK(view.removeHighlightForCurrentSelection());
            CHECK(!view.hasHighlightForCurrentSelection());
            CHECK(rapp.annotations->annotationsFor("testdoc").size() == before);
            const QImage after = page->grab().toImage();
            const QRgb p = after.pixel(gx, gy);
            CHECK(!(qGray(p) >= 150 && qBlue(p) < 225));
        }
    }

    // Row-aware endpoint snap: with tight book leading (3pt inter-line
    // gap), a press just below "makes" must anchor its row, not the word
    // on the row below. The old global-nearest rule picked the lower word
    // (d=1 vs d=4) and the whole selection started a line late.
    {
        std::vector<reader::WordBox> words;
        auto box = [&](const char* t, double x, double y, double w) {
            reader::WordBox b;
            b.text = t;
            b.rect = QRectF(x, y, w, 12);
            words.push_back(b);
        };
        box("makes", 100, 100, 40);     // row 0, bottom = 112
        box("explicit", 145, 100, 55);  // row 0
        box("or", 100, 115, 20);        // row 1, top = 115 (3pt gap)
        box("premises", 125, 115, 60);  // row 1
        CHECK(reader::snapWordIndex(words, QPointF(120, 105)) == 0);  // inside
        CHECK(reader::snapWordIndex(words, QPointF(120, 114)) == 0);  // gap: row 0
        CHECK(reader::snapWordIndex(words, QPointF(120, 116)) == 2);  // past band: row 1
        CHECK(reader::snapWordIndex(words, QPointF(500, 500)) == -1); // far away
    }

    // Page-size accidental drags never become selections.
    {
        CHECK(!PdfView::isSelectionTooLarge("a short passage"));
        CHECK(!PdfView::isSelectionTooLarge(QString(2000, 'x')));
        CHECK(PdfView::isSelectionTooLarge(QString(2001, 'x')));
        CHECK(PdfView::isSelectionTooLarge(QString(5000, 'x')));
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
