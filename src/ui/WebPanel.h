#pragma once
#include <QWidget>
#include <QUrl>
#include <memory>
#include <vector>

class QWebEngineView;
class QWebEngineProfile;
class QLabel;
class QTextBrowser;
class QPushButton;
class QLineEdit;
class QTabWidget;
class QListWidget;
class QStackedWidget;

namespace reader {
class Application;
}

class WebPanel : public QWidget {
    Q_OBJECT
public:
    explicit WebPanel(reader::Application* app, QWidget* parent = nullptr);
    ~WebPanel() override;
    void refreshContext();
    void clearQuestionDrafts();
    void ask();
    void focusQuestion();
    void focusQuestionWithSeed(const QString& seed);
    void cancelPending();
    void ensureLoaded();
    void newSession();
    void closeSession(int index);
    QWebEngineView* currentView() const;

    // Binds the panel to the open paper. Tabs of another paper close (their
    // chats are saved); if this paper has saved chats, the session list
    // shows so one can be resumed or a new one started.
    void setDocument(const QString& documentId);
    void showChatList();
    void openSavedChat(const QString& url);
    bool chatListVisible() const;
    // Canonical https://claude.ai/chat/<id> for a conversation URL, else empty.
    static QString claudeChatUrl(const QUrl& url);
    // Page title without the site suffix; empty for the site's bare name.
    static QString chatTitle(const QString& pageTitle);
    static QUrl homeUrl() { return QUrl("https://claude.ai/new"); }
    static bool isClaudeUrl(const QUrl& url);

    // Fill once, then poll sendScript until the composer accepts the message.
    // A request token prevents an old callback from sending a newer draft.
    static QString fillScript(const QString& prompt, const QString& requestId = "test");
    static QString sendScript(const QString& requestId = "test");
    // 'busy' while an answer streams, 'draft' with unsent composer text,
    // else 'idle'.
    static QString idleScript();
    static QString jsonQuoted(const QString& text);

private:
    struct Session;
    using SessionPtr = std::shared_ptr<Session>;
    SessionPtr sessionAt(int index) const;
    void activateSession(int index);
    void renameSession(int index);
    void setStatus(const SessionPtr& session, const QString& text);
    void cancel(const SessionPtr& session);
    void tryFill(const SessionPtr& session, const QString& prompt, unsigned long generation);
    void trySend(const SessionPtr& session, unsigned long generation);
    void finishAsk(const SessionPtr& session, bool sent, const QString& status);
    QString buildPrompt(const QString& question) const;
    static QString siteConfig();
    SessionPtr createSession(const QString& url = {}, const QString& title = {},
                             bool named = false);
    void removeSession(const SessionPtr& session);
    void stepLifecycle(const SessionPtr& session);
    void wake(const SessionPtr& session);
    // A hidden chat freezes after kFreezeDelayMs. A background tab is then
    // discarded at once, so its renderer exits; the shown tab, hidden with
    // the pane or window, only after kDiscardDelayMs. Discarded pages reload
    // their URL when shown again.
    static constexpr int kFreezeDelayMs = 30 * 1000;
    static constexpr int kDiscardDelayMs = 2 * 60 * 1000;
    void rememberChat(const SessionPtr& session);
    void rebuildChatList();
    void startChatFromList();
    void hideChatList();
    reader::Application* app_;
    QWebEngineProfile* profile_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    std::vector<SessionPtr> sessions_;
    SessionPtr current_;
    unsigned long nextRequest_ = 0;
    int nextSession_ = 0;
    QTextBrowser* context_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* question_ = nullptr;
    QPushButton* askButton_ = nullptr;
    QString document_;
    QStackedWidget* stack_ = nullptr;
    QWidget* listPage_ = nullptr;
    QListWidget* chatList_ = nullptr;
    QPushButton* listBack_ = nullptr;
    QPushButton* chatsButton_ = nullptr;
    bool shuttingDown_ = false;

protected:
    void showEvent(QShowEvent* event) override;
};
