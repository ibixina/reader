#include "ui/CommandPalette.h"
#include "app/Application.h"
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

CommandPalette::CommandPalette(reader::Application* app, QWidget* parent)
    : QDialog(parent), app_(app) {
    setWindowTitle("Command");
    auto* layout = new QVBoxLayout(this);
    input_ = new QLineEdit(this);
    input_->setPlaceholderText("> explain current section");
    list_ = new QListWidget(this);
    layout->addWidget(input_);
    layout->addWidget(list_);
    connect(input_, &QLineEdit::textChanged, this, [this](const QString& t) { refresh(t); });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        emit commandChosen(it->text());
        accept();
    });
    refresh("");
}

void CommandPalette::refresh(const QString& filter) {
    QStringList commands{"explain current section", "summarize current page", "go to methods",
                         "toggle concept map",      "new chat",                "pin current selection",
                         "re-ingest paper",         "toggle AI pane",          "pin toolbar",
                         "unpin toolbar",           "toggle toolbar"};
    for (const auto& s : app_->model.sections)
        commands << ("go to " + QString::fromStdString(s.title));
    list_->clear();
    for (const auto& c : commands)
        if (filter.isEmpty() || c.contains(filter, Qt::CaseInsensitive)) list_->addItem(c);
}
