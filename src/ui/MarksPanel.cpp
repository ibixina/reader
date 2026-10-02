#include "ui/MarksPanel.h"
#include "app/Application.h"
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <unordered_map>

namespace {
QRectF anchorRect(const reader::DocumentAnchor& anchor) {
    const auto& b = anchor.bounds;
    return {b.x, b.y, b.width, b.height};
}

std::string anchorKey(const reader::DocumentAnchor& anchor) {
    return std::to_string(anchor.page) + '\x1f' + anchor.anchorText;
}

bool samePassage(const reader::DocumentAnchor& a, const reader::DocumentAnchor& b) {
    return a.document == b.document && a.page == b.page && a.anchorText == b.anchorText &&
           anchorRect(a).intersects(anchorRect(b));
}

QString preview(const std::string& text, int limit = 160) {
    QString result = QString::fromStdString(text).simplified();
    return result.size() > limit ? result.left(limit) + "…" : result;
}
}

MarksPanel::MarksPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    setObjectName("notesPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    newNote_ = new QPushButton("Note on selection", this);
    newNote_->setObjectName("newNoteButton");
    newNote_->setToolTip("Select a passage and press N to write a note");
    newNote_->setEnabled(false);
    layout->addWidget(newNote_);
    filter_ = new QLineEdit(this);
    filter_->setObjectName("notesFilter");
    filter_->setPlaceholderText("Find in notes and highlights…");
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    list_ = new QListWidget(splitter);
    list_->setObjectName("marksList");
    list_->setWordWrap(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setResizeMode(QListView::Adjust);
    list_->setTextElideMode(Qt::ElideNone);
    list_->setSpacing(4);
    list_->setStyleSheet("QListWidget { border: 0; } QListWidget::item { padding: 10px; "
                         "border: 1px solid #dce1e8; border-radius: 5px; } "
                         "QListWidget::item:selected { background: #e9f0ff; color: #192a46; }");
    details_ = new QWidget(splitter);
    auto* detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(0, 8, 0, 0);
    pageLabel_ = new QLabel(details_);
    pageLabel_->setStyleSheet("font-weight: bold;");
    detailsLayout->addWidget(pageLabel_);
    quote_ = new QTextBrowser(details_);
    quote_->setObjectName("noteQuote");
    quote_->setMaximumHeight(110);
    quote_->document()->setDefaultFont(font());
    quote_->setStyleSheet("QTextBrowser { background: #fff8df; border: 0; padding: 8px; }");
    detailsLayout->addWidget(quote_);
    editor_ = new QPlainTextEdit(details_);
    editor_->setObjectName("noteText");
    editor_->setPlaceholderText("Write your note…");
    editor_->setMinimumHeight(130);
    editor_->setStyleSheet("QPlainTextEdit { border: 1px solid #ccd4df; border-radius: 5px; padding: 8px; }");
    detailsLayout->addWidget(editor_, 1);
    saveStatus_ = new QLabel(details_);
    saveStatus_->setObjectName("noteSaveStatus");
    saveStatus_->setWordWrap(true);
    detailsLayout->addWidget(saveStatus_);
    auto* buttons = new QHBoxLayout;
    deleteNote_ = new QPushButton("Delete note", details_);
    deleteNote_->setObjectName("deleteNoteButton");
    removeHighlight_ = new QPushButton("Remove highlight", details_);
    removeHighlight_->setObjectName("removeHighlightButton");
    buttons->addWidget(deleteNote_);
    buttons->addWidget(removeHighlight_);
    detailsLayout->addLayout(buttons);
    details_->hide();
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);
    auto* hint = new QLabel("H highlights · N adds a note · click a passage to return", this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #667085;");
    layout->addWidget(hint);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(400);
    connect(saveTimer_, &QTimer::timeout, this, [this] { flushPendingNote(); });
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (!currentNote_) return;
        dirty_ = true;
        saveStatus_->setText("Saving…");
        saveTimer_->start();
    });
    connect(newNote_, &QPushButton::clicked, this, &MarksPanel::noteOnSelectionRequested);
    connect(filter_, &QLineEdit::textChanged, this, &MarksPanel::filterEntries);
    connect(list_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem* item) { activateEntry(list_->row(item)); });
    connect(list_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* item) { activateEntry(list_->row(item)); });
    connect(deleteNote_, &QPushButton::clicked, this, &MarksPanel::deleteNote);
    connect(removeHighlight_, &QPushButton::clicked, this, &MarksPanel::removeHighlight);
    auto* save = new QShortcut(QKeySequence("Ctrl+Return"), editor_);
    save->setContext(Qt::WidgetWithChildrenShortcut);
    connect(save, &QShortcut::activated, this, [this] { flushPendingNote(); });
}

MarksPanel::~MarksPanel() { flushPendingNote(); }

