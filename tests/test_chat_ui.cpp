#include "app/Application.h"
#include "ai/OpenAIProvider.h"
#include "storage/Repositories.h"
#include "ui/ChatPanel.h"
#include "ui/ChatTranscript.h"
#include <QApplication>
#include <QComboBox>
#include <QClipboard>
#include <QCompleter>
#include <QEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QScrollArea>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTimer>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace {

int fail(const char* message) {
    std::cerr << "chat UI check failed: " << message << '\n';
    return 1;
}

reader::ChatMessage assistantMessage(const std::string& id, const std::string& text,
                                     reader::DocumentAnchor source,
                                     reader::TimestampMs createdAt) {
    reader::ChatMessage message;
    message.id = id;
    message.role = "assistant";
    message.text = text;
    message.sources.push_back(std::move(source));
    message.createdAt = createdAt;
    return message;
}

class FakeOpenAiServer {
public:
    FakeOpenAiServer() {
        QObject::connect(&server_, &QTcpServer::newConnection, [&] {
            ++connectionCount_;
            socket_ = server_.nextPendingConnection();
            QObject::connect(socket_, &QTcpSocket::readyRead, [this] { consumeRequest(); });
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }
    QString baseUrl() const {
        return QString("http://127.0.0.1:%1/v1").arg(server_.serverPort());
    }
    QByteArray body() const { return body_; }
    int connectionCount() const { return connectionCount_; }

private:
    void consumeRequest() {
        request_ += socket_->readAll();
        const qsizetype headerEnd = request_.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray headers = request_.left(headerEnd);
        qsizetype contentLength = 0;
        for (const QByteArray& line : headers.split('\n')) {
            if (!line.toLower().startsWith("content-length:")) continue;
            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
        }
        if (request_.size() < headerEnd + 4 + contentLength) return;
        body_ = request_.mid(headerEnd + 4, contentLength);
        const QByteArray payload =
            "data: {\"choices\":[{\"delta\":{\"content\":\"Seen [fig_2]\"}}]}\n\n"
            "data: [DONE]\n\n";
        const QByteArray response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " +
            QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
        socket_->write(response);
        socket_->disconnectFromHost();
    }

    QTcpServer server_;
    QTcpSocket* socket_ = nullptr;
    QByteArray request_;
    QByteArray body_;
    int connectionCount_ = 0;
};

struct UiFailingStore final : reader::IChatStore {
    std::vector<reader::ChatMessage> loadRecent(const reader::ConversationId&,
                                                std::size_t) override {
        return {};
    }
    bool saveMessage(const reader::ConversationId&, const reader::ChatMessage&) override {
        return false;
    }
};

} // namespace

int main(int argc, char** argv) {
    char homeTemplate[] = "/tmp/reader-chat-ui-XXXXXX";
    char* testHome = mkdtemp(homeTemplate);
    if (!testHome) return fail("could not create isolated HOME");
    setenv("HOME", testHome, 1);
    setenv("XDG_CONFIG_HOME", testHome, 1);
    setenv("XDG_DATA_HOME", testHome, 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 0);

    QApplication qt(argc, argv);
    QCoreApplication::setOrganizationName("ReaderChatUiTest");
    QCoreApplication::setApplicationName("ReaderChatUiTest");

    reader::Application app;
    app.providerConfig.kind = "offline";
    app.chatManager->setProvider(std::make_unique<reader::EchoProvider>());
    app.model.document.id = "paper-chat-ui";
    app.model.document.title = "Synthetic Evidence Paper";
    app.model.document.pageCount = 8;
    app.model.figures.push_back({"fig_2", 5, {15, 25, 180, 55}, "Newer figure caption"});
    app.model.rebuildIndex();

    // Verify the actual OpenAI-compatible HTTP body: rich image bytes are
    // encoded at the provider edge, alongside the structured text prompt.
    FakeOpenAiServer fakeServer;
    const bool canListenLocally = fakeServer.listen();
    if (!canListenLocally && std::getenv("READER_REQUIRE_LOCAL_HTTP"))
        return fail("local fake provider could not listen");
    reader::ProviderConfig transportConfig;
    transportConfig.kind = "compatible";
    transportConfig.apiKey.clear();
    transportConfig.model = "fixture-model";
    if (canListenLocally) transportConfig.baseUrl = fakeServer.baseUrl().toStdString();
    reader::ContextReference imageReference;
    imageReference.id = "figure-ref";
    imageReference.type = reader::ReferenceType::Figure;
    imageReference.displayName = "Figure 2";
    imageReference.extractedText = "Caption and nearby discussion";
    imageReference.caption = "Figure 2 caption";
    reader::DocumentAnchor relatedSource;
    relatedSource.document = app.model.document.id;
    relatedSource.page = 6;
    relatedSource.anchorText = "Related result";
    relatedSource.section = "sec_results";
    relatedSource.block = "block_related";
    imageReference.relatedSources.push_back(relatedSource);
    imageReference.anchor.document = app.model.document.id;
    imageReference.anchor.page = 5;
    imageReference.anchor.objectType = "figure";
    imageReference.anchor.objectId = "fig_2";
    imageReference.image = reader::ReferenceImage{"image/png", {0x01, 0x02, 0x03}, 1, 1};
    reader::ChatRequest transportRequest;
    transportRequest.question = "Interpret the figure";
    transportRequest.explicitReferences = {imageReference};
    QByteArray wireBody =
        QJsonDocument(reader::OpenAIProvider::requestBody(transportConfig, transportRequest))
            .toJson(QJsonDocument::Compact);
    if (canListenLocally) {
        reader::OpenAIProvider transportProvider(transportConfig);
        std::string transportAnswer;
        reader::StreamCallbacks callbacks;
        callbacks.onDone = [&](const std::string& answer) { transportAnswer = answer; };
        transportProvider.streamChat(transportRequest, std::move(callbacks));
        if (transportAnswer != "Seen [fig_2]")
            return fail("fake provider response was not consumed");
        wireBody = fakeServer.body();
    }
    const QJsonDocument wire = QJsonDocument::fromJson(wireBody);
    const QJsonArray messages = wire.object().value("messages").toArray();
    const QJsonArray content = messages.at(1).toObject().value("content").toArray();
    if (content.size() != 2 ||
        !content.at(1).toObject().value("image_url").toObject().value("url").toString().startsWith(
            "data:image/png;base64,AQID"))
        return fail("rich image payload was not encoded in the HTTP request");
    const QString promptText = content.at(0).toObject().value("text").toString();
    if (!promptText.contains("Figure 2 caption") || !promptText.contains("block_related"))
        return fail("rich caption or related source was omitted from provider text context");
    transportRequest.explicitReferences.assign(5, imageReference);
    const QJsonArray boundedContent =
        reader::OpenAIProvider::requestBody(transportConfig, transportRequest)
            .value("messages").toArray().at(1).toObject().value("content").toArray();
    if (boundedContent.size() != 5)
        return fail("provider did not enforce the four-image request limit");
    auto unsupported = imageReference;
    unsupported.image->mimeType = "image/svg+xml";
    transportRequest.explicitReferences = {unsupported};
    if (reader::OpenAIProvider::requestBody(transportConfig, transportRequest)
            .value("messages").toArray().at(1).toObject().value("content").isArray())
        return fail("unsupported image MIME type was sent to the provider");

    UiFailingStore uiFailingStore;

    reader::DocumentAnchor oldSource;
    oldSource.document = app.model.document.id;
    oldSource.page = 1;
    oldSource.bounds = {10, 20, 200, 40};
    oldSource.anchorText =
        "Exact evidence from the earlier answer. This deliberately long paragraph verifies "
        "that the inspector wraps full evidence without forcing the narrow side pane wider. "
        "The remainder stays available through the bounded preview's vertical scrollbar.";
    oldSource.section = "sec_methods";
    oldSource.block = "block_old";

    reader::DocumentAnchor newSource;
    newSource.document = app.model.document.id;
    newSource.page = 5;
    newSource.bounds = {15, 25, 180, 55};
    newSource.anchorText = "Exact evidence from the newer figure.";
    newSource.section = "sec_results";
    newSource.block = "block_new";
    newSource.objectType = "figure";
    newSource.objectId = "fig_2";

    const auto oldConversation =
        app.chats->createConversation(app.model.document.id, "Earlier evidence");
    reader::ChatMessage oldQuestion;
    oldQuestion.id = "question-old";
    oldQuestion.role = "user";
    oldQuestion.text = "How does the equation support the result?";
    oldQuestion.createdAt = 90;
    reader::ContextReference oldReference;
    oldReference.id = "old-reference";
    oldReference.type = reader::ReferenceType::Paragraph;
    oldReference.anchor = oldSource;
    oldReference.displayName = "Methods evidence";
    oldReference.extractedText = oldSource.anchorText;
    oldQuestion.references = {oldReference};
    app.chats->saveMessage(oldConversation, oldQuestion);
    app.chats->saveMessage(
        oldConversation,
        assistantMessage("message-old",
                         "Earlier **grounded** answer.\n\n```cpp\nint result = 42;\n"
                         "const char* raw = \"\\\\alpha $not_math$\";\n```\n\n"
                         "For $\\alpha^{2} + \\beta_{1}$, inspect [block_old]. Cost \\$5.",
                         oldSource, 100));
    const auto newConversation =
        app.chats->createConversation(app.model.document.id, "Newer evidence");
    app.chats->saveMessage(
        newConversation,
        assistantMessage("message-new", "New grounded answer", newSource, 200));

    ChatPanel panel(&app);
    int semanticSnapshotRequests = 0;
    int referenceImageRequests = 0;
    panel.setReaderServices(
        [&]() -> std::optional<reader::SemanticSearchSnapshot> {
            ++semanticSnapshotRequests;
            return std::nullopt;
        },
        [&](const reader::ContextReference& reference,
            std::function<void(std::optional<reader::ReferenceImage>)> callback) {
            ++referenceImageRequests;
            const bool supported = reference.type == reader::ReferenceType::Figure &&
                                   reference.anchor.objectId == "fig_2";
            QTimer::singleShot(0, &panel,
                               [supported, callback = std::move(callback)]() mutable {
                                   if (!supported) return callback(std::nullopt);
                                   callback(reader::ReferenceImage{
                                       "image/png", {0x11, 0x22, 0x33}, 3, 2});
                               });
        });
    panel.resize(480, 760);
    panel.show();
    qt.processEvents();
    if (panel.width() > 480) return fail("ChatPanel minimum width exceeds a 480px side pane");

    auto* selector = panel.findChild<QComboBox*>("conversationSelector");
    auto* evidence = panel.findChild<QListWidget*>("evidenceList");
    auto* preview = panel.findChild<QLabel*>("evidencePreview");
    auto* previewScroll = panel.findChild<QScrollArea*>("evidencePreviewScroll");
    auto* jump = panel.findChild<QPushButton*>("evidenceJump");
    auto* pin = panel.findChild<QPushButton*>("evidencePin");
    auto* toggle = panel.findChild<QPushButton*>("evidenceToggle");
    auto* thread = panel.findChild<QTextBrowser*>("chatThread");
    auto* transcript = panel.findChild<ChatTranscript*>();
    auto* makeNew = panel.findChild<QPushButton*>("newConversationButton");
    auto* copyAnswer = panel.findChild<QPushButton*>("copyAnswerButton");
    auto* editResend = panel.findChild<QPushButton*>("editResendButton");
    auto* retry = panel.findChild<QPushButton*>("retryButton");
    auto* send = panel.findChild<QPushButton*>("sendButton");
    auto* stop = panel.findChild<QPushButton*>("stopButton");
    auto* input = panel.findChild<QLineEdit*>("chatInput");
    auto* providerSelector = panel.findChild<QComboBox*>("providerSelector");
    auto* modelSelector = panel.findChild<QComboBox*>("modelSelector");
    auto* providerEndpoint = panel.findChild<QLineEdit*>("providerEndpoint");
    auto* providerStatus = panel.findChild<QLabel*>("providerStatus");
    auto* referenceCompleter = panel.findChild<QCompleter*>();
    if (!selector || !evidence || !preview || !previewScroll || !jump || !pin || !toggle ||
        !thread || !transcript || !makeNew || !copyAnswer || !editResend || !retry || !send ||
        !stop || !input ||
        !providerSelector || !modelSelector || !providerEndpoint || !providerStatus ||
        !referenceCompleter)
        return fail("required ChatPanel test seams were not found");
    for (auto* button : panel.findChildren<QPushButton*>()) {
        const QRect bounds(button->mapTo(&panel, QPoint{}), button->size());
        if (button->isVisible() && !panel.rect().contains(bounds))
            return fail("a visible chat control is clipped outside the narrow side pane");
    }

    reader::DocumentAnchor clicked;
    reader::DocumentAnchor hovered;
    int clickCount = 0;
    int hoverCount = 0;
    int clearCount = 0;
    QObject::connect(&panel, &ChatPanel::sourceClicked, [&](const auto& anchor) {
        clicked = anchor;
        ++clickCount;
    });
    QObject::connect(&panel, &ChatPanel::sourceHovered, [&](const auto& anchor) {
        hovered = anchor;
        ++hoverCount;
    });
    QObject::connect(&panel, &ChatPanel::sourceHoverCleared, [&] { ++clearCount; });

    const auto selectConversation = [&](const reader::ConversationId& id) {
        const int index = selector->findData(QString::fromStdString(id));
        if (index < 0) return false;
        selector->setCurrentIndex(index);
        qt.processEvents();
        return true;
    };

    if (!selectConversation(oldConversation)) return fail("earlier conversation missing");
    evidence->setCurrentRow(0);
    if (preview->text() != QString::fromStdString(oldSource.anchorText))
        return fail("earlier source preview was not exact");
    jump->click();
    if (clickCount != 1 || !(clicked == oldSource))
        return fail("earlier source jump lost anchor identity");

    makeNew->click();
    qt.processEvents();
    if (!selectConversation(newConversation)) return fail("newer conversation missing after New");
    evidence->setCurrentRow(0);
    if (preview->text() != QString::fromStdString(newSource.anchorText))
        return fail("newer source preview was not exact");
    QMetaObject::invokeMethod(evidence, "itemEntered", Qt::DirectConnection,
                              Q_ARG(QListWidgetItem*, evidence->item(0)));
    if (hoverCount != 1 || !(hovered == newSource) || hovered.objectType != "figure" ||
        hovered.objectId != "fig_2")
        return fail("hover did not preserve the typed figure anchor");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(evidence, &leave);
    if (clearCount < 1) return fail("evidence hover clear was not emitted");

    pin->click();
    const auto context = app.context.currentContext();
    if (context.pinned.size() != 1 || !(context.pinned.front().anchor == newSource) ||
        context.pinned.front().extractedText != newSource.anchorText)
        return fail("pin did not retain evidence text and typed source identity");

    if (evidence->isVisible() || previewScroll->isVisible() || !toggle->isEnabled())
        return fail("available sources should start collapsed without consuming transcript space");
    toggle->click();
    if (!evidence->isVisible() || !previewScroll->isVisible())
        return fail("Sources toggle did not expand evidence");
    toggle->click();
    if (evidence->isVisible() || previewScroll->isVisible())
        return fail("Sources toggle did not collapse evidence");
    QMetaObject::invokeMethod(transcript, "sourceActivated", Qt::DirectConnection,
                              Q_ARG(int, 0));
    if (!evidence->isVisible() || evidence->currentRow() != 0)
        return fail("activating an inline source did not reveal and select its evidence");

    if (!selectConversation(oldConversation)) return fail("earlier conversation did not reopen");
    evidence->setCurrentRow(0);
    jump->click();
    if (!(clicked == oldSource) || preview->text() != QString::fromStdString(oldSource.anchorText))
        return fail("old message source changed after new/reopen sequence");
    if (!thread->toPlainText().contains("Earlier grounded answer"))
        return fail("reopened conversation body was not rendered");
    if (!thread->toPlainText().contains("int result = 42") ||
        !thread->toPlainText().contains(QString::fromUtf8("α² + β₁"))) {
        std::cerr << "rendered transcript:\n" << thread->toPlainText().toStdString() << '\n';
        return fail("Markdown code or the supported math subset was not rendered");
    }
    if (!thread->toPlainText().contains("\\alpha $not_math$") ||
        !thread->toPlainText().contains("Cost $5"))
        return fail("math fallback modified fenced code or an escaped dollar");
    if (thread->toHtml().count("reader-source:0") < 2)
        return fail("recognized citation in assistant prose was not linked safely");
    if (previewScroll->maximumHeight() > 110)
        return fail("long evidence preview is not bounded for a narrow side pane");

    copyAnswer->click();
    if (QGuiApplication::clipboard()->text() !=
        "Earlier **grounded** answer.\n\n```cpp\nint result = 42;\n"
        "const char* raw = \"\\\\alpha $not_math$\";\n```\n\n"
        "For $\\alpha^{2} + \\beta_{1}$, inspect [block_old]. Cost \\$5.")
        return fail("Copy answer did not preserve the original Markdown");
    editResend->click();
    if (input->text() != "How does the equation support the result?" || !input->hasSelectedText())
        return fail("Edit and resend did not restore the previous user question");

    panel.grab().save("/tmp/reader-chat-evidence.png");
    const std::size_t historyBeforeRetry = app.chatManager->history(oldConversation).size();
    retry->click();
    QElapsedTimer retryWait;
    retryWait.start();
    while (retryWait.elapsed() < 3000 &&
           (app.chatManager->history(oldConversation).size() < historyBeforeRetry + 2 ||
            !send->isEnabled())) {
        qt.processEvents();
        usleep(1000);
    }
    qt.processEvents();
    const auto retriedHistory = app.chatManager->history(oldConversation);
    if (retriedHistory.size() != historyBeforeRetry + 2 ||
        retriedHistory[historyBeforeRetry].references.size() != 1 ||
        !(retriedHistory[historyBeforeRetry].references.front().anchor == oldSource))
        return fail("Retry did not reuse the previous immutable reference snapshot");

    input->setText("@");
    qt.processEvents();
    QStringList completions;
    for (int row = 0; row < referenceCompleter->completionModel()->rowCount(); ++row)
        completions << referenceCompleter->completionModel()
                           ->index(row, 0).data(Qt::DisplayRole).toString();
    if (!completions.contains("@fig_2") || !completions.contains("@selection") ||
        !completions.contains("@methods"))
        return fail("searchable @ picker omitted live objects or standard shorthands");
    const auto historyBeforeReference = app.chatManager->history(oldConversation).size();
    input->setText("Compare @fig2 with the current evidence");
    send->click();
    QElapsedTimer referenceWait;
    referenceWait.start();
    while (referenceWait.elapsed() < 3000 &&
           (app.chatManager->history(oldConversation).size() < historyBeforeReference + 2 ||
            !send->isEnabled())) {
        qt.processEvents();
        usleep(1000);
    }
    const auto referencedHistory = app.chatManager->history(oldConversation);
    if (referencedHistory.size() != historyBeforeReference + 2)
        return fail("@ reference question did not complete");
    bool foundTypedFigure = false;
    for (const auto& reference : referencedHistory[historyBeforeReference].references)
        if (reference.type == reader::ReferenceType::Figure &&
            reference.anchor.objectType == "figure" && reference.anchor.objectId == "fig_2" &&
            reference.image &&
            reference.image->bytes == std::vector<std::uint8_t>({0x11, 0x22, 0x33}))
            foundTypedFigure = true;
    if (!foundTypedFigure)
        return fail("@ figure did not retain typed identity and async crop enrichment");
    if (referenceImageRequests < 1 || semanticSnapshotRequests < 1)
        return fail("reader semantic/image services were not used by the production send path");
    qt.processEvents();

    const int compatibleIndex = providerSelector->findData("compatible");
    providerSelector->setCurrentIndex(compatibleIndex);
    providerEndpoint->setText("not-a-url");
    QMetaObject::invokeMethod(providerEndpoint, "editingFinished");
    if (!providerStatus->text().contains("valid HTTP"))
        return fail("invalid compatible endpoint did not produce a meaningful error");
    const QString persistedEndpoint = canListenLocally
                                          ? fakeServer.baseUrl()
                                          : QString("http://127.0.0.1:9999/v1");
    providerEndpoint->setText(persistedEndpoint);
    QMetaObject::invokeMethod(providerEndpoint, "editingFinished");
    modelSelector->setCurrentText("fixture-custom-model");
    qt.processEvents();
    if (app.providerConfig.kind != "compatible" ||
        app.providerConfig.baseUrl != persistedEndpoint.toStdString() ||
        app.providerConfig.model != "fixture-custom-model")
        return fail("provider, endpoint, or editable model selection was not applied");
    {
        const int connectionsBeforeReopen = fakeServer.connectionCount();
        reader::Application reopenedSettings;
        if (reopenedSettings.providerConfig.kind != "compatible" ||
            reopenedSettings.providerConfig.baseUrl != persistedEndpoint.toStdString() ||
            reopenedSettings.providerConfig.model != "fixture-custom-model") {
            std::cerr << "reopened provider: kind=" << reopenedSettings.providerConfig.kind
                      << " base=" << reopenedSettings.providerConfig.baseUrl
                      << " model=" << reopenedSettings.providerConfig.model << '\n';
            return fail("provider settings did not survive application restart");
        }
        qt.processEvents();
        if (canListenLocally && fakeServer.connectionCount() != connectionsBeforeReopen)
            return fail("reopening compatible-provider settings made an implicit network request");
        reopenedSettings.shutdown();
    }
    providerSelector->setCurrentIndex(providerSelector->findData("openai"));
    input->setText("preserve me without a key");
    send->click();
    if (input->text() != "preserve me without a key" ||
        !providerStatus->text().contains("still in the composer"))
        return fail("missing-key provider error discarded the user's question");

    providerSelector->setCurrentIndex(providerSelector->findData("offline"));
    app.chatManager = std::make_unique<reader::ChatManager>(
        std::make_unique<reader::EchoProvider>(), &uiFailingStore);
    input->setText("question that cannot be saved");
    send->click();
    QElapsedTimer persistenceWait;
    persistenceWait.start();
    while (persistenceWait.elapsed() < 3000 &&
           (app.chatManager->history(oldConversation).size() < 2 || !send->isEnabled())) {
        qt.processEvents();
        usleep(1000);
    }
    qt.processEvents();
    const auto unsavedHistory = app.chatManager->history(oldConversation);
    if (unsavedHistory.size() != 2 || !unsavedHistory.back().incomplete ||
        !thread->toPlainText().contains("could not be saved"))
        return fail("ChatPanel did not surface a retryable persistence failure");
    editResend->click();
    if (input->text() != "question that cannot be saved")
        return fail("persistence failure did not preserve user text for retry");

    std::function<void(std::optional<reader::ReferenceImage>)> pendingCrop;
    panel.setReaderServices(
        {}, [&](const reader::ContextReference&,
                std::function<void(std::optional<reader::ReferenceImage>)> callback) {
            pendingCrop = std::move(callback);
        });
    input->setText("cancel while preparing figure context");
    send->click();
    qt.processEvents();
    if (!pendingCrop || send->isEnabled() || !stop->isEnabled())
        return fail("async reference preparation did not enter a cancellable busy state");
    panel.cancelPending();
    if (!send->isEnabled() || stop->isEnabled() || !providerSelector->isEnabled() ||
        !modelSelector->isEnabled() || !providerEndpoint->isEnabled())
        return fail("cancelling context preparation left chat controls disabled");
    pendingCrop(std::nullopt);
    qt.processEvents();
    std::cout << "chat UI evidence checks passed; screenshot /tmp/reader-chat-evidence.png\n";
    app.shutdown();
    std::filesystem::remove_all(testHome);
    return 0;
}
