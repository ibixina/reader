#include "ui/OutlinePanel.h"
#include "app/Application.h"
#include <QListWidget>
#include <QListWidgetItem>
#include <QVBoxLayout>

OutlinePanel::OutlinePanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    sections_ = new QListWidget(this);
    layout->addWidget(sections_);
    connect(sections_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const auto id = item->data(Qt::UserRole).toString().toStdString();
        if (!app_) return;
        if (const auto* section = app_->model.findSection(id))
            emit anchorActivated(reader::anchorForSection(app_->model, *section));
    });
}

void OutlinePanel::rebuild() {
    sections_->clear();
    if (!app_) return;
    for (const auto& section : app_->model.sections) {
        auto* item = new QListWidgetItem(
            QString(section.level * 2, ' ') + QString::fromStdString(section.title), sections_);
        item->setData(Qt::UserRole, QString::fromStdString(section.id));
    }
}

