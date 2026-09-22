#pragma once

#include <QWidget>

class QListWidget;

namespace reader {
class Application;
}
class PdfView;

// Asynchronous page thumbnail strip. PdfView owns the renderer/cache; this
// widget only requests thumbnails and emits page numbers to its host.
class ThumbnailPanel : public QWidget {
    Q_OBJECT
public:
    explicit ThumbnailPanel(reader::Application* app, PdfView* view,
                            QWidget* parent = nullptr);
    void rebuild();

signals:
    void pageActivated(int page);

private:
    reader::Application* app_;
    PdfView* view_;
    QListWidget* pages_ = nullptr;
    // A second rebuild (open + post-extract) orphans the first batch's
    // staggered timers; they must not re-request into the new list.
    int rebuildGeneration_ = 0;
};

