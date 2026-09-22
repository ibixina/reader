#pragma once
#include "document/DocumentAnchor.h"
#include <QWidget>

class QGraphicsView;
class QGraphicsScene;
class QLabel;
class QPushButton;
class QResizeEvent;
class QShowEvent;

namespace reader {
class Application;
struct ConceptNode;
} // namespace reader

// Map tab (§34-§36): concept graph over the cached manifest.
class MapPanel : public QWidget {
    Q_OBJECT
public:
    explicit MapPanel(reader::Application* app, QWidget* parent = nullptr);
    ~MapPanel() override;
    void rebuild();

signals:
    void askAbout(const QString& conceptId);
    void jumpToSource(const QString& conceptId);
    void sourceActivated(const reader::DocumentAnchor& anchor);

private:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void fitGraph();

    reader::Application* app_;
    QGraphicsView* view_ = nullptr;
    QGraphicsScene* scene_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* ask_ = nullptr;
    QPushButton* jump_ = nullptr;
    QString selectedConcept_;
};
