#include "ui/ChatPanel.h"
#include "ui/ChatTranscript.h"
#include "ai/ChatManager.h"
#include "ai/PromptBuilder.h"
#include "app/Application.h"
#include <QApplication>
#include <QClipboard>
#include <QCompleter>
#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStringListModel>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

ChatPanel::ChatPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    thread_ = new ChatTranscript(this);
    evidence_ = new QListWidget(this);
    evidence_->setObjectName("evidenceList");
    evidenceToggle_ = new QPushButton("Sources ▾", this);
    evidenceToggle_->setObjectName("evidenceToggle");
    evidence_->setMaximumHeight(90);
    evidence_->setMouseTracking(true);
    evidence_->installEventFilter(this);
    evidencePreview_ = new QLabel(this);
    evidencePreview_->setObjectName("evidencePreview");
    evidencePreview_->setWordWrap(true);
    evidencePreview_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    evidencePreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    evidencePreview_->setText("Select a cited source to inspect evidence.");
    evidenceJump_ = new QPushButton("Jump", this);
    evidenceJump_->setObjectName("evidenceJump");
    evidencePin_ = new QPushButton("Pin", this);
    evidencePin_->setObjectName("evidencePin");
    copyButton_ = new QPushButton("Copy answer", this);
    copyButton_->setObjectName("copyAnswerButton");
    retryButton_ = new QPushButton("Retry", this);
    retryButton_->setObjectName("retryButton");
    editButton_ = new QPushButton("Edit then resend", this);
    editButton_->setObjectName("editResendButton");
    evidencePreviewScroll_ = new QScrollArea(this);
    evidencePreviewScroll_->setObjectName("evidencePreviewScroll");
    evidencePreviewScroll_->setWidgetResizable(true);
    evidencePreviewScroll_->setMaximumHeight(100);
    evidencePreviewScroll_->setFrameShape(QFrame::StyledPanel);
    evidencePreviewScroll_->setWidget(evidencePreview_);
    auto* evidenceActions = new QHBoxLayout();
    evidenceActions->addWidget(evidenceJump_);
    evidenceActions->addWidget(evidencePin_);
    evidenceActions->addStretch(1);
    auto* messageActions = new QHBoxLayout();
    messageActions->addStretch(1);
    messageActions->addWidget(copyButton_);
    messageActions->addWidget(retryButton_);
    messageActions->addWidget(editButton_);
    chips_ = new QListWidget(this);
    chips_->setObjectName("contextChips");
    chips_->setMaximumHeight(72);
    input_ = new QLineEdit(this);
    input_->setObjectName("chatInput");
    input_->setPlaceholderText("Ask about the paper…  (@ to reference)");
    send_ = new QPushButton("Send", this);
    send_->setObjectName("sendButton");
    stop_ = new QPushButton("Stop", this);
    stop_->setObjectName("stopButton");
    stop_->setEnabled(false);
    providers_ = new QComboBox(this);
    providers_->setObjectName("providerSelector");
    providers_->addItem("Offline", "offline");
    providers_->addItem("OpenAI", "openai");
    providers_->addItem("OpenAI-compatible", "compatible");
    models_ = new QComboBox(this);
    models_->setObjectName("modelSelector");
    models_->setEditable(true);
    models_->addItems({"gpt-4o-mini", "gpt-4o"});
    providerEndpoint_ = new QLineEdit(this);
    providerEndpoint_->setObjectName("providerEndpoint");
    providerEndpoint_->setPlaceholderText("OpenAI-compatible endpoint");
    providerEndpoint_->setText(QString::fromStdString(app_->providerConfig.baseUrl));
    providerEndpoint_->setVisible(app_->providerConfig.kind == "compatible");
    providerStatus_ = new QLabel(this);
    providerStatus_->setObjectName("providerStatus");
    providerStatus_->setWordWrap(true);

    // @ completer over live document objects (§19).
    auto* completerModel = new QStringListModel(this);
    auto* completer = new QCompleter(completerModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setWidget(input_);
    connect(completer, qOverload<const QString&>(&QCompleter::activated), this,
            [this](const QString& completion) {
                QString value = input_->text();
                const int cursor = input_->cursorPosition();
                const int at = value.lastIndexOf('@', cursor - 1);
                if (at < 0) return;
                int end = cursor;
                while (end < value.size() && (value[end].isLetterOrNumber() || value[end] == '_'))
                    ++end;
                value.replace(at, end - at, completion);
                input_->setText(value);
                input_->setCursorPosition(at + completion.size());
            });

    auto* row = new QHBoxLayout();
    row->addWidget(input_, 1);
    row->addWidget(send_);
    row->addWidget(stop_);
    auto* webButton = new QPushButton("Web ↗", this);
    webButton->setToolTip("Copy prompt with paper context and open chatgpt.com in your browser");
    row->addWidget(webButton);
    layout->addWidget(thread_, 1);
    layout->addWidget(evidenceToggle_);
    layout->addWidget(evidence_);
    layout->addWidget(evidencePreviewScroll_);
    layout->addLayout(evidenceActions);
    layout->addLayout(messageActions);
    layout->addWidget(chips_);
    layout->addLayout(row);
    auto* conversationRow = new QHBoxLayout();
    conversations_ = new QComboBox(this);
    conversations_->setObjectName("conversationSelector");
    conversations_->setMinimumWidth(180);
    conversationSearch_ = new QLineEdit(this);
    conversationSearch_->setObjectName("conversationSearch");
    conversationSearch_->setPlaceholderText("Search chats");
    newChat_ = new QPushButton("New", this);
    newChat_->setObjectName("newConversationButton");
    renameChat_ = new QPushButton("Rename", this);
    renameChat_->setObjectName("renameConversationButton");
    deleteChat_ = new QPushButton("Delete", this);
    deleteChat_->setObjectName("deleteConversationButton");
    conversationRow->addWidget(conversationSearch_, 1);
    conversationRow->addWidget(conversations_, 2);
    layout->addLayout(conversationRow);
    auto* conversationActions = new QHBoxLayout();
    conversationActions->addWidget(newChat_);
    conversationActions->addWidget(renameChat_);
    conversationActions->addWidget(deleteChat_);
    conversationActions->addStretch(1);
    layout->addLayout(conversationActions);
    auto* settingsRow = new QHBoxLayout();
    settingsRow->addWidget(providers_, 1);
    settingsRow->addWidget(models_, 1);
    keyButton_ = new QPushButton("API key…", this);
    keyButton_->setToolTip("Set the OpenAI API key (stored locally via QSettings)");
    settingsRow->addWidget(keyButton_);
    layout->addLayout(settingsRow);
    layout->addWidget(providerEndpoint_);
    layout->addWidget(providerStatus_);

    const QString configuredKind = QString::fromStdString(
        app_->providerConfig.kind.empty() ? "offline" : app_->providerConfig.kind);
    const int configuredProvider = providers_->findData(configuredKind);
    providers_->setCurrentIndex(configuredProvider < 0 ? 0 : configuredProvider);
    models_->setCurrentText(QString::fromStdString(app_->providerConfig.model));

    connect(input_, &QLineEdit::textChanged, this, [this, completerModel, completer](const QString& t) {
        int at = t.lastIndexOf('@');
        if (at >= 0) {
            QStringList items{"@selection", "@page", "@section", "@methods", "@results"};
            for (const auto& e : app_->model.equations)
                items << QString::fromStdString("@" + e.id);
            for (const auto& f : app_->model.figures)
                items << QString::fromStdString("@" + f.id);
            for (const auto& tb : app_->model.tables)
                items << QString::fromStdString("@" + tb.id);
            completerModel->setStringList(items);
            completer->setCompletionPrefix(t.mid(at));
            completer->complete();
        }
    });
    connect(send_, &QPushButton::clicked, this, &ChatPanel::sendCurrent);
    connect(input_, &QLineEdit::returnPressed, this, &ChatPanel::sendCurrent);
    connect(stop_, &QPushButton::clicked, this, &ChatPanel::stopCurrent);
    connect(models_, &QComboBox::currentTextChanged, this, &ChatPanel::applyModelSelection);
    connect(providers_, &QComboBox::currentIndexChanged, this, &ChatPanel::applyProviderSelection);
    connect(providerEndpoint_, &QLineEdit::editingFinished, this, &ChatPanel::applyProviderSelection);
    connect(keyButton_, &QPushButton::clicked, this, &ChatPanel::promptApiKey);
    connect(newChat_, &QPushButton::clicked, this, &ChatPanel::newConversation);
    connect(renameChat_, &QPushButton::clicked, this, &ChatPanel::renameConversation);
    connect(deleteChat_, &QPushButton::clicked, this, &ChatPanel::deleteConversation);
    connect(conversations_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ChatPanel::selectConversation);
    connect(conversationSearch_, &QLineEdit::textChanged, this, [this] { refreshConversations(); });
    connect(copyButton_, &QPushButton::clicked, this, &ChatPanel::copyLast);
    connect(retryButton_, &QPushButton::clicked, this, &ChatPanel::retryLast);
    connect(editButton_, &QPushButton::clicked, this, &ChatPanel::editLast);
    connect(evidence_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= static_cast<int>(allSources_.size())) return;
        evidencePreview_->setText(QString::fromStdString(allSourceText_[row]));
    });
    connect(evidenceToggle_, &QPushButton::clicked, this, [this] {
        setEvidenceExpanded(!evidenceExpanded_);
    });
    connect(evidence_, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        const int row = evidence_->row(item);
        if (row >= 0 && row < static_cast<int>(allSources_.size()))
            emit sourceHovered(allSources_[row].anchor);
    });
    connect(evidence_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const int row = evidence_->row(item);
        if (row >= 0 && row < static_cast<int>(allSources_.size()))
            emit sourceClicked(allSources_[row].anchor);
    });
    connect(evidenceJump_, &QPushButton::clicked, this, [this] {
        const int row = evidence_->currentRow();
        if (row >= 0 && row < static_cast<int>(allSources_.size()))
            emit sourceClicked(allSources_[row].anchor);
    });
    connect(evidencePin_, &QPushButton::clicked, this, [this] {
        const int row = evidence_->currentRow();
        if (row < 0 || row >= static_cast<int>(allSources_.size())) return;
        reader::ContextReference ref; ref.anchor = allSources_[row].anchor; ref.displayName = "Cited source";
        ref.extractedText = allSourceText_[row]; app_->context.setCurrentSelection(ref);
        app_->context.pinReference(app_->context.currentContext().temporary.front().id);
        refreshContextChips();
    });
    connect(webButton, &QPushButton::clicked, this, &ChatPanel::askWebChatGpt);
    connect(thread_, &ChatTranscript::sourceActivated, this, [this](int index) {
        if (index >= 0 && index < static_cast<int>(allSources_.size())) {
            evidence_->setCurrentRow(index);
            setEvidenceExpanded(true);
            emit sourceClicked(allSources_[index].anchor);
        }
    });
    connect(thread_, &ChatTranscript::sourceHovered, this, [this](int index) {
        if (index >= 0 && index < static_cast<int>(allSources_.size()))
            emit sourceHovered(allSources_[index].anchor);
    });
    connect(thread_, &ChatTranscript::sourceHoverCleared, this,
            &ChatPanel::sourceHoverCleared);
    chips_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(chips_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = chips_->itemAt(point);
        if (!item) return;
        const auto id = item->data(Qt::UserRole).toString().toStdString();
        if (id.empty()) return;
        QMenu menu(this);
        auto ctx = app_->context.currentContext();
        auto find = [&](const auto& refs) -> const reader::ContextReference* {
            for (const auto& ref : refs) if (ref.id == id) return &ref;
            return nullptr;
        };
        const auto* ref = find(ctx.pinned); bool pinned = ref != nullptr;
        if (!ref) ref = find(ctx.temporary);
        if (!ref) return;
        menu.addAction("Preview", [this, ref] { QMessageBox::information(this, "Reference preview",
            QString::fromStdString(ref->extractedText)); });
        menu.addAction(pinned ? "Unpin" : "Pin", [this, id, pinned] {
            if (pinned) app_->context.unpinReference(id); else app_->context.pinReference(id);
            refreshContextChips();
        });
        menu.addAction("Remove", [this, id] { app_->context.removeReference(id); refreshContextChips(); });
        menu.addAction("Jump", [this, ref] { emit sourceClicked(ref->anchor); });
        menu.exec(chips_->viewport()->mapToGlobal(point));
    });

    refreshEvidenceVisibility();
    refreshContextChips();
    refreshConversations();
    applyProviderSelection();
}

