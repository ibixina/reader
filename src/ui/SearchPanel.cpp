#include "ui/SearchPanel.h"
#include "app/Application.h"
#include "ui/PdfView.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QAbstractItemView>
#include <QVBoxLayout>

SearchPanel::SearchPanel(reader::Application* app, PdfView* view, QWidget* parent)
    : QWidget(parent), app_(app), view_(view) {
    auto* layout = new QVBoxLayout(this);
    auto* row = new QHBoxLayout();
    query_ = new QLineEdit(this);
    query_->setObjectName("searchQuery");
    query_->setPlaceholderText("Search this paper… (Ctrl+F)");
    query_->setClearButtonEnabled(true);
    auto* searchButton = new QPushButton("Search", this);
    searchButton->setObjectName("searchButton");
    row->addWidget(query_, 1);
    row->addWidget(searchButton);
    layout->addLayout(row);
    status_ = new QLabel("Literal search — instant, offline.", this);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    results_ = new QListWidget(this);
    results_->setObjectName("searchResults");
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(results_, 1);
    connect(query_, &QLineEdit::returnPressed, this, [this] { search(query_->text()); });
    connect(searchButton, &QPushButton::clicked, this, [this] { search(query_->text()); });
    connect(results_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const int index = item->data(Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(anchors_.size()))
            emit anchorActivated(anchors_[index]);
    });
}

void SearchPanel::focusQuery() {
    query_->setFocus(Qt::OtherFocusReason);
    query_->selectAll();
}

void SearchPanel::search(const QString& query) {
    rebuildResults(query.trimmed());
}

void SearchPanel::rebuildResults(const QString& query) {
    results_->clear();
    anchors_.clear();
    if (query.isEmpty() || !app_ || !view_) return;
    std::size_t count = 0;
    for (const auto& hit : view_->searchLiteral(query.toStdString(), 100)) {
        auto* item = new QListWidgetItem(
            QString("p.%1  %2").arg(hit.anchor.page + 1)
                .arg(QString::fromStdString(hit.snippet).trimmed()),
            results_);
        anchors_.push_back(hit.anchor);
        item->setData(Qt::UserRole, static_cast<int>(anchors_.size() - 1));
        ++count;
    }
    status_->setText(count == 0 ? QString("No matches for “%1”.").arg(query)
                                : QString("%1 match%2.")
                                      .arg(count)
                                      .arg(count == 1 ? "" : "es"));
}
