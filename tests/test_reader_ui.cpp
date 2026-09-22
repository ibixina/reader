#include "app/Application.h"
#include "ai/LlmProvider.h"
#include "document/DocumentAnchor.h"
#include "academic_fixture.h"
#include "ui/CommandPalette.h"
#include "ui/MainWindow.h"
#include "ui/MapPanel.h"
#include "ui/PdfView.h"
#include "ui/SummaryPanel.h"
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QDockWidget>
#include <QGraphicsEllipseItem>
#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTextBrowser>
#include <QUrl>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <thread>

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

class CountingOfflineProvider final : public reader::LlmProvider {
public:
    struct Observation {
        std::atomic<int> references{0};
        std::atomic<bool> equation{false};
        std::atomic<bool> figure{false};
        std::atomic<bool> figureImage{false};
    };

    CountingOfflineProvider(std::atomic<int>* calls, Observation* observation)
        : calls_(calls), observation_(observation) {}
    std::string name() const override { return "counting-offline"; }
    void streamChat(const reader::ChatRequest& request,
                    reader::StreamCallbacks callbacks) override {
        ++*calls_;
        observation_->references = static_cast<int>(request.explicitReferences.size());
        observation_->equation = false;
        observation_->figure = false;
        observation_->figureImage = false;
        for (const auto& reference : request.explicitReferences) {
            if (reference.type == reader::ReferenceType::Equation)
                observation_->equation = true;
            if (reference.type == reader::ReferenceType::Figure) {
                observation_->figure = true;
                observation_->figureImage = reference.image.has_value() &&
                                            !reference.image->bytes.empty();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::string answer = "Grounded offline answer.";
        if (!request.explicitReferences.empty())
            answer += " [" + reader::contextReferenceId(request.explicitReferences.front()) + "]";
        if (callbacks.onToken) callbacks.onToken(answer);
        if (callbacks.onSource && !request.explicitReferences.empty())
            callbacks.onSource(request.explicitReferences.front().anchor);
        if (callbacks.onDone) callbacks.onDone(answer);
    }

private:
    std::atomic<int>* calls_;
    Observation* observation_;
};

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
    app.providerConfig.kind = "offline";
    app.providerConfig.apiKey.clear();
    std::atomic<int> providerCalls{0};
    CountingOfflineProvider::Observation providerObservation;
    MainWindow window(&app);
    app.chatManager->setProvider(
        std::make_unique<CountingOfflineProvider>(&providerCalls, &providerObservation));
    window.resize(1100, 760);
    window.show();
    QElapsedTimer firstPaintTimer;
    firstPaintTimer.start();
    window.openFile(QString::fromStdString(pdfPath));

    CHECK(waitFor(qt, 5000, [&] { return !app.model.document.id.empty(); }));
    CHECK(waitFor(qt, 8000, [&] { return app.model.blocks.size() > 8; }));
    CHECK(app.model.document.pageCount == 4);
    CHECK(!app.documents->recentDocuments(10).empty());
    CHECK(providerCalls.load() == 0);

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
    qint64 delayedAiJumpMs = -1;
    qint64 literalSearchMs = -1;
    qint64 fakeProviderFirstTokenMs = -1;

    // An actual drag on the rendered page updates structured context while
    // preserving the in-progress composer and clipboard. It must not send a
    // request simply because a selection changed.
    auto* chatInput = window.findChild<QLineEdit*>("chatInput");
    QWidget* firstPage = window.findChild<QWidget*>("pdfPage_0");
    CHECK(chatInput && firstPage);
    if (chatInput && firstPage) {
        chatInput->setText("draft question");
        chatInput->setFocus(Qt::OtherFocusReason);
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
            CHECK(chatInput->text() == "draft question");
            CHECK(waitFor(qt, 200, [&] { return chatInput->hasFocus(); }));
            CHECK(QGuiApplication::clipboard()->text() == "clipboard sentinel");
            CHECK(providerCalls.load() == 0);
        }
        for (QWidget* top : QApplication::topLevelWidgets())
            if (qobject_cast<QMenu*>(top)) top->close();
    }

