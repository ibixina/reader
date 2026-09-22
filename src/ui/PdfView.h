#pragma once
#include "document/DocumentAnchor.h"
#include "ai/EmbeddingProviderQt.h"
#include "pdf/PdfRenderer.h"
#include "pdf/PdfSelection.h"
#include "pdf/PopplerBridge.h"
#include "pdf/PdfEngine.h"
#include "pdf/WordIndex.h"
#include "search/TextIndex.h"
#include "search/VectorIndex.h"
#include "ai/References.h"
#include <QScrollArea>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

class QPdfDocument;
class QPdfLinkModel;
class QPdfLink;
class QKeyEvent;
class QResizeEvent;
class QTimer;

namespace reader {
class Application;
struct NavEntry;

struct PdfViewState {
    int page = 0;
    int scrollY = 0;
    double zoom = 1.25;
    int rotation = 0;
    bool pageMode = false;
    std::optional<DocumentAnchor> selection;
};
} // namespace reader

// Continuous PDF view (§4.1): scroll, zoom, fit, rotate, selection,
// links, overlays (AI + user, §5.9), floating toolbar (§38).
class PdfView : public QScrollArea {
    Q_OBJECT
public:
    explicit PdfView(reader::Application* app, QWidget* parent = nullptr);
    ~PdfView() override;
    void attachDocument(std::shared_ptr<QPdfDocument> doc, const QString& path);
    int pageCount() const;
    void requestThumbnail(int page, const QSize& size,
                          std::function<void(const QImage&)> callback);
    // Render a bounded object/region crop on the renderer lane. The callback
    // is delivered on the GUI thread and is generation-bound to the document.
    void requestAnchorImage(const reader::DocumentAnchor& anchor,
                            std::function<void(const QImage&)> callback);
    void configureSemantic(const reader::EmbeddingProviderQtConfig& config);
    std::optional<reader::SemanticSearchSnapshot> semanticSnapshot() const;
    void requestReferenceImage(
        const reader::ContextReference& reference,
        std::function<void(std::optional<reader::ReferenceImage>)> callback);
    void loadSemanticCacheAsync(std::function<void(bool, const QString&)> callback);
    void buildSemanticIndexAsync(
        std::size_t maxBlocks, std::function<void(bool, const QString&)> callback);
    void searchSemanticAsync(
        const std::string& query, std::size_t limit,
        std::function<void(std::vector<reader::VectorIndex::Hit>, const QString&)> callback);
    bool semanticReady() const;
    // external shared ownership: background index jobs may outlive a reopen
    void goToPage(int page);
    void jumpToAnchor(const reader::DocumentAnchor& anchor, bool highlight);
    void setHoverAnchor(const reader::DocumentAnchor& anchor);
    void clearHoverAnchor();
    std::optional<reader::DocumentAnchor> highlightedAnchor() const {
        return hasHighlight_ ? std::optional<reader::DocumentAnchor>(pendingHighlight_)
                             : std::nullopt;
    }
    const std::optional<reader::DocumentAnchor>& hoveredAnchor() const {
        return hoverAnchor_;
    }
    void setZoom(double z);
    void setRotation(int degrees);
    void rotate(int quarterTurns = 1);
    int rotation() const { return rotation_; }
    void setPageMode(bool enabled);
    bool pageMode() const { return pageMode_; }
    void fitWidth();
    void fitPage();
    int currentPage() const { return currentPage_; }
    reader::PdfViewState captureState() const;
    void restoreState(const reader::PdfViewState& state, bool highlight = true);
    std::vector<reader::TextIndex::Hit> searchLiteral(const std::string& query,
                                                      std::size_t limit = 20);
    std::vector<reader::VectorIndex::Hit> searchSemantic(const std::string& query,
                                                         std::size_t limit = 10);
    std::vector<reader::PdfOutlineEntry> outlineEntries() const;
    void clearAllSelections();
    // Repaint AI emphasis from the cached PaperAnalysis (§5.8/§5.9).
    void refreshAiOverlays();
    // Repaint saved user highlights from SQLite (§38).
    void refreshUserOverlays();
    // Index word geometry around a page on the serial doc lane.
    void prefetchAround(int page);
    // Debounced version for scroll-driven calls: waits for a scroll pause
    // and warms both word geometry and pixels. Firing eagerly on every
    // scrollbar tick queues unbounded seconds of per-word engine queries.
    void schedulePrefetchAround(int page);
    // Hand cached line fragments to existing page widgets.
    void distributeLines();

signals:
    void selectionChanged(const reader::DocumentAnchor& anchor);
    void objectClicked(const reader::DocumentAnchor& anchor, const QString& kind);
    void sourceActivated(const reader::DocumentAnchor& anchor);
    void linkActivated(int page, const QString& uri);
    void bookmarkRequested(const reader::DocumentAnchor& anchor);
    void regionCaptured(const reader::DocumentAnchor& anchor, const QImage& image);
    void askAiRequested();
    // Single-key quick ask from the selection menu. seed is empty when the
    // user pressed A, otherwise the first typed character that should land
    // in the ask box.
    void quickAskRequested(const QString& seed);
    void pageChanged(int page);
    void selectionGeometryReady(int page);
    void zoomChanged(double zoom);
    void viewStateChanged(int page, int scrollY, double zoom, int rotation);
    void findRequested();
    void commandPaletteRequested();
    void sidecarToggleRequested();
    void tabRequested(int index);
    void historyBackRequested();
    void historyForwardRequested();

private:
    void resizeEvent(QResizeEvent* event) override;
    void applyFitMode();
    void rebuildPages();
    void updateCurrentPage();
    void prefetchPixelsAround(int page);
    void schedulePixelPrefetch(int page);
    QWidget* pageWidget(int page) const;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    QPdfLink linkAt(int page, const QPointF& point) const;
    void showSelectionMenu(const reader::DocumentAnchor& anchor);
    reader::Application* app_;
    std::shared_ptr<QPdfDocument> doc_;
    // Page count/sizes cached once at attach time. QPdfDocument serializes
    // every query (pagePointSize included) on an internal mutex that the
    // serial doc lane can hold for ~1s per dense page; touching it on the
    // GUI thread during scrolling froze the app, so the scroll path must
    // never call into it (proven by backtrace: UI blocked in
    // QPdfDocument::pagePointSize behind a word-index job).
    int pageCount_ = 0;
    std::vector<QSizeF> pageSizes_;
    std::shared_ptr<std::atomic<unsigned long>> docGen_;
    std::shared_ptr<reader::SelectionIndex> selIndex_;
    QWidget* pageHost_ = nullptr;
    std::unique_ptr<reader::PdfRenderer> renderer_;
    double zoom_ = 1.25;
    int rotation_ = 0;
    int currentPage_ = 0;
    bool pageMode_ = false;
    enum class FitMode { None, Width, Page } fitMode_ = FitMode::None;
    bool applyingFit_ = false;
    reader::DocumentAnchor pendingHighlight_;
    bool hasHighlight_ = false;
    std::optional<reader::DocumentAnchor> hoverAnchor_;
    std::shared_ptr<PopplerBridge> raster_;
    std::unique_ptr<QPdfLinkModel> linkModel_;
    reader::TextIndex textIndex_;
    reader::VectorIndex vectorIndex_;
    struct SemanticState {
        mutable std::mutex mutex;
        reader::VectorIndex index;
        std::shared_ptr<reader::EmbeddingProviderQt> provider;
    };
    std::shared_ptr<SemanticState> semanticState_ = std::make_shared<SemanticState>();
    reader::CancellationToken semanticToken_;
    std::optional<reader::DocumentAnchor> selection_;
    bool pendingG_ = false;
    bool suppressNextClickClear_ = false;
    QTimer* prefetchTimer_ = nullptr;
    int pendingPrefetchPage_ = -1;
    // Latest scroll-driven prefetch target. Word jobs compare against it at
    // start and drop themselves when the user has scrolled far past them.
    std::atomic<int> latestPrefetchPage_{-1};
    // Pages with a word-index job already queued or running. Without this,
    // every pageChanged during a scroll enqueues 5 more ~1s jobs and the
    // single doc lane never drains, pinning a core and starving the UI.
    std::mutex wordPendingMutex_;
    std::unordered_set<int> wordPending_;
};