ChatPanel::~ChatPanel() { cancelPending(); }

void ChatPanel::cancelPending() {
    ++chatRequestGeneration_;
    chatToken_.cancel();
    if (app_->chatManager) app_->chatManager->stop();
    streaming_ = false;
    if (send_) send_->setEnabled(true);
    if (stop_) stop_->setEnabled(false);
    if (providers_) providers_->setEnabled(true);
    if (models_) models_->setEnabled(true);
    if (providerEndpoint_) providerEndpoint_->setEnabled(true);
    if (keyButton_) keyButton_->setEnabled(true);
}

void ChatPanel::refreshContextChips() {
    chips_->clear();
    auto ctx = app_->context.currentContext();
    QString loc = QString("JADAI · p.%1").arg(app_->state.page + 1);
    chips_->addItem("📍 " + loc);
    for (const auto& r : ctx.pinned) {
        auto* item = new QListWidgetItem("📌 " + QString::fromStdString(r.displayName), chips_);
        item->setData(Qt::UserRole, QString::fromStdString(r.id));
        item->setToolTip(QString::fromStdString(r.extractedText));
    }
    for (const auto& t : ctx.temporary) {
        auto* item = new QListWidgetItem("▫ " + QString::fromStdString(t.displayName), chips_);
        item->setData(Qt::UserRole, QString::fromStdString(t.id));
        item->setToolTip(QString::fromStdString(t.extractedText));
    }
}

