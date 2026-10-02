#include "ui/WebPanel.h"
#include "ai/References.h"
#include "app/Application.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <cstdlib>
#include <algorithm>

namespace {
std::string webProfileDir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.local/share/paper-reader/webprofile";
}

} // namespace

struct WebPanel::Session {
    QPointer<QWebEngineView> view;
    QPointer<QTimer> timeout;
    QString question;
    QString sentQuestion;
    QString status;
    QString requestId;
    unsigned long generation = 0;
    bool loaded = false;
    bool initialLoad = false;
    bool asking = false;
    bool clicked = false;
    bool closed = false;
    bool named = false;
};

// Manual JSON string literal: explicit escaping, no conversion quirks.
// Public so the WebEngine tests reuse the exact escaping of the live
// fill script.
QString WebPanel::jsonQuoted(const QString& text) {
    QString payload;
    payload += '"';
    for (QChar c : text) {
        switch (c.unicode()) {
            case '"': payload += "\\\""; break;
            case '\\': payload += "\\\\"; break;
            case '\n': payload += "\\n"; break;
            case '\r': payload += "\\r"; break;
            case '\t': payload += "\\t"; break;
            default:
                if (c.unicode() < 0x20)
                    payload += QString("\\u%1").arg(static_cast<unsigned>(c.unicode()), 4,
                                                   16, QChar('0'));
                else
                    payload += c;
        }
    }
    payload += '"';
    return payload;
}

