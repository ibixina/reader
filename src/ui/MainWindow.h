#pragma once
#include "app/ApplicationState.h"
#include "core/CancellationToken.h"
#include "core/Types.h"
#include "document/DocumentAnchor.h"
#include "pdf/QtPdfEngine.h"
#include <QMainWindow>
#include <memory>

class QSplitter;
class QLabel;
class QToolBar;
class QTabWidget;
class QPdfDocument;
class QtPdfEngine;
class QFrame;
class QResizeEvent;

namespace reader {
class Application;
struct NavEntry;
}
class PdfView;
class WebPanel;
class SearchPanel;
class OutlinePanel;
class QTimer;

// Minimal reader: PDF viewer + browser chat + outline/search overlay.
// Keeps viewer, fit/zoom, history, literal search, outline, notes,
// highlights, selection→browser-chat and shortcuts. Nothing else.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(reader::Application* app, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString& path);

private slots:
    void toggleAiPane();
    void saveHighlightsToPdf();

private:
    void setupShortcuts();
    void scheduleReadingStateSave();
    void saveReadingState();
    void navigateTo(const reader::NavEntry& entry);
    void navigateToAnchor(const reader::DocumentAnchor& anchor);
    void persistWindowLayout();
    void setupToolbarAutoHide();
    void updateToolbarAutoHide();
    void setToolbarPinned(bool pinned);
    void setupEdgeReveal();
    void updateEdgeReveal();
    // Reader tools live in a left in-window overlay: the edge reveal and
    // the manual toggle show the same hovering sidebar, which never pushes
    // the document and needs no window-manager positioning.
    void showReaderTools();
    void openReaderTools();
    void placeReaderOverlay();
    void resizeEvent(QResizeEvent* event) override;
    void selectReaderTab(const QString& name);
    void showSelectionHint();
    // Browser chat is the only AI chat: space/ask focus its question box.
    void focusBrowserQuestion();
    void focusBrowserQuestionWithSeed(const QString& seed);
    // h toggles the highlight for the live selection (apply once, press
    // again to remove). Ctrl+H does the same from anywhere, including
    // while typing: plain keys can never hijack the composer.
    void toggleHighlight();
    // Focus inside the browser chat (including ChatGPT's own composer,
    // which Qt sees as the web view): reading shortcuts must never steal
    // keystrokes from a conversation.
    bool chatHasFocus() const;
    bool eventFilter(QObject* watched, QEvent* event) override;
    reader::Application* app_;
    QSplitter* splitter_ = nullptr;
    PdfView* pdf_ = nullptr;
    WebPanel* web_ = nullptr;
    QFrame* readerOverlay_ = nullptr;
    SearchPanel* searchPanel_ = nullptr;
    OutlinePanel* outlinePanel_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QTimer* toolbarRevealTimer_ = nullptr;
    bool toolbarPinned_ = false;
    QTimer* edgeRevealTimer_ = nullptr;
    bool edgeRevealActive_ = false;
    QTabWidget* readerTabs_ = nullptr;
    QLabel* statusPage_ = nullptr;
    QLabel* statusHint_ = nullptr;
    std::shared_ptr<QPdfDocument> pdfDoc_;
    std::shared_ptr<QtPdfEngine> engine_;
    unsigned long openGeneration_ = 0;
    reader::CancellationToken documentToken_;
    bool restoringHistory_ = false;
    QTimer* readingStateTimer_ = nullptr;
    bool exportRunning_ = false;
    // Position to restore after an in-place save reopens the file.
    std::optional<reader::NavEntry> pendingPosition_;
};
