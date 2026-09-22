#pragma once

#include "document/DocumentAnchor.h"
#include <QWidget>
#include <vector>

class QLineEdit;
class QListWidget;
class QLabel;

namespace reader {
class Application;
}
class PdfView;

// Reader-owned literal search surface. Emits anchors instead of reaching
// into MainWindow, so the host decides how to preserve history and
// highlight the selected source. Literal only: instant, offline, zero
// background work.
class SearchPanel : public QWidget {
    Q_OBJECT
public:
    explicit SearchPanel(reader::Application* app, PdfView* view,
                         QWidget* parent = nullptr);
    void focusQuery();
    void search(const QString& query);

signals:
    void anchorActivated(const reader::DocumentAnchor& anchor);

private:
    void rebuildResults(const QString& query);
    reader::Application* app_;
    PdfView* view_;
    QLineEdit* query_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* results_ = nullptr;
    std::vector<reader::DocumentAnchor> anchors_;
};
