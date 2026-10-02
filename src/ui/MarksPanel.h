#pragma once

#include "storage/Repositories.h"
#include <QWidget>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTimer;
class QHideEvent;

namespace reader {
class Application;
}

class MarksPanel : public QWidget {
    Q_OBJECT
public:
    explicit MarksPanel(reader::Application* app, QWidget* parent = nullptr);
    ~MarksPanel() override;
    void rebuild();
    void setSelection(const reader::DocumentAnchor& anchor);
    bool beginNote(const reader::DocumentAnchor& anchor);
    bool flushPendingNote();

signals:
    void anchorActivated(const reader::DocumentAnchor& anchor);
    void noteOnSelectionRequested();
    void annotationsChanged();

protected:
    void hideEvent(QHideEvent* event) override;

private:
    struct Entry {
        std::string key;
        std::string kind;
        reader::DocumentAnchor anchor;
        std::vector<reader::AnnotationId> highlights;
        std::optional<reader::Note> note;
    };
    void activateEntry(int row);
    void openEditor(const reader::Note& note, const std::string& key,
                    const std::vector<reader::AnnotationId>& highlights, bool persisted);
    void clearEditor();
    void filterEntries();
    void deleteNote();
    void removeHighlight();
    reader::Application* app_;
    QListWidget* list_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QPushButton* newNote_ = nullptr;
    QWidget* details_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    QTextBrowser* quote_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QLabel* saveStatus_ = nullptr;
    QPushButton* deleteNote_ = nullptr;
    QPushButton* removeHighlight_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    std::vector<Entry> entries_;
    reader::DocumentId document_;
    std::optional<reader::Note> currentNote_;
    std::string currentKey_;
    std::vector<reader::AnnotationId> currentHighlights_;
    bool dirty_ = false;
    bool persisted_ = false;
};
