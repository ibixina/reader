#pragma once

#include "document/DocumentAnchor.h"
#include <QWidget>

class QListWidget;

namespace reader {
class Application;
}

// Bookmarks and notes for the open paper, click-to-jump. Consumes the
// annotation repository and emits anchors; navigation history stays in the
// host reader.
class MarksPanel : public QWidget {
    Q_OBJECT
public:
    explicit MarksPanel(reader::Application* app, QWidget* parent = nullptr);
    void rebuild();

signals:
    void anchorActivated(const reader::DocumentAnchor& anchor);

private:
    reader::Application* app_;
    QListWidget* list_ = nullptr;
    std::vector<reader::DocumentAnchor> anchors_;
};