void ChatPanel::setEvidenceExpanded(bool expanded) {
    evidenceExpanded_ = expanded;
    refreshEvidenceVisibility();
}

void ChatPanel::refreshEvidenceVisibility() {
    const bool hasSources = !allSources_.empty();
    const bool visible = hasSources && evidenceExpanded_;
    evidenceToggle_->setEnabled(hasSources);
    evidenceToggle_->setText(
        hasSources ? QString("Sources (%1) %2")
                         .arg(allSources_.size())
                         .arg(visible ? QString::fromUtf8("▾") : QString::fromUtf8("▸"))
                   : "Sources (none)");
    evidence_->setVisible(visible);
    evidencePreviewScroll_->setVisible(visible);
    evidenceJump_->setVisible(visible);
    evidencePin_->setVisible(visible);
}

void ChatPanel::refreshConversations() {
    if (!conversations_ || !app_->chats || app_->model.document.id.empty()) return;
    const auto query = conversationSearch_ ? conversationSearch_->text().trimmed().toStdString() : std::string{};
    const auto rows = query.empty() ? app_->chats->conversationsFor(app_->model.document.id)
                                    : app_->chats->searchConversations(app_->model.document.id, query);
    conversations_->blockSignals(true);
    conversations_->clear();
    for (const auto& row : rows)
        conversations_->addItem(QString::fromStdString(row.second), QString::fromStdString(row.first));
    conversations_->blockSignals(false);
    const int current = conversations_->findData(QString::fromStdString(conv_));
    if (current >= 0) {
        conversations_->setCurrentIndex(current);
    } else if (conversations_->count() > 0) {
        conversations_->setCurrentIndex(0);
        selectConversation(0);
    } else {
        conv_.clear();
        convDocument_.clear();
        thread_->clear(); allSources_.clear(); allSourceText_.clear(); evidence_->clear();
        refreshEvidenceVisibility();
    }
}

