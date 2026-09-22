#pragma once
#include <QWidget>
#include <string>
#include <unordered_set>

class QTextBrowser;
class QComboBox;
#include "document/DocumentAnchor.h"

namespace reader {
class Application;
}

// Summary tab (§31-§33): structured overview + depth + per-section detail.
class SummaryPanel : public QWidget {
    Q_OBJECT
public:
    explicit SummaryPanel(reader::Application* app, QWidget* parent = nullptr);
    void rebuild();

signals:
    void sourceClicked(const reader::DocumentAnchor& anchor);

private:
    reader::Application* app_;
    QTextBrowser* view_ = nullptr;
    QComboBox* depth_ = nullptr;
    QComboBox* sectionDepth_ = nullptr;
    std::unordered_set<std::string> expandedSections_;
};
