#pragma once
#include "ai/References.h"
#include "core/CancellationToken.h"
#include "document/DocumentAnchor.h"
#include "search/VectorIndex.h"
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

class QListWidget;
class ChatTranscript;
class QLineEdit;
class QPushButton;
class QComboBox;
class QLabel;
class QScrollArea;

namespace reader {
class Application;
}

// Chat tab (§11): streaming Markdown-ish messages, clickable source chips
// (§21/§22), evidence inspector (§24), context chips (§14), @ selector
// (§19/§20), history (§30).
class ChatPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChatPanel(reader::Application* app, QWidget* parent = nullptr);
    ~ChatPanel() override;
    void cancelPending();
    void refreshContextChips();
    void refreshConversations();
    void appendAssistant(const QString& text, const std::vector<reader::ChatSource>& sources);
    void focusQuestion();
    void prepareQuestion(const QString& question,
                         std::optional<reader::ContextReference> reference = std::nullopt);
    using SemanticSnapshotGetter =
        std::function<std::optional<reader::SemanticSearchSnapshot>()>;
    using ReferenceImageRequester = std::function<void(
        const reader::ContextReference&,
        std::function<void(std::optional<reader::ReferenceImage>)>)>;
    void setReaderServices(SemanticSnapshotGetter semanticSnapshot,
                           ReferenceImageRequester referenceImage);

signals:
    void sourceClicked(const reader::DocumentAnchor& anchor);
    void sourceHovered(const reader::DocumentAnchor& anchor);
    void sourceHoverCleared();
    void askRequested();

private slots:
    void sendCurrent();
    void stopCurrent();
    void applyModelSelection();
    void applyProviderSelection();
    void promptApiKey();
    void askWebChatGpt();
    void newConversation();
    void renameConversation();
    void deleteConversation();
    void selectConversation(int index);
    void retryLast();
    void editLast();
    void copyLast();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void submitQuestion(
        QString question,
        std::optional<std::vector<reader::ContextReference>> references = std::nullopt);
    void renderConversation();
    void appendUser(const QString& text, std::size_t referenceCount = 0);
    void appendEvidenceLinks(const std::vector<reader::ChatSource>& sources, int base);
    void refreshEvidenceVisibility();
    void setEvidenceExpanded(bool expanded);

    reader::Application* app_;
    ChatTranscript* thread_ = nullptr;
    QListWidget* chips_ = nullptr;
    QListWidget* evidence_ = nullptr;
    QLabel* evidencePreview_ = nullptr;
    QScrollArea* evidencePreviewScroll_ = nullptr;
    QPushButton* evidenceJump_ = nullptr;
    QPushButton* evidencePin_ = nullptr;
    QPushButton* evidenceToggle_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* keyButton_ = nullptr;
    QPushButton* retryButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QComboBox* providers_ = nullptr;
    QComboBox* models_ = nullptr;
    QLineEdit* providerEndpoint_ = nullptr;
    QLabel* providerStatus_ = nullptr;
    QComboBox* conversations_ = nullptr;
    QLineEdit* conversationSearch_ = nullptr;
    QPushButton* newChat_ = nullptr;
    QPushButton* renameChat_ = nullptr;
    QPushButton* deleteChat_ = nullptr;
    reader::ConversationId conv_;
    reader::DocumentId convDocument_;
    bool streaming_ = false;
    bool evidenceExpanded_ = false;
    reader::CancellationToken chatToken_;
    std::uint64_t chatRequestGeneration_ = 0;
    std::vector<reader::ChatSource> lastSources_;
    std::vector<reader::ChatSource> allSources_;
    std::vector<std::string> allSourceText_;
    SemanticSnapshotGetter semanticSnapshotGetter_;
    ReferenceImageRequester referenceImageRequester_;
};