void ChatPanel::selectConversation(int index) {
    if (index < 0 || !app_->chatManager) return;
    const auto id = conversations_->itemData(index).toString().toStdString();
    if (id.empty() || id == conv_) return;
    cancelPending();
    conv_ = id;
    convDocument_ = app_->model.document.id;
    app_->chatManager->openConversation(conv_);
    renderConversation();
}

void ChatPanel::newConversation() {
    if (!app_->chatManager || app_->model.document.id.empty()) return;
    cancelPending();
    convDocument_ = app_->model.document.id;
    conv_ = app_->chatManager->newConversation("New chat", convDocument_);
    thread_->clear(); allSources_.clear(); allSourceText_.clear(); evidence_->clear();
    refreshEvidenceVisibility();
    refreshConversations();
}

void ChatPanel::renameConversation() {
    if (conv_.empty() || !app_->chats) return;
    bool ok = false;
    const QString current = conversations_->currentText();
    const QString title = QInputDialog::getText(this, "Rename conversation", "Title:",
                                                 QLineEdit::Normal, current, &ok).trimmed();
    if (ok && !title.isEmpty() &&
        app_->chats->renameConversation(conv_, title.toStdString())) refreshConversations();
}

void ChatPanel::deleteConversation() {
    if (conv_.empty() || !app_->chats) return;
    if (!app_->chats->deleteConversation(conv_)) return;
    conv_.clear();
    convDocument_.clear();
    thread_->clear(); allSources_.clear(); allSourceText_.clear(); evidence_->clear();
    refreshEvidenceVisibility();
    refreshConversations();
}