void MarksPanel::setSelection(const reader::DocumentAnchor& anchor) {
    newNote_->setEnabled(!anchor.anchorText.empty() && anchor.document == app_->model.document.id);
}

void MarksPanel::rebuild() {
    const auto doc = app_->model.document.id;
    if (document_ != doc) {
        if (!flushPendingNote()) return;
        document_ = doc;
        clearEditor();
        filter_->clear();
    }
    const QSignalBlocker blocker(list_);
    list_->clear();
    entries_.clear();
    if (!app_->annotations || doc.empty()) return;
    auto annotations = app_->annotations->annotationsFor(doc);
    std::stable_sort(annotations.begin(), annotations.end(), [](const auto& a, const auto& b) {
        if (a.anchor.page != b.anchor.page) return a.anchor.page < b.anchor.page;
        return a.anchor.bounds.y < b.anchor.bounds.y;
    });
    std::unordered_map<std::string, std::size_t> groups;
    for (const auto& annotation : annotations) {
        if (annotation.kind != "bookmark" && annotation.kind != "highlight") continue;
        if (annotation.kind == "bookmark") {
            entries_.push_back({annotation.id, "bookmark", annotation.anchor, {}, std::nullopt});
            continue;
        }
        const auto key = annotation.groupId.empty() ? "legacy:" + anchorKey(annotation.anchor)
                                                    : annotation.groupId;
        auto found = groups.find(key);
        // Older versions stored rows without a passage ID. Only join nearby rows.
        if (found != groups.end() && annotation.groupId.empty() &&
            (anchorRect(annotation.anchor).center().y() <= anchorRect(entries_[found->second].anchor).bottom() ||
             anchorRect(annotation.anchor).top() - anchorRect(entries_[found->second].anchor).bottom() >
                 annotation.anchor.bounds.height * 2))
            found = groups.end();
        if (found == groups.end()) {
            groups[key] = entries_.size();
            entries_.push_back({annotation.id, "highlight", annotation.anchor,
                                {annotation.id}, std::nullopt});
        } else {
            auto& entry = entries_[found->second];
            const auto bounds = anchorRect(entry.anchor).united(anchorRect(annotation.anchor));
            entry.anchor.bounds = {float(bounds.x()), float(bounds.y()),
                                    float(bounds.width()), float(bounds.height())};
            entry.highlights.push_back(annotation.id);
        }
    }
    std::unordered_multimap<std::string, std::size_t> passages;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        passages.emplace(anchorKey(entries_[i].anchor), i);
    for (const auto& note : app_->annotations->notesFor(doc)) {
        const auto [first, last] = passages.equal_range(anchorKey(note.anchor));
        auto found = std::find_if(first, last, [&](const auto& candidate) {
            const auto& entry = entries_[candidate.second];
            return !entry.note && samePassage(entry.anchor, note.anchor);
        });
        if (found == last) entries_.push_back({note.id, "note", note.anchor, {}, note});
        else entries_[found->second].note = note;
    }
    std::stable_sort(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
        if (a.anchor.page != b.anchor.page) return a.anchor.page < b.anchor.page;
        if (a.anchor.bounds.y != b.anchor.bounds.y) return a.anchor.bounds.y < b.anchor.bounds.y;
        return a.anchor.bounds.x < b.anchor.bounds.x;
    });
    bool currentFound = false;
    for (const auto& entry : entries_) {
        QString label = QString("%1 · p.%2").arg(entry.kind == "highlight" ? "Highlight" :
                                                  entry.kind == "bookmark" ? "Bookmark" : "Note")
                                               .arg(entry.anchor.page + 1);
        if (!entry.anchor.anchorText.empty()) label += "\n" + preview(entry.anchor.anchorText);
        if (entry.note) label += "\n" + preview(entry.note->text, 100);
        auto* item = new QListWidgetItem(label, list_);
        item->setToolTip(QString::fromStdString(entry.anchor.anchorText));
        if (entry.key == currentKey_ || (currentNote_ && entry.note && entry.note->id == currentNote_->id)) {
            currentFound = true;
            currentKey_ = entry.key;
            currentHighlights_ = entry.highlights;
            list_->setCurrentItem(item);
        }
    }
    if (!currentFound) currentHighlights_.clear();
    removeHighlight_->setEnabled(!currentHighlights_.empty());
    if (entries_.empty()) {
        auto* item = new QListWidgetItem("No notes or highlights yet. Select text and press H or N.", list_);
        item->setFlags(Qt::NoItemFlags);
    }
    filterEntries();
}

void MarksPanel::filterEntries() {
    const auto query = filter_->text().trimmed();
    for (int i = 0; i < int(entries_.size()); ++i) {
        const auto& entry = entries_[i];
        const QString text = QString::fromStdString(entry.anchor.anchorText) + "\n" +
                             (entry.note ? QString::fromStdString(entry.note->text) : QString());
        list_->item(i)->setHidden(!query.isEmpty() && !text.contains(query, Qt::CaseInsensitive));
    }
}

