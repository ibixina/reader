#pragma once

#include "document/DocumentAnchor.h"
#include <QWidget>

class QListWidget;

namespace reader {
class Application;
}

// Lightweight outline/section navigation. It consumes the stable document
// model and emits anchors, keeping navigation history in the host reader.
class OutlinePanel : public QWidget {
    Q_OBJECT
public:
    explicit OutlinePanel(reader::Application* app, QWidget* parent = nullptr);
    void rebuild();
    void followPage(int page);

signals:
    void anchorActivated(const reader::DocumentAnchor& anchor);

private:
    void activateRow(int row);

    reader::Application* app_;
    QListWidget* sections_ = nullptr;
    bool following_ = false;
};