void ChatPanel::appendAssistant(const QString& text,
                                const std::vector<reader::ChatSource>& sources) {
    lastSources_ = sources;
    const int base = static_cast<int>(allSources_.size());
    allSources_.insert(allSources_.end(), sources.begin(), sources.end());
    for (const auto& source : sources)
        allSourceText_.push_back(source.anchor.anchorText.empty() ?
            "Evidence unavailable for page " + std::to_string(source.anchor.page + 1) :
            source.anchor.anchorText);
    evidence_->clear();
    for (std::size_t i = 0; i < allSources_.size(); ++i) {
        const auto& source = allSources_[i];
        QString label;
        label = QString::fromStdString(source.citationId);
        const QString page = QString("p.%1").arg(source.anchor.page + 1);
        evidence_->addItem(label == page ? page : QString("%1 · %2").arg(label, page));
    }
    refreshEvidenceVisibility();
    thread_->appendRole("AI");
    std::vector<std::pair<QString, int>> links;
    for (std::size_t i = 0; i < sources.size(); ++i)
        links.emplace_back(QString::fromStdString(sources[i].citationId),
                           base + static_cast<int>(i));
    thread_->appendMarkdown(text, links);
    appendEvidenceLinks(sources, base);
}

void ChatPanel::appendUser(const QString& text, std::size_t referenceCount) {
    thread_->appendRole("You");
    thread_->appendMarkdown(text);
    if (referenceCount > 0)
        thread_->appendHtml(QString("<p><i>[%1 reference%2]</i></p>")
                                .arg(referenceCount)
                                .arg(referenceCount == 1 ? "" : "s"));
}

void ChatPanel::appendEvidenceLinks(const std::vector<reader::ChatSource>& sources, int base) {
    if (sources.empty()) return;
    QString chips;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i];
        const QString name = QString::fromStdString(source.citationId);
        const QString page = QString("p.%1").arg(source.anchor.page + 1);
        const QString label = name == page ? page : QString("%1 · %2").arg(name, page);
        chips += QString("<a href=\"reader-source:%1\">[%2]</a> ")
                     .arg(base + static_cast<int>(i))
                     .arg(label.toHtmlEscaped());
    }
    thread_->appendHtml("<p>" + chips + "</p>");
}

void ChatPanel::renderConversation() {
    thread_->clear();
    allSources_.clear();
    allSourceText_.clear();
    evidence_->clear();
    refreshEvidenceVisibility();
    if (!app_->chatManager || conv_.empty()) return;
    for (const auto& message : app_->chatManager->history(conv_)) {
        if (message.role == "user")
            appendUser(QString::fromStdString(message.text), message.references.size());
        else
            if (!message.sourceRecords.empty()) {
                appendAssistant(QString::fromStdString(message.text), message.sourceRecords);
            } else {
                std::vector<reader::ChatSource> legacy;
                for (const auto& anchor : message.sources)
                    legacy.push_back({reader::anchorReferenceId(anchor), anchor});
                appendAssistant(QString::fromStdString(message.text), legacy);
            }
        if (message.incomplete && message.role == "assistant")
            thread_->appendHtml("<p><i>Incomplete response — use Retry to ask again.</i></p>");
    }
}

void ChatPanel::focusQuestion() {
    input_->setFocus(Qt::OtherFocusReason);
}

void ChatPanel::prepareQuestion(
    const QString& question, std::optional<reader::ContextReference> reference) {
    if (reference) {
        app_->context.setCurrentSelection(std::move(*reference));
        refreshContextChips();
    }
    input_->setText(question);
    input_->setFocus(Qt::OtherFocusReason);
    input_->setCursorPosition(input_->text().size());
}

void ChatPanel::setReaderServices(SemanticSnapshotGetter semanticSnapshot,
                                  ReferenceImageRequester referenceImage) {
    semanticSnapshotGetter_ = std::move(semanticSnapshot);
    referenceImageRequester_ = std::move(referenceImage);
}