void MarksPanel::activateEntry(int row) {
    if (row < 0 || row >= int(entries_.size())) return;
    const auto key = entries_[row].key;
    if (!flushPendingNote()) return;
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const auto& entry) { return entry.key == key; });
    if (found == entries_.end()) return;
    const auto entry = *found;
    if (!entry.anchor.anchorText.empty()) {
        reader::Note note = entry.note.value_or(reader::Note{});
        if (!entry.note) {
            note.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            note.anchor = entry.anchor;
            note.createdAt = reader::nowMs();
        }
        openEditor(note, entry.key, entry.highlights, entry.note.has_value());
    } else clearEditor();
    emit anchorActivated(entry.anchor);
}

bool MarksPanel::beginNote(const reader::DocumentAnchor& anchor) {
    if (anchor.document.empty() || anchor.anchorText.empty() || !flushPendingNote()) return false;
    rebuild();
    for (const auto& entry : entries_) {
        if (samePassage(entry.anchor, anchor)) {
            reader::Note note = entry.note.value_or(reader::Note{});
            if (!entry.note) {
                note.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                note.anchor = anchor;
                note.createdAt = reader::nowMs();
            }
            openEditor(note, entry.key, entry.highlights, entry.note.has_value());
            return true;
        }
    }
    reader::Note note;
    note.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    note.anchor = anchor;
    note.createdAt = reader::nowMs();
    openEditor(note, note.id, {}, false);
    return true;
}

void MarksPanel::openEditor(const reader::Note& note, const std::string& key,
                            const std::vector<reader::AnnotationId>& highlights, bool persisted) {
    saveTimer_->stop();
    currentNote_ = note;
    currentKey_ = key;
    currentHighlights_ = highlights;
    persisted_ = persisted;
    dirty_ = false;
    pageLabel_->setText(QString("Page %1 · selected passage").arg(note.anchor.page + 1));
    quote_->setPlainText(QString::fromStdString(note.anchor.anchorText));
    const QSignalBlocker blocker(editor_);
    editor_->setPlainText(QString::fromStdString(note.text));
    saveStatus_->setText(persisted ? "Saved" : "Notes save automatically as you type.");
    deleteNote_->setEnabled(persisted);
    removeHighlight_->setEnabled(!highlights.empty());
    details_->show();
    editor_->setFocus(Qt::OtherFocusReason);
}

void MarksPanel::clearEditor() {
    saveTimer_->stop();
    currentNote_.reset();
    currentKey_.clear();
    currentHighlights_.clear();
    dirty_ = false;
    persisted_ = false;
    const QSignalBlocker blocker(editor_);
    editor_->clear();
    quote_->clear();
    pageLabel_->clear();
    details_->hide();
}

bool MarksPanel::flushPendingNote() {
    saveTimer_->stop();
    if (!dirty_ || !currentNote_) return true;
    const auto text = editor_->toPlainText();
    if (!persisted_ && text.trimmed().isEmpty()) {
        dirty_ = false;
        return true;
    }
    auto note = *currentNote_;
    note.text = text.toStdString();
    note.updatedAt = reader::nowMs();
    if (!app_->annotations || !app_->annotations->saveNote(note.anchor.document, note)) {
        saveStatus_->setText("Could not save. Your draft is kept here; press Ctrl+Enter to retry.");
        return false;
    }
    currentNote_ = std::move(note);
    persisted_ = true;
    dirty_ = false;
    saveStatus_->setText("Saved");
    deleteNote_->setEnabled(true);
    rebuild();
    emit annotationsChanged();
    return true;
}

void MarksPanel::deleteNote() {
    if (!currentNote_ || !persisted_) return;
    if (!app_->annotations->deleteNote(currentNote_->anchor.document, currentNote_->id)) {
        saveStatus_->setText("Could not delete the note.");
        return;
    }
    clearEditor();
    rebuild();
    emit annotationsChanged();
}

void MarksPanel::removeHighlight() {
    if (!currentNote_ || currentHighlights_.empty() || !flushPendingNote()) return;
    const auto doc = currentNote_->anchor.document;
    const bool removed = app_->db->transaction([&] {
        for (const auto& id : currentHighlights_)
            if (!app_->annotations->deleteAnnotation(doc, id)) return false;
        return true;
    });
    if (!removed) {
        saveStatus_->setText("Could not remove the highlight.");
        return;
    }
    currentHighlights_.clear();
    removeHighlight_->setEnabled(false);
    rebuild();
    emit annotationsChanged();
}

void MarksPanel::hideEvent(QHideEvent* event) {
    flushPendingNote();
    QWidget::hideEvent(event);
}
