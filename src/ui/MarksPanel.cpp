#include "ui/MarksPanel.h"
#include "app/Application.h"
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

MarksPanel::MarksPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget(this);
    list_->setObjectName("marksList");
    layout->addWidget(list_);
    auto* hint = new QLabel("b bookmark page · n note on selection", this);
    hint->setStyleSheet("color: #666; padding: 4px;");
    layout->addWidget(hint);
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const int row = list_->row(item);
        if (row < 0 || row >= (int)anchors_.size()) return;
        emit anchorActivated(anchors_[row]);
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const int row = list_->row(item);
        if (row < 0 || row >= (int)anchors_.size()) return;
        emit anchorActivated(anchors_[row]);
    });
}

void MarksPanel::rebuild() {
    list_->clear();
    anchors_.clear();
    if (!app_ || !app_->annotations || app_->model.document.id.empty()) return;
    const reader::DocumentId docId = app_->model.document.id;
    // Bookmarks and notes in page order: the reading path through the
    // paper, not the order they were made.
    std::vector<QPair<int, QString>> rows;
    for (const auto& ann : app_->annotations->annotationsFor(docId)) {
        if (ann.kind != "bookmark") continue;
        rows.push_back({ann.anchor.page,
                        QString("🔖  Bookmark  ·  p.%1").arg(ann.anchor.page + 1)});
        anchors_.push_back(ann.anchor);
    }
    for (const auto& note : app_->annotations->notesFor(docId)) {
        QString text = QString::fromStdString(note.text).simplified();
        if (text.size() > 60) text = text.left(60) + "…";
        rows.push_back(
            {note.anchor.page, QString("📝  p.%1  %2").arg(note.anchor.page + 1).arg(text)});
        anchors_.push_back(note.anchor);
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [page, label] : rows)
        new QListWidgetItem(label, list_);
    if (rows.empty())
        new QListWidgetItem("No bookmarks or notes yet.", list_);
}