WebPanel::WebPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    // Dedicated persistent profile: login, history and cookies survive
    // restarts. Paths are fixed BEFORE the profile is used by any page —
    // reconfiguring the shared default profile is silently ignored once
    // WebEngine has initialized it, which drops the login every launch.
    profile_ = new QWebEngineProfile("paper-reader", this);
    profile_->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);
    profile_->setCachePath(QString::fromStdString(webProfileDir() + "/cache"));
    profile_->setPersistentStoragePath(QString::fromStdString(webProfileDir() + "/storage"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // The nav row auto-hides like the main toolbar: edge reveal only, so
    // the chat stays a clean ask box + page. The ask row never hides.
    navBar_ = new QWidget(this);
    navBar_->setObjectName("browserNavBar");
    auto* nav = new QHBoxLayout(navBar_);
    nav->setContentsMargins(0, 0, 0, 0);
    auto* back = new QPushButton("‹", navBar_);
    auto* forward = new QPushButton("›", navBar_);
    auto* reload = new QPushButton("⟳", navBar_);
    auto* home = new QPushButton("chatgpt.com", navBar_);
    auto* copy = new QPushButton("Copy prompt", navBar_);
    copy->setToolTip("Copy selection + question as a ChatGPT-ready prompt");
    context_ = new QTextBrowser(this);
    context_->setObjectName("browserContext");
    context_->setReadOnly(true);
    context_->setOpenExternalLinks(false);
    context_->setMinimumHeight(120);
    context_->setMaximumHeight(280);
    context_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    QFont contextFont = context_->font();
    contextFont.setPointSize(contextFont.pointSize() + 1);
    contextFont.setBold(false);
    context_->setFont(contextFont);
    context_->setStyleSheet(
        "QTextBrowser { background: #eef4ff; border: 1px solid #9bb8e8; border-radius: 6px; "
        "padding: 8px; color: #1a1a1a; }");
    nav->addWidget(back);
    nav->addWidget(forward);
    nav->addWidget(reload);
    nav->addWidget(home);
    nav->addWidget(copy);
    layout->addWidget(navBar_);
    navBar_->hide();
    navTimer_ = new QTimer(this);
    navTimer_->setInterval(250);
    connect(navTimer_, &QTimer::timeout, this, &WebPanel::updateNavReveal);
    navTimer_->start();
    layout->addWidget(context_);
    auto* askRow = new QHBoxLayout();
    question_ = new QLineEdit(this);
    question_->setObjectName("browserQuestion");
    question_->setPlaceholderText("Ask about the selection…");
    question_->setClearButtonEnabled(true);
    question_->setMinimumHeight(38);
    QFont askFont = question_->font();
    askFont.setPointSize(askFont.pointSize() + 2);
    question_->setFont(askFont);
    askButton_ = new QPushButton("Ask ▸", this);
    askButton_->setObjectName("browserAskButton");
    askButton_->setMinimumHeight(38);
    askButton_->setToolTip("Send selection + question to the current chat");
    askRow->addWidget(question_, 1);
    askRow->addWidget(askButton_);
    layout->addLayout(askRow);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setMaximumHeight(36);
    status_->setStyleSheet("color: #666;");
    layout->addWidget(status_);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("browserSessions");
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->tabBar()->setExpanding(false);
    auto* newChat = new QPushButton("New chat", tabs_);
    newChat->setObjectName("browserNewChat");
    newChat->setToolTip("Open another chat (Ctrl+T). Double-click a tab to name it.");
    tabs_->setCornerWidget(newChat, Qt::TopRightCorner);
    layout->addWidget(tabs_, 1);
    connect(newChat, &QPushButton::clicked, this, &WebPanel::newSession);
    connect(tabs_, &QTabWidget::currentChanged, this, &WebPanel::activateSession);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &WebPanel::closeSession);
    connect(tabs_->tabBar(), &QTabBar::tabBarDoubleClicked, this, &WebPanel::renameSession);
    connect(question_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (current_) current_->question = text;
    });
    auto* newShortcut = new QShortcut(QKeySequence("Ctrl+T"), this);
    newShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(newShortcut, &QShortcut::activated, this, &WebPanel::newSession);
    auto* closeShortcut = new QShortcut(QKeySequence("Ctrl+W"), this);
    closeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeShortcut, &QShortcut::activated, this, [this] { closeSession(tabs_->currentIndex()); });

    connect(back, &QPushButton::clicked, this, [this] { if (auto* view = currentView()) view->back(); });
    connect(forward, &QPushButton::clicked, this, [this] { if (auto* view = currentView()) view->forward(); });
    connect(reload, &QPushButton::clicked, this, [this] {
        if (current_ && current_->loaded) current_->view->reload();
        else ensureLoaded();
    });
    connect(home, &QPushButton::clicked, this, [this] {
        if (auto* view = currentView()) view->load(temporaryChatUrl());
    });
    connect(copy, &QPushButton::clicked, this, &WebPanel::copyPrompt);
    connect(askButton_, &QPushButton::clicked, this, &WebPanel::ask);
    connect(question_, &QLineEdit::returnPressed, this, &WebPanel::ask);
    newSession();

    // Lazy first load: constructing the panel (e.g. at startup with the AI
    // pane hidden, or in tests) must not spawn the WebEngine process and
    // fetch chatgpt.com. The URL loads on first show; explicit navigation
    // (home/reload) still works immediately by forcing the load.
    refreshContext();
}

WebPanel::~WebPanel() {
    shuttingDown_ = true;
    cancelPending();
    // Every page must be destroyed before its shared profile.
    for (auto* view : findChildren<QWebEngineView*>()) delete view;
}

QWebEngineView* WebPanel::currentView() const {
    return current_ ? current_->view.data() : nullptr;
}

WebPanel::SessionPtr WebPanel::sessionAt(int index) const {
    QWidget* widget = tabs_->widget(index);
    for (const auto& session : sessions_)
        if (session->view == widget) return session;
    return {};
}

void WebPanel::setStatus(const SessionPtr& session, const QString& text) {
    session->status = text;
    if (!shuttingDown_ && current_ == session) status_->setText(text);
}

void WebPanel::activateSession(int index) {
    if (shuttingDown_) return;
    const auto session = sessionAt(index);
    if (!session || session == current_) return;
    if (current_) current_->question = question_->text();
    current_ = session;
    const QSignalBlocker blocker(question_);
    question_->setText(session->question);
    status_->setText(session->status);
    askButton_->setEnabled(!session->asking);
    if (isVisible()) ensureLoaded();
}

