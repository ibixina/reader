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
class QTimer;

namespace reader {
class Application;
}

// Embedded ChatGPT website, living where the Chat tab was. No API key:
// the user logs into chatgpt.com once (persisted profile) and pastes
// selection-aware prompts assembled by the reader.
class WebPanel : public QWidget {
    Q_OBJECT
public:
    explicit WebPanel(reader::Application* app, QWidget* parent = nullptr);
    void refreshContext();
    void copyPrompt();
    void ask();
    void focusQuestion();
    void focusQuestionWithSeed(const QString& seed);
    // Whole-paper ingest through the embedded chat: attaches the PDF file,
    // sends the ID-skeleton prompt, then polls until generation stops. Emits
    // chatIngestResponse with the raw response text (possibly empty on
    // timeout/failure). Falls back to the full-text prompt when the file
    // cannot be attached; either way the user can complete a failed
    // auto-send manually and polling still picks up the answer.
    void ingestViaChat(const QString& pdfPath, const QString& fullPrompt,
                       const QString& skeletonPrompt, quint64 requestId = 0);
    void cancelPending();
    // JS that fills ChatGPT's prompt box with the payload and presses send.
    // Returns 'sent', 'filled-no-send', 'no-editor', or 'error:...'.
    // 'sent' is mechanical only (a send-looking button was clicked): callers
    // must confirm via confirmScript, because the site can silently swallow
    // programmatic sends while the composer still looks filled.
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
    // Pure decision rule over two confirmScript snapshots. Unit-testable;
    // the async confirmSend below just fetches the snapshots around a delay.
    static bool sendConfirmed(const QString& beforeJson, const QString& afterJson);
    // JS polling the latest assistant message. Returns a JSON string:
    // {"generating":bool,"text":"..."}. generating is true while the stop
    // button is visible; text is the last assistant block (innerText with
    // textContent fallback for headless rendering).
    static QString pollScript();
    // Temporary chat: no history, memory, or personalization. Unknown URL
    // params are ignored by the site, so this degrades to a normal chat.
    static QUrl temporaryChatUrl() { return QUrl("https://chatgpt.com/?temporary-chat=true"); }

signals:
    void chatIngestResponse(const QString& responseText, quint64 requestId);

private:
    QString buildPrompt(const QString& question) const;
    void tryFill(const QString& prompt, int retriesLeft,
                 std::function<void(const QString&)> done);
    // Re-reads send evidence after a delay and maps it to 'sent' or
    // 'send-unconfirmed'. A mechanical click is never trusted on its own.
    void confirmSend(const QString& beforeJson,
                     std::function<void(const QString&)> done);
    // Runs attachScript then polls window.__attach to completion.
    // done() receives 'attached' or 'noattach:<reason>'.
    void attachPdf(const QString& base64Pdf, const QString& filename,
                   std::function<void(const QString&)> done);
    // Starts the assistant-response poller (shared by auto and manual send).
    void startPolling(quint64 requestId);
    void pollChatResponse();
    reader::Application* app_;
    QWebEngineProfile* profile_ = nullptr;
    QWebEngineView* view_ = nullptr;
    QTextBrowser* context_ = nullptr;
    QLabel* status_ = nullptr;
    QLineEdit* question_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    int pollAttempts_ = 0;
    bool asking_ = false;
    quint64 activeIngestRequestId_ = 0;
};
