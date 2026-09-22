#include "ui/OutlinePanel.h"
#include "app/Application.h"
#include <QAbstractItemView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

OutlinePanel::OutlinePanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    sections_ = new QListWidget(this);
    layout->addWidget(sections_);
    connect(sections_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (following_) return;
        activateRow(sections_->row(item));
    });
    connect(sections_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (following_) return;
        activateRow(sections_->row(item));
    });
}

void OutlinePanel::activateRow(int row) {
    if (!app_ || row < 0 || row >= sections_->count()) return;
    const auto id = sections_->item(row)->data(Qt::UserRole).toString().toStdString();
    if (const auto* section = app_->model.findSection(id))
        emit anchorActivated(reader::anchorForSection(app_->model, *section));
}

void OutlinePanel::rebuild() {
    sections_->clear();
    if (!app_) return;
    for (const auto& section : app_->model.sections) {
        auto* item = new QListWidgetItem(
            QString(section.level * 2, ' ') + QString::fromStdString(section.title) +
                QString("  ·  p.%1").arg(section.startPage + 1),
            sections_);
        item->setData(Qt::UserRole, QString::fromStdString(section.id));
    }
    followPage(app_->state.page);
}

void OutlinePanel::followPage(int page) {
    if (!app_ || sections_->count() != static_cast<int>(app_->model.sections.size())) return;
    const reader::Section* current = app_->model.sectionForPage(page);
    if (!current) return;
    const QString id = QString::fromStdString(current->id);
    for (int i = 0; i < sections_->count(); ++i) {
        if (sections_->item(i)->data(Qt::UserRole).toString() == id) {
            if (sections_->currentRow() == i) return;
            following_ = true;
            sections_->setCurrentRow(i);
            sections_->scrollToItem(sections_->item(i), QAbstractItemView::PositionAtCenter);
            following_ = false;
            return;
        }
    }
}

