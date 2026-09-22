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
    // JS that stuffs a PDF into the composer's file input. Synchronous
    // result: 'started' (async attach running, outcome in window.__attach),
    // 'no-file-input', or 'error:...'. The async half verifies the file
    // actually landed (input holds it AND its name renders as a chip) and
    // records 'attached' or 'error:<reason>' on window.__attach.
    static QString attachScript(const QString& base64Pdf, const QString& filename);
    // Reads window.__attach ('pending' when unset).
    static QString attachPollScript();
    // JS snapshot of send evidence. Returns a JSON string:
    // {"users":int,"composer":int,"generating":bool,"url":"..."} where users
    // counts user-role messages, composer is the editor's char count, and
    // generating mirrors the stop button. Compared before/after a fill to
    // decide whether a 'sent' actually left the building.
    static QString confirmScript();
    // Pure decision rule over two confirmScript snapshots. Unit-testable.
    static bool sendConfirmed(const QString& beforeJson, const QString& afterJson);
    // JS polling the latest assistant message. Returns a JSON string:
    // {"generating":bool,"text":"..."}. generating is true while the stop
    // button is visible; text is the last assistant block (innerText with
    // textContent fallback for headless rendering).
    static QString pollScript();
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
    QTextBrowser* context_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* question_ = nullptr;
    bool asking_ = false;
    bool loaded_ = false;

protected:
    void showEvent(QShowEvent* event) override;
};
