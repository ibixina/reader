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
class QTimer;

namespace reader {
class Application;
}

class WebPanel : public QWidget {
    Q_OBJECT
public:
    explicit WebPanel(reader::Application* app, QWidget* parent = nullptr);
    ~WebPanel() override;
    void refreshContext();
    void copyPrompt();
    void ask();
    void focusQuestion();
    void focusQuestionWithSeed(const QString& seed);
    void cancelPending();
    void ensureLoaded();
    void updateNavReveal();
    void newSession();
    void closeSession(int index);
    QWebEngineView* currentView() const;

    // Fill once, then poll sendScript until the composer accepts the message.
    // A request token prevents an old callback from sending a newer draft.
    static QString fillScript(const QString& prompt, const QString& requestId = "test");
    static QString sendScript(const QString& requestId = "test");
    static QString jsonQuoted(const QString& text);
    static QUrl temporaryChatUrl() { return QUrl("https://chatgpt.com/?temporary-chat=true"); }

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
    reader::Application* app_;
    QWebEngineProfile* profile_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    std::vector<SessionPtr> sessions_;
    SessionPtr current_;
    unsigned long nextRequest_ = 0;
    int nextSession_ = 0;
    QWidget* navBar_ = nullptr;
    QTimer* navTimer_ = nullptr;
    QTextBrowser* context_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* question_ = nullptr;
    QPushButton* askButton_ = nullptr;
    bool shuttingDown_ = false;

protected:
    void showEvent(QShowEvent* event) override;
};
