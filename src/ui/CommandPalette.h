#pragma once
#include <QDialog>

class QLineEdit;
class QListWidget;

namespace reader {
class Application;
}

// Command palette (§41): app commands + navigation + semantic search.
class CommandPalette : public QDialog {
    Q_OBJECT
public:
    explicit CommandPalette(reader::Application* app, QWidget* parent = nullptr);

signals:
    void commandChosen(const QString& command);

private:
    reader::Application* app_;
    QLineEdit* input_ = nullptr;
    QListWidget* list_ = nullptr;
    void refresh(const QString& filter);
};