bool ChatPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == evidence_ && event->type() == QEvent::Leave) {
        emit sourceHoverCleared();
    }
    return QWidget::eventFilter(watched, event);
}

void ChatPanel::applyModelSelection() {
    const QString model = models_->currentText().trimmed();
    if (!model.isEmpty()) app_->providerConfig.model = model.toStdString();
    QSettings().setValue("provider/model", model);
    if (app_->chatManager) app_->chatManager->setProvider(app_->buildProvider());
    providerStatus_->setText(QString("Model: %1").arg(model));
}

void ChatPanel::applyProviderSelection() {
    const QString kind = providers_->currentData().toString();
    app_->providerConfig.kind = kind.toStdString();
    providerEndpoint_->setVisible(kind == "compatible");
    if (kind == "openai") {
        app_->providerConfig.baseUrl = "https://api.openai.com/v1";
    } else if (kind == "compatible") {
        const QString endpoint = providerEndpoint_->text().trimmed();
        const QUrl endpointUrl(endpoint);
        if (!endpointUrl.isValid() ||
            (endpointUrl.scheme() != "http" && endpointUrl.scheme() != "https")) {
            providerStatus_->setText("Enter a valid HTTP(S) compatible-provider endpoint.");
            return;
        }
        app_->providerConfig.baseUrl = endpoint.toStdString();
    }
    QSettings settings;
    settings.setValue("provider/kind", kind);
    settings.setValue("provider/baseUrl", QString::fromStdString(app_->providerConfig.baseUrl));
    if (app_->chatManager) app_->chatManager->setProvider(app_->buildProvider());
    if (kind == "offline")
        providerStatus_->setText("Offline provider — deterministic answers from local context.");
    else if (kind == "openai" && app_->providerConfig.apiKey.empty())
        providerStatus_->setText("Add an API key before sending to this provider.");
    else
        providerStatus_->setText(QString("%1 ready at %2")
                                     .arg(providers_->currentText(),
                                          QString::fromStdString(app_->providerConfig.baseUrl)));
}

void ChatPanel::promptApiKey() {
    bool ok = false;
    QString key = QInputDialog::getText(this, "OpenAI API key",
                                        "Key (stored only on this machine):",
                                        QLineEdit::Password, QString(), &ok);
    if (!ok) return;
    key = key.trimmed();
    app_->providerConfig.apiKey = key.toStdString();
    if (!key.isEmpty() && app_->providerConfig.kind == "offline") {
        app_->providerConfig.kind = "openai";
        providers_->setCurrentIndex(providers_->findData("openai"));
    }
    QSettings settings;
    if (key.isEmpty()) settings.remove("openai/apiKey");
    else settings.setValue("openai/apiKey", key);
    settings.setValue("provider/kind", QString::fromStdString(app_->providerConfig.kind));
    if (app_->chatManager) app_->chatManager->setProvider(app_->buildProvider());
    providerStatus_->setText(key.isEmpty() ? "API key cleared."
                                            : "API key saved locally; provider ready.");
}

void ChatPanel::askWebChatGpt() {
    // No API key needed: hand the question + paper context to the real
    // ChatGPT website, where the user is already logged in.
    QString question = input_->text().trimmed();
    if (question.isEmpty()) question = "Explain this passage.";
    QString prompt = QString("I am reading the paper \"%1\".\n")
                         .arg(QString::fromStdString(app_->model.document.title));
    auto ctx = app_->context.currentContext();
    auto addRef = [&](const reader::ContextReference& r) {
        prompt += QString("\n[%1, page %2]: %3\n")
                      .arg(QString::fromStdString(r.displayName))
                      .arg(r.anchor.page + 1)
                      .arg(QString::fromStdString(r.extractedText).left(1500));
    };
    for (const auto& r : ctx.pinned) addRef(r);
    for (const auto& r : ctx.temporary) addRef(r);
    prompt += "\nQuestion: " + question;
    QGuiApplication::clipboard()->setText(prompt);
    QDesktopServices::openUrl(QUrl("https://chatgpt.com/"));
    input_->clear();
    thread_->appendHtml("<p><i>Prompt with paper context copied to clipboard — "
                        "chatgpt.com opened in your browser. Paste and send.</i></p>");
}

void ChatPanel::sendCurrent() {
    const QString question = input_->text().trimmed();
    if (question.isEmpty() || streaming_) return;
    submitQuestion(question);
}