    auto* sendButton = window.findChild<QPushButton*>("sendButton");
    CHECK(sendButton != nullptr);
    if (chatInput && sendButton && !app.context.currentContext().temporary.empty()) {
        chatInput->setText("What does this selected passage mean?");
        sendButton->click();
        CHECK(waitFor(qt, 200, [&] { return !sendButton->isEnabled(); }));
        QElapsedTimer delayedAiJump;
        delayedAiJump.start();
        pdf->goToPage(1);
        pdf->goToPage(0);
        qt.processEvents(QEventLoop::AllEvents, 25);
        delayedAiJumpMs = delayedAiJump.elapsed();
        CHECK(delayedAiJumpMs < 250);
        CHECK(waitFor(qt, 2000, [&] { return providerCalls.load() == 1; }));
        CHECK(waitFor(qt, 2000, [&] { return sendButton->isEnabled(); }));
        pdf->fitWidth();
        const auto selectedContext = app.context.currentContext();
        if (!selectedContext.temporary.empty())
            pdf->jumpToAnchor(selectedContext.temporary.front().anchor, true);
        QElapsedTimer chatRender;
        chatRender.start();
        while (chatRender.elapsed() < 400) {
            qt.processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
        window.grab().save("/tmp/reader-chat-flow.png");
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

    // The palette emits a real command and MainWindow executes it.
    CommandPalette palette(&app, &window);
    QObject::connect(&palette, &CommandPalette::commandChosen, &window,
                     [&](const QString& command) {
                         QMetaObject::invokeMethod(&window, "executeCommand",
                                                   Qt::DirectConnection,
                                                   Q_ARG(QString, command));
                     });
    auto* paletteList = palette.findChild<QListWidget*>();
    CHECK(paletteList && paletteList->count() > 0);
    if (paletteList) {
        for (int i = 0; i < paletteList->count(); ++i) {
            if (paletteList->item(i)->text() == "go to methods") {
                paletteList->setCurrentRow(i);
                QMetaObject::invokeMethod(paletteList, "itemActivated", Qt::DirectConnection,
                                          Q_ARG(QListWidgetItem*, paletteList->item(i)));
                break;
            }
        }
    }
    CHECK(pdf->currentPage() == 1);

    // PdfView consumes these shortcuts, so each signal must be wired to a
    // real MainWindow action rather than disappearing at the viewport.
    pdf->setFocus(Qt::OtherFocusReason);
    QKeyEvent findKey(QEvent::KeyPress, Qt::Key_F, Qt::ControlModifier);
    QApplication::sendEvent(pdf, &findKey);
    auto* readerDock = window.findChild<QDockWidget*>();
    CHECK(readerDock && readerDock->isVisible());
    window.activateWindow();
    QMetaObject::invokeMethod(pdf, "askAiRequested", Qt::DirectConnection);
    qt.processEvents(QEventLoop::AllEvents, 50);
    CHECK(chatInput && (chatInput->hasFocus() || QApplication::focusWidget() == chatInput));
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
    QMetaObject::invokeMethod(&window, "executeCommand", Qt::DirectConnection,
                              Q_ARG(QString, QString("pin current selection")));
    const auto pinned = app.context.currentContext();
    CHECK(pinned.temporary.empty());
    CHECK(pinned.pinned.size() == 1);
    CHECK(app.annotations->annotationsFor(app.model.document.id).empty());

    // Command actions populate meaningful questions and structured scope.
    QMetaObject::invokeMethod(&window, "executeCommand", Qt::DirectConnection,
                              Q_ARG(QString, QString("explain current section")));
    CHECK(chatInput && chatInput->text().contains("Explain the current section"));
    CHECK(!app.context.currentContext().temporary.empty());
    if (!app.context.currentContext().temporary.empty())
        CHECK(app.context.currentContext().temporary.front().type ==
              reader::ReferenceType::Section);
    QMetaObject::invokeMethod(&window, "executeCommand", Qt::DirectConnection,
                              Q_ARG(QString, QString("summarize current page")));
    CHECK(chatInput && chatInput->text().contains("Summarize page"));
    if (!app.context.currentContext().temporary.empty())
        CHECK(app.context.currentContext().temporary.front().type == reader::ReferenceType::Page);

    // Core rich-object flow uses real PDF clicks: equation is pinned, figure
    // becomes the live comparison reference with an async raster crop, then
    // table and citation clicks preserve their typed payloads.
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
        std::optional<reader::DocumentAnchor> equationAnchor;
        if (!app.model.equations.empty()) {
            const auto equation = reader::anchorForEquation(app.model, app.model.equations.front());
            equationAnchor = equation;
            clickAnchor(equation);
            CHECK(!app.context.currentContext().temporary.empty());
            if (!app.context.currentContext().temporary.empty()) {
                const auto equationContext = app.context.currentContext();
                const auto& reference = equationContext.temporary.front();
                CHECK(reference.type == reader::ReferenceType::Equation);
                CHECK(reference.anchor.objectId == equation.objectId);
                CHECK(!reference.latex.empty());
            }
            QMetaObject::invokeMethod(&window, "executeCommand", Qt::DirectConnection,
                                      Q_ARG(QString, QString("pin current selection")));
        }
        if (!app.model.figures.empty()) {
            const auto figure = reader::anchorForFigure(app.model, app.model.figures.front());
            clickAnchor(figure);
            CHECK(waitFor(qt, 2000, [&] {
                const auto current = app.context.currentContext();
                return !current.temporary.empty() && current.temporary.front().image.has_value();
            }));
            const auto current = app.context.currentContext();
            CHECK(current.pinned.size() == 1);
            CHECK(!current.temporary.empty());
            if (!current.temporary.empty()) {
                CHECK(current.temporary.front().type == reader::ReferenceType::Figure);
                CHECK(current.temporary.front().anchor.objectId == figure.objectId);
            }
            if (chatInput && sendButton) {
                auto* transcript = window.findChild<QTextBrowser*>("chatThread");
                const int previousAnswers = transcript
                    ? transcript->toPlainText().count("Grounded offline answer.")
                    : 0;
                chatInput->setText("Compare the pinned equation with this figure.");
                QElapsedTimer firstTokenTimer;
                firstTokenTimer.start();
                sendButton->click();
                CHECK(waitFor(qt, 2500, [&] {
                    return transcript &&
                           transcript->toPlainText().count("Grounded offline answer.") >
                               previousAnswers;
                }));
                fakeProviderFirstTokenMs = firstTokenTimer.elapsed();
                CHECK(waitFor(qt, 1000, [&] { return sendButton->isEnabled(); }));
                CHECK(providerCalls.load() == 2);
                CHECK(providerObservation.references.load() == 2);
                CHECK(providerObservation.equation.load());
                CHECK(providerObservation.figure.load());
                CHECK(providerObservation.figureImage.load());

                // Exercise the rendered source link itself. Hover previews the
                // exact equation bounds; clicking navigates and promotes the
                // same anchor to the persistent source highlight.
                if (transcript) {
                    QPoint sourcePoint(-1, -1);
                    for (int y = 0; y < transcript->viewport()->height() && sourcePoint.x() < 0;
                         y += 2) {
                        for (int x = 0; x < transcript->viewport()->width(); x += 2) {
                            if (transcript->anchorAt({x, y}) == "reader-source:1") {
                                sourcePoint = {x, y};
                                break;
                            }
                        }
                    }
                    CHECK(sourcePoint.x() >= 0);
                    if (sourcePoint.x() >= 0) {
                        QMouseEvent hover(QEvent::MouseMove, sourcePoint,
                                          transcript->viewport()->mapToGlobal(sourcePoint),
                                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(transcript->viewport(), &hover);
                        CHECK(pdf->hoveredAnchor().has_value());
                        if (pdf->hoveredAnchor() && equationAnchor)
                            CHECK(pdf->hoveredAnchor()->objectId == equationAnchor->objectId);
                        QMouseEvent press(QEvent::MouseButtonPress, sourcePoint,
                                          transcript->viewport()->mapToGlobal(sourcePoint),
                                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                        QMouseEvent release(QEvent::MouseButtonRelease, sourcePoint,
                                            transcript->viewport()->mapToGlobal(sourcePoint),
                                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(transcript->viewport(), &press);
                        QApplication::sendEvent(transcript->viewport(), &release);
                        qt.processEvents(QEventLoop::AllEvents, 50);
                        CHECK(pdf->highlightedAnchor().has_value());
                        if (pdf->highlightedAnchor() && equationAnchor)
                            CHECK(pdf->highlightedAnchor()->objectId == equationAnchor->objectId);
                    }
                }
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

    // Pane visibility/collapse state is exercised through the actual slots.
    QMetaObject::invokeMethod(&window, "toggleAiPane", Qt::DirectConnection);
    QMetaObject::invokeMethod(&window, "toggleAiPane", Qt::DirectConnection);
    QMetaObject::invokeMethod(&window, "toggleAiCollapse", Qt::DirectConnection);
    QMetaObject::invokeMethod(&window, "toggleAiCollapse", Qt::DirectConnection);

    // Reopen is cache-safe and remains offline: the hash identity is stable
    // and no provider is configured by this test.
    const auto identity = app.model.document.id;
    window.openFile(QString::fromStdString(pdfPath));
    CHECK(waitFor(qt, 8000, [&] { return app.model.document.id == identity; }));
    CHECK(waitFor(qt, 8000, [&] { return app.model.blocks.size() > 8; }));
    CHECK(!app.context.currentContext().pinned.empty());

    // Switching to a distinct document cancels pending chat/crops and clears
    // every paper-A reference. Same-document reopen above intentionally kept
    // pinned comparison context.
    std::atomic<bool> staleImagePublished{false};
    if (!app.model.figures.empty()) {
        reader::ContextReference figure;
        figure.type = reader::ReferenceType::Figure;
        figure.anchor = reader::anchorForFigure(app.model, app.model.figures.front());
        pdf->requestReferenceImage(figure, [&](std::optional<reader::ReferenceImage>) {
            staleImagePublished.store(true);
        });
    }
    if (chatInput && sendButton) {
        chatInput->setText("This paper-A request must be cancelled on document switch.");
        sendButton->click();
        CHECK(waitFor(qt, 200, [&] { return !sendButton->isEnabled(); }));
    }
    window.openFile(QString::fromStdString(secondPdfPath));
    CHECK(waitFor(qt, 8000, [&] {
        return !app.model.document.id.empty() && app.model.document.id != identity;
    }));
    CHECK(waitFor(qt, 8000, [&] { return app.model.blocks.size() > 8; }));
    CHECK(app.context.currentContext().temporary.empty());
    CHECK(app.context.currentContext().pinned.empty());
    CHECK(!app.state.history.canBack());
    CHECK(pdf->currentPage() == 0);
    CHECK(waitFor(qt, 1500, [&] { return sendButton->isEnabled(); }));
    CHECK(!staleImagePublished.load());

    // Cached reopen and local ingest remain network-free. The populated
    // Summary tab plus readable PDF are captured as end-to-end evidence.
    CHECK(providerCalls.load() == 2);
    QMetaObject::invokeMethod(&window, "runIngest", Qt::DirectConnection,
                              Q_ARG(bool, false));
    CHECK(waitFor(qt, 8000, [&] { return app.analysis && app.analysis->usable(); }));
    auto* summaryPanel = window.findChild<SummaryPanel*>();
    auto* mapPanel = window.findChild<MapPanel*>();
    auto* summaryView = window.findChild<QTextBrowser*>("summaryView");
    CHECK(summaryPanel && mapPanel && summaryView);
    if (summaryPanel) summaryPanel->rebuild();
    CHECK(summaryView && summaryView->toPlainText().contains("Local extractive analysis"));

    // Full deterministic manifest exercises every Summary/Map surface while
    // the preceding assertions keep local ingest as a real smoke test.
    app.analysis = reader_test::academicAnalysis(app.model);
    if (summaryPanel) summaryPanel->rebuild();
    if (mapPanel) mapPanel->rebuild();
    pdf->setRotation(0);
    pdf->goToPage(0);
    pdf->fitWidth();
    auto* tabs = window.findChild<QTabWidget*>();
    CHECK(tabs != nullptr);
    if (tabs) {
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Summary") tabs->setCurrentIndex(i);
    }
    app.state.settings.showMethods = true;
    if (auto* depth = window.findChild<QComboBox*>("summaryDepth"))
        depth->setCurrentText("Technical");
    if (auto* sections = window.findChild<QComboBox*>("sectionSummaryDepth"))
        sections->setCurrentText("Detailed");
    QElapsedTimer renderWait;
    renderWait.start();
    while (renderWait.elapsed() < 800) {
        qt.processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }

    window.grab().save("/tmp/reader-main-window.png");
    CHECK(summaryView && summaryView->toHtml().contains("block:"));
    const auto methodBlock = std::find_if(
        app.model.blocks.begin(), app.model.blocks.end(), [](const reader::TextBlock& block) {
            return block.text.find("Selection is represented") != std::string::npos;
        });
    CHECK(methodBlock != app.model.blocks.end());
    if (summaryView && methodBlock != app.model.blocks.end()) {
        const QUrl source("block:" + QString::fromStdString(methodBlock->id));
        CHECK(summaryView->toHtml().contains(QString::fromStdString(methodBlock->id)));
        QMetaObject::invokeMethod(summaryView, "anchorClicked", Qt::DirectConnection,
                                  Q_ARG(QUrl, source));
        CHECK(pdf->currentPage() == methodBlock->page);
    }
    if (tabs) {
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == "Map") tabs->setCurrentIndex(i);
    }
    qt.processEvents(QEventLoop::AllEvents, 100);
    auto* mapView = window.findChild<QGraphicsView*>("conceptMapView");
    auto* conceptDetail = window.findChild<QLabel*>("conceptDetail");
    CHECK(mapView && !mapView->scene()->items().isEmpty());
    if (mapView) {
        for (QGraphicsItem* item : mapView->scene()->items()) {
            if (qgraphicsitem_cast<QGraphicsEllipseItem*>(item)) {
                item->setSelected(true);
                break;
            }
        }
    }
    qt.processEvents(QEventLoop::AllEvents, 50);
    CHECK(conceptDetail && conceptDetail->text().contains("block:"));
    if (conceptDetail && methodBlock != app.model.blocks.end()) {
        QMetaObject::invokeMethod(
            conceptDetail, "linkActivated", Qt::DirectConnection,
            Q_ARG(QString, "block:" + QString::fromStdString(methodBlock->id)));
        CHECK(pdf->currentPage() == methodBlock->page);
    }
    pdf->goToPage(0);
    pdf->fitWidth();
    QElapsedTimer mapRenderWait;
    mapRenderWait.start();
    while (mapRenderWait.elapsed() < 500) {
        qt.processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    window.grab().save("/tmp/reader-map.png");
    std::cout << "reader timings ms: first_paint=" << firstPaintMs
              << " delayed_ai_page_round_trip=" << delayedAiJumpMs
              << " literal_search=" << literalSearchMs
              << " selection_context=" << selectionContextMs
              << " fake_provider_first_token=" << fakeProviderFirstTokenMs << '\n';
    std::cout << "reader UI evidence screenshot: /tmp/reader-main-window.png\n";
    app.shutdown();
    window.hide();
    std::filesystem::remove_all(testHome);
    return failures == 0 ? 0 : 1;
}
