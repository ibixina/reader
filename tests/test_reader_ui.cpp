// Integrated reader test for the minimal feature set: viewer, selection→
// browser-chat context, literal search, outline, notes/highlights,
// shortcuts, history, fit/rotate, and document-switch isolation.
// The browser chat needs no network here: only its local context surface
// (selection chips, copy-prompt) is exercised, never a page load or send.
#include "app/Application.h"
#include "document/DocumentAnchor.h"
#include "academic_fixture.h"
#include "ui/MainWindow.h"
#include "ui/OutlinePanel.h"
#include "ui/PdfView.h"
#include "ui/WebPanel.h"
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTextBrowser>
#include <QThread>
#include <QWebEngineView>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <algorithm>
#include <cmath>

namespace {

int failures = 0;
#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::cerr << "FAIL line " << __LINE__ << ": " #condition << '\n';           \
            ++failures;                                                                  \
        }                                                                                 \
    } while (false)

template <typename Predicate>
bool waitFor(QApplication& app, int timeoutMs, Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        app.processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    return predicate();
}

} // namespace

int main(int argc, char** argv) {
    char homeTemplate[] = "/tmp/reader-ui-XXXXXX";
    char* testHome = mkdtemp(homeTemplate);
    if (!testHome) return 2;
    setenv("HOME", testHome, 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    unsetenv("OPENAI_API_KEY");
    unsetenv("PAPER_READER_MODEL");
    QApplication qt(argc, argv);
    QCoreApplication::setOrganizationName("ReaderUiAcceptance");
    QCoreApplication::setApplicationName("ReaderUiAcceptance");

    const std::string pdfPath = std::string(testHome) + "/academic-fixture.pdf";
    const std::string secondPdfPath = std::string(testHome) + "/academic-fixture-copy.pdf";
    reader_test::writeAcademicPdf(pdfPath);
    reader_test::writeAcademicPdf(secondPdfPath);
    {
        std::ofstream distinct(secondPdfPath, std::ios::app);
        distinct << "\n% distinct document identity\n";
    }
    reader::Application app;
    MainWindow window(&app);
    window.resize(1100, 760);
    window.show();
    window.activateWindow();
    qt.processEvents(QEventLoop::AllEvents, 50);
    QElapsedTimer firstPaintTimer;
    firstPaintTimer.start();
    window.openFile(QString::fromStdString(pdfPath));

    CHECK(waitFor(qt, 5000, [&] { return !app.model.document.id.empty(); }));
    CHECK(waitFor(qt, 8000, [&] { return app.model.blocks.size() > 8; }));
    CHECK(app.model.document.pageCount == 4);
    CHECK(!app.documents->recentDocuments(10).empty());

    auto* pdf = window.findChild<PdfView*>();
    CHECK(pdf != nullptr);
    if (!pdf) return failures == 0 ? 0 : 1;
    CHECK(waitFor(qt, 3000, [&] {
        QWidget* page = window.findChild<QWidget*>("pdfPage_0");
        if (!page) return false;
        const QImage image = page->grab().toImage();
        int ink = 0;
        for (int y = 0; y < image.height(); y += 8)
            for (int x = 0; x < image.width(); x += 8)
                if (qGray(image.pixel(x, y)) < 220 && ++ink > 30) return true;
        return false;
    }));
    const qint64 firstPaintMs = firstPaintTimer.elapsed();
    qint64 selectionContextMs = -1;
    qint64 literalSearchMs = -1;
    QString selText;

    auto* browser = window.findChild<WebPanel*>();
    CHECK(browser != nullptr);
    // Fresh start (isolated HOME, no settings): the chat pane defaults on.
    CHECK(browser->isVisible());
    auto* browserContext = window.findChild<QTextBrowser*>("browserContext");
    auto* browserQuestion = window.findChild<QLineEdit*>("browserQuestion");
    CHECK(browserContext && browserQuestion);

    // An actual drag on the rendered page updates structured context while
    // preserving focus and clipboard. It must not navigate or send anything.
    QWidget* firstPage = window.findChild<QWidget*>("pdfPage_0");
    CHECK(firstPage);
    if (firstPage) {
        QGuiApplication::clipboard()->setText("clipboard sentinel");
        const auto blockIt = std::find_if(
            app.model.blocks.begin(), app.model.blocks.end(), [](const reader::TextBlock& block) {
                return block.text.find("Interactive reading systems") != std::string::npos;
            });
        CHECK(blockIt != app.model.blocks.end());
        if (blockIt != app.model.blocks.end()) {
            QElapsedTimer selectionTimer;
            selectionTimer.start();
            const qreal scale = firstPage->width() / 612.0;
            const QPointF start((blockIt->bounds.x + 2) * scale,
                                (blockIt->bounds.y + blockIt->bounds.height / 2) * scale);
            const QPointF finish((blockIt->bounds.x + blockIt->bounds.width - 2) * scale,
                                 start.y());
            QMouseEvent press(QEvent::MouseButtonPress, start,
                              firstPage->mapToGlobal(start.toPoint()), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent move(QEvent::MouseMove, finish,
                             firstPage->mapToGlobal(finish.toPoint()), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, finish,
                                firstPage->mapToGlobal(finish.toPoint()), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(firstPage, &press);
            QApplication::sendEvent(firstPage, &move);
            QApplication::sendEvent(firstPage, &release);
            CHECK(waitFor(qt, 1000, [&] {
                return !app.context.currentContext().temporary.empty();
            }));
            selectionContextMs = selectionTimer.elapsed();
            CHECK(selectionContextMs < 500);
            CHECK(QGuiApplication::clipboard()->text() == "clipboard sentinel");
            // The browser chat surface reflects the live selection locally:
            // whatever the drag resolved to must appear verbatim.
            qt.processEvents(QEventLoop::AllEvents, 50);
            selText = app.context.currentContext().temporary.empty()
                          ? QString()
                          : QString::fromStdString(
                                app.context.currentContext().temporary.front().extractedText)
                                .simplified();
            CHECK(!selText.isEmpty());
            CHECK(browserContext && browserContext->toPlainText().contains(selText.left(40)));
        }
    }

    // Copy-prompt assembles selection + question without any network.
    if (browser && !selText.isEmpty()) {
        browserQuestion->setText("What does this passage mean?");
        browser->copyPrompt();
        const QString prompt = QGuiApplication::clipboard()->text();
        CHECK(prompt.contains("What does this passage mean?"));
        CHECK(prompt.contains(selText.left(40)));
    }

    // Search is a real reader widget action, not a model-only shortcut.
    QMetaObject::invokeMethod(&window, "openReaderTools", Qt::DirectConnection);
    auto* query = window.findChild<QLineEdit*>("searchQuery");
    auto* results = window.findChild<QListWidget*>("searchResults");
    CHECK(query && results);
    if (query && results) {
        QElapsedTimer searchTimer;
        searchTimer.start();
        query->setText("history encoder");
        QMetaObject::invokeMethod(query, "returnPressed", Qt::DirectConnection);
        CHECK(waitFor(qt, 1000, [&] { return results->count() > 0; }));
        literalSearchMs = searchTimer.elapsed();
        CHECK(literalSearchMs < 500);
        if (results->count() > 0) {
            results->setCurrentRow(0);
            QMetaObject::invokeMethod(results, "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QListWidgetItem*, results->currentItem()));
        }
    }

    // Outline lists detected sections; activating one navigates the reader.
    auto* outline = window.findChild<OutlinePanel*>();
    CHECK(outline != nullptr);
    if (outline) {
        CHECK(waitFor(qt, 2000, [&] {
            auto* list = outline->findChild<QListWidget*>();
            return list && list->count() > 0;
        }));
        auto* list = outline->findChild<QListWidget*>();
        if (list && list->count() > 0) {
            list->setCurrentRow(0);
            QMetaObject::invokeMethod(list, "itemActivated", Qt::DirectConnection,
                                      Q_ARG(QListWidgetItem*, list->currentItem()));
            qt.processEvents(QEventLoop::AllEvents, 50);
        }
    }

    // PdfView consumes these shortcuts, so each signal must be wired to a
    // real MainWindow action rather than disappearing at the viewport.
    pdf->setFocus(Qt::OtherFocusReason);
    QKeyEvent findKey(QEvent::KeyPress, Qt::Key_F, Qt::ControlModifier);
    QApplication::sendEvent(pdf, &findKey);
    auto* readerTools = window.findChild<QWidget*>("readerToolsOverlay");
    CHECK(readerTools && readerTools->isVisible());
    // In-window overlay: hovers over the left edge without pushing the
    // document and without relying on window-manager positioning.
    CHECK(readerTools && readerTools->x() == 0);
    CHECK(readerTools && readerTools->width() > 200);
    // Window shortcuts drive the reading actions end to end: h applies the
    // highlight exactly once and arms space, which then focuses the
    // browser ask box. The highlight is removed again so later annotation
    // counts stay clean.
    window.activateWindow();
    pdf->setFocus(Qt::OtherFocusReason);
    qt.processEvents(QEventLoop::AllEvents, 20);
    const std::size_t annotationsBefore =
        app.annotations->annotationsFor(app.model.document.id).size();
    QKeyEvent highlightKey(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
    QApplication::sendEvent(pdf, &highlightKey);
    qt.processEvents(QEventLoop::AllEvents, 50);
    CHECK(app.annotations->annotationsFor(app.model.document.id).size() == annotationsBefore + 1);
    QKeyEvent spaceKey(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(&window, &spaceKey);
    qt.processEvents(QEventLoop::AllEvents, 50);
    CHECK(browserQuestion &&
          (browserQuestion->hasFocus() || QApplication::focusWidget() == browserQuestion));
    CHECK(pdf->removeHighlightForCurrentSelection());
    // Chatting is sacred: with focus inside the browser chat (ChatGPT's
    // own composer looks like the web view to Qt), space must NOT jump to
    // the ask box, and letter shortcuts must not fire either.
    if (auto* webView = window.findChild<QWebEngineView*>()) {
        webView->setFocus(Qt::OtherFocusReason);
        qt.processEvents(QEventLoop::AllEvents, 20);
        // Focus lands on the view's internal proxy: it must still be inside
        // the page (that is exactly what chatHasFocus sees).
        CHECK(webView->isAncestorOf(QApplication::focusWidget()));
        QKeyEvent chatSpace(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(webView, &chatSpace);
        qt.processEvents(QEventLoop::AllEvents, 50);
        // Stolen focus would land on the ask box, which is NOT inside the page.
        CHECK(QApplication::focusWidget() != browserQuestion);
        CHECK(webView->isAncestorOf(QApplication::focusWidget()));
        // 'n' with chat focus must not pop the note editor. Premise: the
        // earlier drag selection is still live, so an unguarded shortcut
        // would open the dialog.
        CHECK(pdf->hasLiveSelection());
        const std::size_t notesBefore =
            app.annotations->notesFor(app.model.document.id).size();
        QKeyEvent chatN(QEvent::KeyPress, Qt::Key_N, Qt::NoModifier);
        QApplication::sendEvent(webView, &chatN);
        qt.processEvents(QEventLoop::AllEvents, 50);
        CHECK(app.annotations->notesFor(app.model.document.id).size() == notesBefore);
        CHECK(window.findChild<QDialog*>("noteEditor") == nullptr);
    } else {
        CHECK(false); // browser chat view must exist
    }
    pdf->goToPage(2);
    app.state.history.clear();
    app.state.history.visit({0, 25, 1.1, std::nullopt});
    app.state.history.visit({2, 190, 1.65, std::nullopt});
    QMetaObject::invokeMethod(pdf, "historyBackRequested", Qt::DirectConnection);
    qt.processEvents(QEventLoop::AllEvents, 50);
    const auto restoredHistory = pdf->captureState();
    CHECK(restoredHistory.page == 0);
    CHECK(std::abs(restoredHistory.zoom - 1.1) < 0.001);
    CHECK(restoredHistory.scrollY == 25);

    // Rotation retains the exact anchor created by the real drag.
    CHECK(!app.context.currentContext().temporary.empty());
    reader::DocumentAnchor anchor = app.context.currentContext().temporary.empty()
                                        ? reader::DocumentAnchor{}
                                        : app.context.currentContext().temporary.front().anchor;
    pdf->setRotation(90);
    pdf->jumpToAnchor(anchor, true);
    CHECK(pdf->rotation() == 90);
    CHECK(pdf->captureState().selection.has_value());
    if (pdf->captureState().selection)
        CHECK(pdf->captureState().selection->anchorText == anchor.anchorText);
    // Pin the live reference for comparison, like the chat would.
    {
        const auto context = app.context.currentContext();
        if (!context.temporary.empty()) app.context.pinReference(context.temporary.front().id);
    }
    const auto pinned = app.context.currentContext();
    CHECK(pinned.temporary.empty());
    CHECK(pinned.pinned.size() == 1);
    CHECK(app.annotations->annotationsFor(app.model.document.id).empty());

    // Core rich-object flow uses real PDF clicks: equation, figure, table
    // and citation clicks become typed live context for the browser chat.
    {
        const auto context = app.context.currentContext();
        for (const auto& reference : context.temporary)
            app.context.removeReference(reference.id);
        for (const auto& reference : context.pinned)
            app.context.removeReference(reference.id);
        pdf->setRotation(0);
        pdf->goToPage(0);
        pdf->fitWidth();
        qt.processEvents(QEventLoop::AllEvents, 100);
        auto clickAnchor = [&](const reader::DocumentAnchor& object) {
            QWidget* page = window.findChild<QWidget*>(
                QString("pdfPage_%1").arg(object.page));
            CHECK(page != nullptr);
            if (!page) return;
            pdf->goToPage(object.page);
            const qreal scale = page->width() / 612.0;
            const QPointF point((object.bounds.x + object.bounds.width / 2) * scale,
                                (object.bounds.y + object.bounds.height / 2) * scale);
            QMouseEvent press(QEvent::MouseButtonPress, point,
                              page->mapToGlobal(point.toPoint()), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point,
                                page->mapToGlobal(point.toPoint()), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(page, &press);
            QApplication::sendEvent(page, &release);
            qt.processEvents(QEventLoop::AllEvents, 25);
        };
        CHECK(!app.model.equations.empty());
        CHECK(!app.model.figures.empty());
        CHECK(!app.model.tables.empty());
        CHECK(!app.model.citations.empty());
        if (!app.model.equations.empty()) {
            const auto equation = reader::anchorForEquation(app.model, app.model.equations.front());
            clickAnchor(equation);
            CHECK(!app.context.currentContext().temporary.empty());
            if (!app.context.currentContext().temporary.empty()) {
                const auto equationContext = app.context.currentContext();
                const auto& reference = equationContext.temporary.front();
                CHECK(reference.type == reader::ReferenceType::Equation);
                CHECK(reference.anchor.objectId == equation.objectId);
                CHECK(!reference.latex.empty());
            }
            const auto eqContext = app.context.currentContext();
            if (!eqContext.temporary.empty())
                app.context.pinReference(eqContext.temporary.front().id);
        }
        if (!app.model.figures.empty()) {
            const auto figure = reader::anchorForFigure(app.model, app.model.figures.front());
            clickAnchor(figure);
            const auto current = app.context.currentContext();
            CHECK(current.pinned.size() == 1);
            CHECK(!current.temporary.empty());
            if (!current.temporary.empty()) {
                CHECK(current.temporary.front().type == reader::ReferenceType::Figure);
                CHECK(current.temporary.front().anchor.objectId == figure.objectId);
            }
        }
        if (!app.model.tables.empty()) {
            const auto table = reader::anchorForTable(app.model, app.model.tables.front());
            clickAnchor(table);
            const auto current = app.context.currentContext();
            CHECK(!current.temporary.empty());
            if (!current.temporary.empty()) {
                CHECK(current.temporary.front().type == reader::ReferenceType::Table);
                CHECK(current.temporary.front().anchor.objectId == table.objectId);
                CHECK(current.temporary.front().tableRows == app.model.tables.front().rows);
            }
        }
        if (!app.model.citations.empty()) {
            const auto citation = reader::anchorForCitation(app.model, app.model.citations.front());
            clickAnchor(citation);
            const auto current = app.context.currentContext();
            CHECK(!current.temporary.empty());
            if (!current.temporary.empty()) {
                CHECK(current.temporary.front().type == reader::ReferenceType::Citation);
                CHECK(current.temporary.front().anchor.objectId == citation.objectId);
            }
        }
    }

    // Pane visibility is exercised through the actual slot.
    QMetaObject::invokeMethod(&window, "toggleAiPane", Qt::DirectConnection);
    QMetaObject::invokeMethod(&window, "toggleAiPane", Qt::DirectConnection);

    // Reopen is cache-safe and remains offline: the hash identity is stable.
    const auto identity = app.model.document.id;
    // Reading position persists across reopen: leave page 2 behind.
    pdf->goToPage(2);
    qt.processEvents(QEventLoop::AllEvents, 100);
    window.openFile(QString::fromStdString(pdfPath));
    CHECK(waitFor(qt, 8000, [&] { return app.model.document.id == identity; }));
    CHECK(waitFor(qt, 8000, [&] { return app.model.blocks.size() > 8; }));
    CHECK(waitFor(qt, 3000, [&] { return pdf->currentPage() == 2; }));
    CHECK(!app.context.currentContext().pinned.empty());

    // Switching to a distinct document clears every paper-A reference.
    // Same-document reopen above intentionally kept pinned context.
    window.openFile(QString::fromStdString(secondPdfPath));
    CHECK(waitFor(qt, 8000, [&] {
        return !app.model.document.id.empty() && app.model.document.id != identity;
    }));
    CHECK(waitFor(qt, 8000, [&] { return app.model.blocks.size() > 8; }));
    CHECK(app.context.currentContext().temporary.empty());
    CHECK(app.context.currentContext().pinned.empty());
    CHECK(!app.state.history.canBack());
    CHECK(pdf->currentPage() == 0);

    pdf->goToPage(0);
    pdf->fitWidth();
    qt.processEvents(QEventLoop::AllEvents, 100);
    window.grab().save("/tmp/reader-main-window.png");
    std::cout << "reader timings ms: first_paint=" << firstPaintMs
              << " literal_search=" << literalSearchMs
              << " selection_context=" << selectionContextMs << '\n';
    std::cout << "reader UI evidence screenshot: /tmp/reader-main-window.png\n";
    app.shutdown();
    window.hide();
    std::filesystem::remove_all(testHome);
    return failures == 0 ? 0 : 1;
}
