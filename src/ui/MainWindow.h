#pragma once
#include "analysis/PaperIngestor.h"
#include "ai/LlmProvider.h"
#include "pdf/QtPdfEngine.h"
#include <QMainWindow>
#include <cstdint>
#include <memory>
#include <vector>
class QSplitter;
class QTabWidget;
class QPushButton;
class QLabel;
class QMenu;
class QToolBar;
class QPdfDocument;
class QtPdfEngine;

namespace reader {
class Application;
struct NavEntry;
}
class PdfView;
class ChatPanel;
class WebPanel;
class MapPanel;
class SummaryPanel;
class IngestRawPanel;
class SearchPanel;
class OutlinePanel;
class ThumbnailPanel;
class QDockWidget;
class QTimer;

// Two synchronized panes (§3): PDF reader + AI tabs, draggable divider,
// collapsible/detachable AI pane (Ctrl+Shift+A), ingest control (§5).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(reader::Application* app, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString& path);

private slots:
    void runIngest(bool reingest = false);
    void onIngestProgress(const reader::IngestProgress& p);
    void toggleAiPane();
    void toggleAiCollapse();
    void toggleAiDetach();
    void openReaderTools();
    void executeCommand(const QString& command);
#ifdef HAVE_WEBENGINE
    void runChatIngest();
    void onChatIngestResponse(const QString& responseText, quint64 requestId);
#endif

private:
    void setupShortcuts();
    void updateIngestButton();
    void showIngestOutput();
    void scheduleReadingStateSave();
    void saveReadingState();
    void navigateTo(const reader::NavEntry& entry);
    void navigateToAnchor(const reader::DocumentAnchor& anchor);
    void openLibrary();
    void openAnnotationManager();
    void openSettings();
    void persistWindowLayout();
    void setupToolbarAutoHide();
    void updateToolbarAutoHide();
    void setToolbarPinned(bool pinned);
    void setupEdgeReveal();
    void updateEdgeReveal();
#ifdef HAVE_WEBENGINE
    WebPanel* ensureWebPanel();
    void openWebChat();
#endif
    reader::Application* app_;
    QSplitter* splitter_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    PdfView* pdf_ = nullptr;
    ChatPanel* chat_ = nullptr;
    WebPanel* web_ = nullptr;
    MapPanel* map_ = nullptr;
    SummaryPanel* summary_ = nullptr;
    IngestRawPanel* ingestRaw_ = nullptr;
    QDockWidget* readerDock_ = nullptr;
    SearchPanel* searchPanel_ = nullptr;
    OutlinePanel* outlinePanel_ = nullptr;
    ThumbnailPanel* thumbnailPanel_ = nullptr;
    QPushButton* ingestButton_ = nullptr;
    QMenu* ingestMenu_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QTimer* toolbarRevealTimer_ = nullptr;
    bool toolbarPinned_ = false;
    QTimer* edgeRevealTimer_ = nullptr;
    bool edgeRevealActive_ = false;
    QTabWidget* readerTabs_ = nullptr;
    QLabel* statusPage_ = nullptr;
    QLabel* statusAi_ = nullptr;
    reader::IngestState ingestState_ = reader::IngestState::NotIngested;
    std::shared_ptr<QPdfDocument> pdfDoc_;
    std::shared_ptr<QtPdfEngine> engine_;
    bool docReady_ = false;
    unsigned long openGeneration_ = 0;
    reader::CancellationToken ingestToken_;
    reader::CancellationToken documentToken_;
    bool restoringHistory_ = false;
    bool aiPaneLeft_ = false;
    bool aiPaneDetached_ = false;
    std::vector<int> expandedSplitterSizes_;
    std::shared_ptr<reader::LlmProvider> activeIngestProvider_;
    QTimer* readingStateTimer_ = nullptr;
#ifdef HAVE_WEBENGINE
    quint64 chatIngestRequestId_ = 0;
    std::string chatIngestHash_;
#endif
};
