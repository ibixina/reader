#pragma once
#include <QWidget>
#include <QUrl>
#include <functional>

class QWebEngineView;
class QWebEngineProfile;
class QLabel;
class QTextBrowser;
class QPushButton;
class QLineEdit;

namespace reader {
class Application;
}

// Embedded ChatGPT website: the only AI chat. No API key: the user logs
// into chatgpt.com once (persisted profile) and sends selection-aware
// prompts assembled by the reader (Ask) or copies them (Copy prompt).
class WebPanel : public QWidget {
    Q_OBJECT
public:
    explicit WebPanel(reader::Application* app, QWidget* parent = nullptr);
    void refreshContext();
    void copyPrompt();
    void ask();
    void focusQuestion();
    void focusQuestionWithSeed(const QString& seed);
    void cancelPending();
    void ensureLoaded();
    void updateNavReveal();
    // JS that fills ChatGPT's prompt box with the payload and presses send.
    // Returns 'sent', 'filled-no-send', 'no-editor', or 'error:...'.
    // 'sent' is mechanical only (a send-looking button was clicked).
    // Testable against any page exposing the same editor contract.
    static QString fillScript(const QString& prompt);
    // Manual JSON string escaping shared by the prompt fill script.
    static QString jsonQuoted(const QString& text);
    // Temporary chat: no history, memory, or personalization. Unknown URL
    // params are ignored by the site, so this degrades to a normal chat.
    static QUrl temporaryChatUrl() { return QUrl("https://chatgpt.com/?temporary-chat=true"); }

private:
    QString buildPrompt(const QString& question) const;
    void tryFill(const QString& prompt, int retriesLeft,
                 std::function<void(const QString&)> done);
    reader::Application* app_;
    QWebEngineProfile* profile_ = nullptr;
    QWebEngineView* view_ = nullptr;
    QWidget* navBar_ = nullptr;
    QTimer* navTimer_ = nullptr;
    QTimer* askTimeout_ = nullptr;
    QTextBrowser* context_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* question_ = nullptr;
    bool asking_ = false;
    bool loaded_ = false;

protected:
    void showEvent(QShowEvent* event) override;
};