void ChatPanel::submitQuestion(
    QString question, std::optional<std::vector<reader::ContextReference>> references) {
    if (question.isEmpty() || streaming_ || !app_->chatManager) return;
    const QString providerKind = providers_->currentData().toString();
    if (providerKind == "openai" && app_->providerConfig.apiKey.empty()) {
        providerStatus_->setText("This provider needs an API key. Your question is still in the composer.");
        input_->setText(question);
        input_->setFocus(Qt::OtherFocusReason);
        return;
    }

    if (conv_.empty() || convDocument_ != app_->model.document.id) {
        convDocument_ = app_->model.document.id;
        conv_ = app_->chatManager->newConversation("New chat", convDocument_);
        refreshConversations();
    }

    // Resolve @ shorthands into structured, pinned references (§20). Trim
    // surrounding punctuation so "compare @fig2," still resolves.
    if (!references) {
        for (QString token : question.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
            if (!token.startsWith('@')) continue;
            token.remove(QRegularExpression("[^A-Za-z0-9_]+$"));
            if (auto resolved = app_->context.resolveShorthand(
                    token.toStdString(), app_->model, app_->state.readerState)) {
                app_->context.setCurrentSelection(*resolved);
                const auto current = app_->context.currentContext();
                if (!current.temporary.empty())
                    app_->context.pinReference(current.temporary.front().id);
            }
        }
    }

    reader::PaperMetadata paper{app_->model.document.title, app_->model.document.authors,
                                app_->model.document.id};
    reader::ChatRequest req =
        app_->context.buildRequest(question.toStdString(), app_->state.readerState, paper,
                                   {}, {});
    if (references) req.explicitReferences = std::move(*references);
    reader::DocumentModel model = app_->model;
    reader::RetrievalEngine retrieval = app_->retrieval;
    std::optional<reader::PaperAnalysis> analysis = app_->analysis;
    if (semanticSnapshotGetter_) {
        if (auto snapshot = semanticSnapshotGetter_())
            retrieval.setSemanticSnapshot(std::move(*snapshot));
    }
    reader::RetrievalOptions retrievalOptions;
    retrievalOptions.includeImplicitContext =
        !app_->state.settings.sendOnlyRetrievedPassages;
    refreshContextChips();
    input_->clear();
    appendUser(question, req.explicitReferences.size());
    thread_->appendRole("AI");

    streaming_ = true;
    chatToken_.cancel();
    chatToken_ = reader::CancellationToken{};
    const reader::CancellationToken token = chatToken_;
    const auto requestGeneration = ++chatRequestGeneration_;
    if (app_->chatManager) app_->chatManager->resume();
    send_->setEnabled(false);
    stop_->setEnabled(true);
    providers_->setEnabled(false);
    models_->setEnabled(false);
    providerEndpoint_->setEnabled(false);
    keyButton_->setEnabled(false);
    providerStatus_->setText("Preparing paper context…");

    const reader::ConversationId conversation = conv_;
    auto* manager = app_->chatManager.get();
    auto* application = app_;
    QPointer<ChatPanel> guard(this);
    struct EnrichmentState {
        reader::ChatRequest request;
        std::size_t pending = 0;
        bool dispatched = false;
    };
    auto enrichment = std::make_shared<EnrichmentState>();
    enrichment->request = std::move(req);

    auto dispatch = std::make_shared<std::function<void()>>();
    *dispatch = [guard, application, manager, conversation, enrichment, model = std::move(model),
                 retrieval = std::move(retrieval), analysis = std::move(analysis),
                 retrievalOptions, token, requestGeneration]() mutable {
        if (!guard || enrichment->dispatched || token.cancelled() ||
            requestGeneration != guard->chatRequestGeneration_)
            return;
        enrichment->dispatched = true;
        reader::ChatRequest request = std::move(enrichment->request);
        application->searchPool.submit(
            [guard, application, manager, conversation, request = std::move(request),
             model = std::move(model), retrieval = std::move(retrieval),
             analysis = std::move(analysis), retrievalOptions, token,
             requestGeneration]() mutable {
                if (token.cancelled()) return;
                request.retrievedPassages = retrieval.retrieve(
                    model, request.question, request.readerState,
                    analysis ? &*analysis : nullptr, retrievalOptions, token);
                if (token.cancelled()) return;
                application->networkPool.submit(
                    [guard, manager, conversation, request = std::move(request), token,
                     requestGeneration]() mutable {
                        reader::ChatMessage done = manager->send(
                            conversation, std::move(request),
                            [guard, token, requestGeneration](const std::string& tok) {
                                if (auto* gui = QCoreApplication::instance())
                                    QMetaObject::invokeMethod(
                                        gui,
                                        [guard, t = QString::fromStdString(tok), token,
                                         requestGeneration] {
                                            if (!guard || token.cancelled() ||
                                                requestGeneration !=
                                                    guard->chatRequestGeneration_)
                                                return;
                                            guard->thread_->appendPlainText(t);
                                        },
                                        Qt::QueuedConnection);
                            },
                            token);
                        if (auto* gui = QCoreApplication::instance())
                            QMetaObject::invokeMethod(
                                gui,
                                [guard, done, token, requestGeneration, conversation] {
                                    if (!guard || token.cancelled() ||
                                        requestGeneration != guard->chatRequestGeneration_)
                                        return;
                                    if (conversation == guard->conv_)
                                        guard->renderConversation();
                                    guard->streaming_ = false;
                                    guard->send_->setEnabled(true);
                                    guard->stop_->setEnabled(false);
                                    guard->providers_->setEnabled(true);
                                    guard->models_->setEnabled(true);
                                    guard->providerEndpoint_->setEnabled(true);
                                    guard->keyButton_->setEnabled(true);
                                    guard->providerStatus_->setText(
                                        done.incomplete
                                            ? "The response was incomplete. Retry is available."
                                            : "Response complete.");
                                },
                                Qt::QueuedConnection);
                    },
                    token);
            },
            token);
    };

    std::vector<std::size_t> missingImages;
    if (referenceImageRequester_) {
        for (std::size_t i = 0; i < enrichment->request.explicitReferences.size(); ++i) {
            const auto& reference = enrichment->request.explicitReferences[i];
            if (!reference.image &&
                (reference.type == reader::ReferenceType::Figure ||
                 reference.type == reader::ReferenceType::Table))
                missingImages.push_back(i);
        }
    }
    enrichment->pending = missingImages.size();
    for (const std::size_t index : missingImages) {
        const auto reference = enrichment->request.explicitReferences[index];
        referenceImageRequester_(
            reference,
            [guard, enrichment, dispatch, index, token,
             requestGeneration](std::optional<reader::ReferenceImage> image) {
                if (!guard || token.cancelled() ||
                    requestGeneration != guard->chatRequestGeneration_ ||
                    enrichment->dispatched)
                    return;
                if (image) enrichment->request.explicitReferences[index].image = std::move(image);
                if (enrichment->pending > 0) --enrichment->pending;
                if (enrichment->pending == 0) (*dispatch)();
            });
    }
    if (enrichment->pending == 0) {
        (*dispatch)();
    } else {
        QTimer::singleShot(3000, this, [dispatch] { (*dispatch)(); });
    }
}

