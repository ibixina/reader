#pragma once

#include "document/DocumentAnchor.h"
#include <QWidget>
#include <cstdint>
#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QLabel;
class QPushButton;

namespace reader {
class Application;
}
class PdfView;

// Reader-owned literal/semantic search surface. It deliberately emits
// anchors instead of reaching into MainWindow, so the host can decide how to
// preserve history and highlight the selected source.
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
    QComboBox* mode_ = nullptr;
    QPushButton* search_ = nullptr;
    QPushButton* configure_ = nullptr;
    QPushButton* build_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* results_ = nullptr;
    std::vector<reader::DocumentAnchor> anchors_;
    std::uint64_t queryGeneration_ = 0;
};