void WebPanel::newSession() {
    if (shuttingDown_) return;
    auto session = std::make_shared<Session>();
    auto* view = new QWebEngineView(tabs_);
    view->setObjectName(QString("browserChatView_%1").arg(++nextSession_));
    view->setPage(new QWebEnginePage(profile_, view));
    session->view = view;
    session->status = "Ask about a selection or start a conversation.";
    session->timeout = new QTimer(view);
    session->timeout->setObjectName("browserAskTimeout");
    session->timeout->setSingleShot(true);
    session->timeout->setInterval(20000);
    connect(session->timeout, &QTimer::timeout, this, [this, session] {
        if (!session->asking) return;
        finishAsk(session, false, session->clicked
            ? "Couldn't confirm sending — check the chat before retrying."
            : "ChatGPT isn't ready to send yet — your question is kept. Sign in or reload and retry.");
    });
    connect(view, &QWebEngineView::loadStarted, this, [this, session] {
        if (shuttingDown_ || session->closed) return;
        const bool initialLoad = session->initialLoad;
        session->initialLoad = false;
        session->loaded = true;
        if (session->asking && !initialLoad) cancel(session);
        if (!session->asking) setStatus(session, "Loading ChatGPT…");
    });
    connect(view, &QWebEngineView::loadFinished, this, [this, session](bool ok) {
        if (shuttingDown_ || session->closed || session->asking) return;
        setStatus(session, ok ? "ChatGPT ready — your login is shared across chats."
                              : "Couldn't load ChatGPT — check the connection and reload.");
    });
    connect(view, &QWebEngineView::titleChanged, this, [this, session](const QString& title) {
        if (shuttingDown_ || session->closed || session->named || title.isEmpty() || title == "ChatGPT") return;
        const int index = tabs_->indexOf(session->view);
        if (index < 0) return;
        tabs_->setTabText(index, title.left(32));
        tabs_->setTabToolTip(index, title);
    });
    sessions_.push_back(session);
    const int index = tabs_->addTab(view, QString("Chat %1").arg(nextSession_));
    tabs_->setCurrentIndex(index);
    activateSession(index);
}

void WebPanel::renameSession(int index) {
    const auto session = sessionAt(index);
    if (!session) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, "Rename chat", "Chat name:",
        QLineEdit::Normal, tabs_->tabText(index), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    session->named = true;
    tabs_->setTabText(index, name.left(32));
    tabs_->setTabToolTip(index, name);
}

void WebPanel::closeSession(int index) {
    const auto session = sessionAt(index);
    if (!session) return;
    cancel(session);
    session->closed = true;
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session), sessions_.end());
    tabs_->removeTab(index);
    session->view->deleteLater();
    if (tabs_->count() == 0) newSession();
    else activateSession(tabs_->currentIndex());
}

void WebPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    ensureLoaded();
}

void WebPanel::ensureLoaded() {
    if (!current_ || current_->loaded) return;
    current_->loaded = true;
    current_->initialLoad = true;
    current_->view->load(temporaryChatUrl());
}

void WebPanel::updateNavReveal() {
    if (!navBar_) return;
    if (!isVisible()) return;
    if (navBar_->isHidden()) {
        const QPoint local = mapFromGlobal(QCursor::pos());
        constexpr int kRevealHeight = 10;
        if (local.y() >= 0 && local.y() <= kRevealHeight && local.x() >= 0 &&
            local.x() < width())
            navBar_->show();
        return;
    }
    if (navBar_->underMouse()) return;
    if (QWidget* focus = QApplication::focusWidget();
        focus && navBar_->isAncestorOf(focus))
        return;
    const QPoint local = mapFromGlobal(QCursor::pos());
    const int hideBelow = navBar_->height() + 8;
    if (local.y() > hideBelow || local.x() < 0 || local.x() >= width() || local.y() < 0)
        navBar_->hide();
}

void WebPanel::clearQuestionDrafts() {
    for (const auto& session : sessions_) session->question.clear();
    question_->clear();
}

