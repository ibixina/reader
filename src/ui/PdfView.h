#pragma once
#include "document/DocumentAnchor.h"
#include "pdf/PdfRenderer.h"
#include "pdf/PdfSelection.h"
#include "pdf/PopplerBridge.h"
#include "pdf/PdfEngine.h"
#include "pdf/WordIndex.h"
#include "search/TextIndex.h"
#include "ai/References.h"
#include <QScrollArea>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QRectF>
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
class QDialog;
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
    // Unrotated page sizes in points, matching selection/highlight geometry.
    std::vector<QSizeF> pageSizes() const;
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
    std::vector<reader::PdfOutlineEntry> outlineEntries() const;
    void clearAllSelections();
    // Page-size accidental drags never become selections: at most
    // kMaxSelectionChars of dragged text is accepted as a live selection.
    static bool isSelectionTooLarge(const QString& text);
    // Explicit selection actions for the h/n/a reading shortcuts (§42).
    // Highlight toggles: the first press applies it exactly once no matter
    // how often it repeats, and pressing h again removes it.
    bool hasLiveSelection() const;
    bool hasHighlightForCurrentSelection() const;
    bool highlightCurrentSelection();
    bool removeHighlightForCurrentSelection();
    bool promptNoteForCurrentSelection();
    bool askAboutCurrentSelection();
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
    // Empty seed means the user pressed A on a live selection; focusing the
    // ask box is the whole action.
    void quickAskRequested(const QString& seed);
    void pageChanged(int page);
    void selectionGeometryReady(int page);
    void zoomChanged(double zoom);
    void viewStateChanged(int page, int scrollY, double zoom, int rotation);
    void findRequested();
    void sidecarToggleRequested();
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
    QPdfLink linkAt(int page, const QPointF& point) const;
    void promptNoteForAnchor(const reader::DocumentAnchor& anchor);
    // Screen position for the floating note editor: above the selection
    // when its geometry is known, otherwise at the cursor.
    QPoint noteEditorPos(const reader::DocumentAnchor& anchor, const QSize& size) const;
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
    std::optional<reader::DocumentAnchor> selection_;
    // Per-row rects (PDF points) of the live selection, stashed at drag
    // time: the paint truth used when a highlight is saved, so multiline
    // highlights hug the selected rows instead of the bounding box.
    QList<QRectF> selectionRows_;
    bool pendingG_ = false;
    bool suppressNextClickClear_ = false;
    // Upper bound for one drag selection: about a long paragraph or two.
    // AI context truncates at 500 chars anyway; anything bigger is an
    // accidental page-size drag, never intent.
    static constexpr int kMaxSelectionChars = 2000;
    // Floating note editor currently open, if any. A popup, never modal:
    // click-away cancels, Save persists.
    QPointer<QDialog> noteEditor_ = nullptr;
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
