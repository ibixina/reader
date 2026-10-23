// Highlight autosave: saving writes pristine + highlights into the user's
// file without reloading the view, the paper keeps its identity (reading
// state, highlights, cached text) across reopen, removing every highlight
// restores the untouched bytes, and closing flushes a pending change.
// Skipped (exit 77) when qpdf/pdfinfo are unavailable.
#include "academic_fixture.h"
#include "app/Application.h"
#include "core/Types.h"
#include "pdf/HighlightExport.h"
#include "pdf/WordIndex.h"
#include "ui/MainWindow.h"
#include "ui/PdfView.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QToolBar>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <unistd.h>

namespace {

int failures = 0;
#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::cerr << "FAIL line " << __LINE__ << ": " #condition << '\n';           \
            ++failures;                                                                  \
        }                                                                                 \
    } while (false)

bool waitFor(QApplication& qt, int ms, const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > ms) return false;
        qt.processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

bool openAndWait(QApplication& qt, reader::Application& app, MainWindow& window,
                 const std::string& path) {
    window.resize(1100, 760);
    window.show();
    window.activateWindow();
    qt.processEvents(QEventLoop::AllEvents, 50);
    window.openFile(QString::fromStdString(path));
    return waitFor(qt, 15000, [&] {
        return !app.model.document.id.empty() && app.model.blocks.size() > 8;
    });
}

// Drag across the fixture's opening words and press-highlight them.
bool highlightOpeningWords(QApplication& qt, const std::string& path, MainWindow& window) {
    auto* pdf = window.findChild<PdfView*>();
    auto* page = window.findChild<QWidget*>("pdfPage_0");
    if (!pdf || !page) return false;
    pdf->goToPage(0);
    QString text;
    const auto words = reader::buildWordBoxesFromLayout(QString::fromStdString(path), 0, text);
    const auto first = std::find_if(words.begin(), words.end(),
                                    [](const auto& word) { return word.text == "Interactive"; });
    if (first == words.end()) return false;
    const auto last = first + std::min<std::ptrdiff_t>(2, words.end() - first - 1);
    if (!waitFor(qt, 5000, [&] { return pdf->captureState().zoom > 0; })) return false;
    const double scale = pdf->captureState().zoom;
    const QPointF start(qRound(first->rect.left() * scale), qRound(first->rect.center().y() * scale));
    const QPointF finish(qRound(last->rect.right() * scale), qRound(last->rect.center().y() * scale));
    // Word geometry is indexed in the background: retry until it is ready.
    return waitFor(qt, 8000, [&] {
        QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, finish, finish, Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(page, &press);
        QApplication::sendEvent(page, &release);
        return pdf->hasLiveSelection() && pdf->highlightCurrentSelection();
    });
}

QAction* saveAction(MainWindow& window) {
    auto* toolbar = window.findChild<QToolBar*>("readerToolbar");
    if (!toolbar) return nullptr;
    for (auto* action : toolbar->actions())
        if (action->text() == "Save") return action;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (QStandardPaths::findExecutable("qpdf").isEmpty() ||
        QStandardPaths::findExecutable("pdfinfo").isEmpty())
        return 77;
    char homeTemplate[] = "/tmp/reader-autosave-XXXXXX";
    char* testHome = mkdtemp(homeTemplate);
    if (!testHome) return 2;
    setenv("HOME", testHome, 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    unsetenv("OPENAI_API_KEY");
    QApplication qt(argc, argv);
    QCoreApplication::setOrganizationName("ReaderAutosave");
    QCoreApplication::setApplicationName("ReaderAutosave");

    const std::string pdfPath = std::string(testHome) + "/academic-fixture.pdf";
    reader_test::writeAcademicPdf(pdfPath);
    const std::string originalHash = reader::sha256File(pdfPath);
    CHECK(!originalHash.empty());

    // 1. Save writes into the file; the view is not reloaded.
    {
        reader::Application app;
        MainWindow window(&app);
        CHECK(openAndWait(qt, app, window, pdfPath));
        CHECK(app.model.document.id == originalHash);
        CHECK(highlightOpeningWords(qt, pdfPath, window));
        auto* autosave = window.findChild<QTimer*>("highlightAutosave");
        CHECK(autosave && autosave->isActive() && autosave->interval() == 20000);
        QWidget* pageBefore = window.findChild<QWidget*>("pdfPage_0");
        const std::size_t blocksBefore = app.model.blocks.size();
        QAction* save = saveAction(window);
        CHECK(save);
        if (save) save->trigger();
        CHECK(!autosave || !autosave->isActive());
        CHECK(waitFor(qt, 15000, [&] {
            return app.documents->savedFile(originalHash).has_value();
        }));
        const auto saved = app.documents->savedFile(originalHash);
        CHECK(saved && saved->savedHash == reader::sha256File(pdfPath));
        CHECK(saved && saved->savedHash != originalHash);
        CHECK(saved && reader::sha256File(saved->pristinePath) == originalHash);
        // Same widgets, same identity, same extracted text: nothing reloaded.
        qt.processEvents(QEventLoop::AllEvents, 100);
        CHECK(window.findChild<QWidget*>("pdfPage_0") == pageBefore);
        CHECK(app.model.document.id == originalHash);
        CHECK(app.model.blocks.size() == blocksBefore);
    }

    // 2. Reopen: same paper, same highlights; removing them all restores the
    // untouched bytes.
    {
        reader::Application app;
        MainWindow window(&app);
        CHECK(openAndWait(qt, app, window, pdfPath));
        CHECK(app.model.document.id == originalHash);
        const auto rows = app.annotations->annotationsFor(originalHash);
        CHECK(!rows.empty());
        for (const auto& row : rows) app.annotations->deleteAnnotation(originalHash, row.id);
        QAction* save = saveAction(window);
        if (save) save->trigger();
        // The record is written right after the file: wait for both.
        CHECK(waitFor(qt, 15000, [&] {
            const auto saved = app.documents->savedFile(originalHash);
            return saved && saved->rowsSignature.empty() && saved->savedHash == originalHash;
        }));
        CHECK(reader::sha256File(pdfPath) == originalHash);

        // 3. A change still pending when the window closes is written.
        CHECK(highlightOpeningWords(qt, pdfPath, window));
        auto* autosave = window.findChild<QTimer*>("highlightAutosave");
        CHECK(autosave && autosave->isActive());
    }
    {
        CHECK(reader::sha256File(pdfPath) != originalHash);
        reader::Application app;
        const auto saved = app.documents->savedFile(originalHash);
        CHECK(saved && !saved->rowsSignature.empty());
        CHECK(saved && saved->savedHash == reader::sha256File(pdfPath));
        // The bytes on disk are the untouched copy plus one highlight.
        CHECK(saved && reader::sha256File(saved->pristinePath) == originalHash);
    }

    std::filesystem::remove_all(testHome);
    if (failures == 0) std::cout << "ALL AUTOSAVE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