void ChatPanel::copyLast() {
    if (!app_->chatManager || conv_.empty()) return;
    const auto messages = app_->chatManager->history(conv_);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != "assistant") continue;
        QGuiApplication::clipboard()->setText(QString::fromStdString(it->text));
        providerStatus_->setText("Answer copied.");
        return;
    }
}

void ChatPanel::editLast() {
    if (!app_->chatManager || conv_.empty() || streaming_) return;
    const auto messages = app_->chatManager->history(conv_);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != "user") continue;
        input_->setText(QString::fromStdString(it->text));
        input_->setFocus(Qt::OtherFocusReason);
        input_->selectAll();
        providerStatus_->setText("Edit the previous question, then press Enter to resend.");
        return;
    }
}

void ChatPanel::retryLast() {
    if (!app_->chatManager || conv_.empty() || streaming_) return;
    const auto messages = app_->chatManager->history(conv_);
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role != "user") continue;
        submitQuestion(QString::fromStdString(it->text), it->references);
        return;
    }
}

void ChatPanel::stopCurrent() {
    cancelPending();
    streaming_ = false;
    send_->setEnabled(true);
    stop_->setEnabled(false);
    providers_->setEnabled(true);
    models_->setEnabled(true);
    providerEndpoint_->setEnabled(true);
    keyButton_->setEnabled(true);
    thread_->appendHtml(
        "<p><i>Generation stopped. The partial response is marked incomplete.</i></p>");
}