void WebPanel::refreshContext() {
    auto ctx = app_->context.currentContext();
    QString text = QString("📍 p.%1").arg(app_->state.page + 1);
    auto fullText = [](const std::string& source) {
        return QString::fromStdString(source).simplified();
    };
    for (const auto& r : ctx.pinned) {
        text += "  📌 " + QString::fromStdString(r.displayName);
        if (const QString snippet = fullText(r.extractedText); !snippet.isEmpty())
            text += " — “" + snippet + "”";
    }
    for (const auto& r : ctx.temporary) {
        text += "  ▫ " + QString::fromStdString(r.displayName);
        if (const QString snippet = fullText(r.extractedText); !snippet.isEmpty())
            text += " — “" + snippet + "”";
    }
    context_->setPlainText(text);
}

QString WebPanel::buildPrompt(const QString& question) const {
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
    prompt += "\nQuestion: " + (question.isEmpty() ? "Explain this passage." : question);
    return prompt;
}

QString WebPanel::fillScript(const QString& prompt, const QString& requestId) {
    return QStringLiteral(R"JS(
(function(payload, id) {
    try {
        if (document.querySelector('[data-testid="stop-button"], button[aria-label="Stop generating"]'))
            return 'busy';
        var ed = document.getElementById('prompt-textarea')
              || document.querySelector('[data-testid="composer-text-input"]')
              || document.querySelector('form [contenteditable="true"], form textarea')
              || document.querySelector('[contenteditable="true"]');
        if (!ed) return 'no-editor';
        var input = ed.querySelector('.ProseMirror, textarea, [contenteditable="true"]') || ed;
        input.focus();
        if (input.tagName === 'TEXTAREA') {
            Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set.call(input, payload);
            input.dispatchEvent(new InputEvent('input', {bubbles: true, data: payload, inputType: 'insertText'}));
        } else {
            var selection = window.getSelection();
            selection.selectAllChildren(input);
            if (!document.execCommand('insertText', false, payload)) {
                input.textContent = payload;
                input.dispatchEvent(new InputEvent('input', {bubbles: true, data: payload, inputType: 'insertText'}));
            }
        }
        window.__paperReaderAsk = {id: id, clicked: false, users: 0, url: location.href};
        return 'filled';
    } catch (error) { return 'error:' + error; }
})(%1, %2)
)JS").arg(jsonQuoted(prompt), jsonQuoted(requestId));
}

QString WebPanel::sendScript(const QString& requestId) {
    return QStringLiteral(R"JS(
(function(id) {
    try {
        var request = window.__paperReaderAsk;
        if (!request || request.id !== id) return 'cancelled';
        var ed = document.getElementById('prompt-textarea')
              || document.querySelector('[data-testid="composer-text-input"]')
              || document.querySelector('form [contenteditable="true"], form textarea')
              || document.querySelector('[contenteditable="true"]');
        var input = ed ? (ed.querySelector('.ProseMirror, textarea, [contenteditable="true"]') || ed) : null;
        var text = input ? (input.value === undefined ? (input.innerText || input.textContent || '') : input.value) : '';
        var users = document.querySelectorAll('[data-message-author-role="user"]').length;
        var stop = document.querySelector('[data-testid="stop-button"], button[aria-label="Stop generating"]');
        if (request.clicked) {
            if (users > request.users || (input && !text.trim() && (stop || location.href !== request.url)))
                return 'sent';
            return 'waiting-confirmation';
        }
        if (location.href !== request.url) return 'cancelled';
        if (stop) return 'busy';
        if (!input) return 'no-editor';
        if (!text.trim()) return 'empty-editor';
        var scope = input.closest('form') || document;
        var buttons = scope.querySelectorAll('#composer-submit-button, [data-testid="send-button"], '
            + '[data-testid="composer-send-button"], button[aria-label="Send prompt"], '
            + 'button[aria-label*="Send"], button[type="submit"]');
        for (var button of buttons) {
            if (button.disabled || button.getAttribute('aria-disabled') === 'true' || !button.getClientRects().length)
                continue;
            request.clicked = true;
            request.users = users;
            request.url = location.href;
            button.click();
            return 'clicked';
        }
        return 'waiting-send';
    } catch (error) { return 'error:' + error; }
})(%1)
)JS").arg(jsonQuoted(requestId));
}

