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
class QCloseEvent;
class QShowEvent;
class QHideEvent;
class QStackedWidget;
class QToolButton;
class QColor;
class QIcon;

namespace reader {
class Application;
struct NavEntry;
}
class PdfView;
class WebPanel;
class SearchPanel;
class OutlinePanel;
class MarksPanel;
class QTimer;
class DocumentShelf;

// Folder-backed document shelf and reader with browser chat and tools.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(reader::Application* app, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString& path);
    void showShelf();

private slots:
    void toggleAiPane();
    // wait: block until written (exit). manual: report "already saved" too.
    void saveHighlights(bool wait, bool manual = false);
    void scheduleHighlightSave();
    // b toggles a bookmark on the current page: saved as a top-strip
    // anchor (painted as a dot by the overlay), press again to remove.
    void toggleBookmark();

private:
    void setupShortcuts();
    bool readerVisible() const;
    void scheduleReadingStateSave();
    void saveReadingState();
    void navigateTo(const reader::NavEntry& entry);
    void navigateBack();
    void navigateForward();
    void navigateToAnchor(const reader::DocumentAnchor& anchor, bool highlight = true);
    void persistWindowLayout();
    void setupToolbarAutoHide();
    void updateToolbarAutoHide();
    void setToolbarPinned(bool pinned);
    void setupEdgeReveal();
    void updateEdgeReveal();
    // Hover polling (toolbar, edge, chat nav) runs only while the reader
    // is on screen in the active window: an idle, minimized or background
    // window must not wake the CPU several times a second.
    void updatePollTimers();
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    // Reader tools live in a left in-window overlay: the edge reveal and
    // the manual toggle show the same hovering sidebar, which never pushes
    // the document and needs no window-manager positioning.
    void showReaderTools();
    void showNotes();
    void openReaderTools();
    void placeReaderOverlay();
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void selectReaderTab(const QString& name);
    void showSelectionHint();
    // Browser chat is the only AI chat: space/ask focus its question box.
    void focusBrowserQuestion();
    void focusBrowserQuestionWithSeed(const QString& seed);
    // h toggles the highlight for the live selection (apply once, press
    // again to remove). Ctrl+H does the same from anywhere, including
    // while typing: plain keys can never hijack the composer.
    void toggleHighlight();
    // Toolbar color picker: new highlights use it, and a live selection
    // over an existing highlight is recolored. Persisted in QSettings.
    void setHighlightColor(const QColor& color);
    static QIcon colorSwatch(const QColor& color);
    // Focus inside the browser chat (including Claude's own composer,
    // which Qt sees as the web view): reading shortcuts must never steal
    // keystrokes from a conversation.
    bool chatHasFocus() const;
    bool eventFilter(QObject* watched, QEvent* event) override;
    reader::Application* app_;
    QStackedWidget* pages_ = nullptr;
    DocumentShelf* shelf_ = nullptr;
    QSplitter* splitter_ = nullptr;
    PdfView* pdf_ = nullptr;
    WebPanel* web_ = nullptr;
    QFrame* readerOverlay_ = nullptr;
    SearchPanel* searchPanel_ = nullptr;
    OutlinePanel* outlinePanel_ = nullptr;
    MarksPanel* marksPanel_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QToolButton* highlightColorButton_ = nullptr;
    QTimer* toolbarRevealTimer_ = nullptr;
    bool toolbarPinned_ = false;
    QTimer* edgeRevealTimer_ = nullptr;
    QTabWidget* readerTabs_ = nullptr;
    QLabel* statusPage_ = nullptr;
    QLabel* statusHint_ = nullptr;
    std::shared_ptr<QPdfDocument> pdfDoc_;
    std::shared_ptr<QtPdfEngine> engine_;
    unsigned long openGeneration_ = 0;
    reader::CancellationToken documentToken_;
    bool restoringHistory_ = false;
    QTimer* readingStateTimer_ = nullptr;
    QTimer* autosaveTimer_ = nullptr;
    static constexpr int kAutosaveDelayMs = 20 * 1000;
    // Size + mtime of the open file when it was opened: a save refuses to
    // overwrite a file changed outside the reader since.
    qint64 openedSize_ = 0;
    qint64 openedMtime_ = 0;
};