void WebPanel::ask() {
    if (!current_ || current_->asking) return;
    ensureLoaded();
    const auto session = current_;
    session->asking = true;
    session->clicked = false;
    session->sentQuestion = question_->text();
    session->requestId = QString::number(++nextRequest_);
    const unsigned long generation = ++session->generation;
    askButton_->setEnabled(false);
    setStatus(session, "Sending to ChatGPT…");
    session->timeout->start();
    tryFill(session, buildPrompt(session->sentQuestion.trimmed()), generation);
}

void WebPanel::tryFill(const SessionPtr& session, const QString& prompt, unsigned long generation) {
    if (session->closed || !session->asking || session->generation != generation || !session->view) return;
    QPointer<WebPanel> guard(this);
    session->view->page()->runJavaScript(fillScript(prompt, session->requestId),
        [this, guard, session, prompt, generation](const QVariant& result) {
            if (!guard || session->closed || !session->asking || session->generation != generation) return;
            const QString status = result.toString();
            if (status == "filled") {
                // Let the site's input handler enable its send button first.
                QTimer::singleShot(100, this, [this, session, generation] { trySend(session, generation); });
            } else if (status == "no-editor" || status == "busy") {
                QTimer::singleShot(250, this, [this, session, prompt, generation] { tryFill(session, prompt, generation); });
            } else {
                finishAsk(session, false, "Couldn't prepare the ChatGPT composer — your question is kept. Reload and retry.");
            }
        });
}

void WebPanel::trySend(const SessionPtr& session, unsigned long generation) {
    if (session->closed || !session->asking || session->generation != generation || !session->view) return;
    QPointer<WebPanel> guard(this);
    session->view->page()->runJavaScript(sendScript(session->requestId),
        [this, guard, session, generation](const QVariant& result) {
            if (!guard || session->closed || !session->asking || session->generation != generation) return;
            const QString status = result.toString();
            if (status == "sent") {
                finishAsk(session, true, "Sent ✓ — answer streams in this chat.");
            } else if (status == "clicked" || status == "waiting-confirmation" ||
                       status == "waiting-send" || status == "busy" || status == "no-editor") {
                session->clicked = session->clicked || status == "clicked";
                QTimer::singleShot(100, this, [this, session, generation] { trySend(session, generation); });
            } else {
                finishAsk(session, false, session->clicked
                    ? "Couldn't confirm sending — check this chat before retrying."
                    : "ChatGPT couldn't send the prompt — your question is kept. Reload and retry.");
            }
        });
}

void WebPanel::finishAsk(const SessionPtr& session, bool sent, const QString& status) {
    session->asking = false;
    ++session->generation;
    session->timeout->stop();
    if (!sent && session->view)
        session->view->page()->runJavaScript("window.__paperReaderAsk = null;");
    if (sent && session->question == session->sentQuestion) {
        session->question.clear();
        if (current_ == session) question_->clear();
    }
    setStatus(session, status);
    if (current_ == session) {
        askButton_->setEnabled(true);
        if (sent && (question_->hasFocus() || askButton_->hasFocus())) session->view->setFocus();
    }
}

void WebPanel::cancel(const SessionPtr& session) {
    const bool pending = session->asking;
    session->asking = false;
    ++session->generation;
    session->timeout->stop();
    if (pending && session->view)
        session->view->page()->runJavaScript("window.__paperReaderAsk = null;");
    if (pending) setStatus(session, "Sending cancelled — your question is kept.");
    if (!shuttingDown_ && current_ == session) askButton_->setEnabled(true);
}

void WebPanel::cancelPending() {
    for (const auto& session : sessions_) cancel(session);
}

void WebPanel::copyPrompt() {
    // Copy exactly what Ask would send: selection context + question box.
    QGuiApplication::clipboard()->setText(buildPrompt(question_->text().trimmed()));
    context_->setPlainText(context_->toPlainText() + "  · prompt copied, paste into ChatGPT");
    if (auto* view = currentView()) view->setFocus();
}

void WebPanel::focusQuestion() {
    question_->setFocus(Qt::OtherFocusReason);
}

void WebPanel::focusQuestionWithSeed(const QString& seed) {
    question_->setFocus(Qt::OtherFocusReason);
    if (!seed.isEmpty()) question_->insert(seed);
}
