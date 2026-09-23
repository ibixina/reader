#include "ui/WebPanel.h"
#include "ai/References.h"
#include "app/Application.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <cstdlib>

namespace {
std::string webProfileDir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.local/share/paper-reader/webprofile";
}

} // namespace

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
    auto* askButton = new QPushButton("Ask ▸", this);
    askButton->setMinimumHeight(38);
    askButton->setToolTip("Fill the ChatGPT box with selection + question and send");
    askRow->addWidget(question_, 1);
    askRow->addWidget(askButton);
    layout->addLayout(askRow);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setMaximumHeight(36);
    status_->setStyleSheet("color: #666;");
    layout->addWidget(status_);

    view_ = new QWebEngineView(this);
    view_->setPage(new QWebEnginePage(profile_, view_));
    layout->addWidget(view_, 1);

    connect(back, &QPushButton::clicked, view_, &QWebEngineView::back);
    connect(forward, &QPushButton::clicked, view_, &QWebEngineView::forward);
    connect(reload, &QPushButton::clicked, view_, &QWebEngineView::reload);
    connect(home, &QPushButton::clicked, this,
            [this] { view_->load(temporaryChatUrl()); });
    connect(copy, &QPushButton::clicked, this, &WebPanel::copyPrompt);
    connect(askButton, &QPushButton::clicked, this, &WebPanel::ask);
    connect(question_, &QLineEdit::returnPressed, this, &WebPanel::ask);
    // Load state feedback: a silent white page is indistinguishable from
    // a broken one. Report loading / ready / failure persistently.
    connect(view_, &QWebEngineView::loadStarted, this,
            [this] { status_->setText("Loading ChatGPT…"); });
    connect(view_, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (asking_) return; // poll/ask flow owns the status line
        status_->setText(ok ? "ChatGPT ready — log in once, it persists after that."
                            : "Couldn't load chatgpt.com — check connection, then press ⟳.");
    });

    // Lazy first load: constructing the panel (e.g. at startup with the AI
    // pane hidden, or in tests) must not spawn the WebEngine process and
    // fetch chatgpt.com. The URL loads on first show; explicit navigation
    // (home/reload) still works immediately by forcing the load.
    refreshContext();
}

void WebPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    ensureLoaded();
}

void WebPanel::ensureLoaded() {
    if (loaded_ || !view_) return;
    loaded_ = true;
    view_->load(temporaryChatUrl());
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

QString WebPanel::fillScript(const QString& prompt) {
    const QString payload = jsonQuoted(prompt);
    return QStringLiteral(
               "(function(payload){"
               " try {"
               "  var ed = document.getElementById('prompt-textarea')"
               "        || document.querySelector('[data-testid=\"composer-text-input\"]')"
               "        || document.querySelector('form div[contenteditable=\"true\"]')"
               "        || document.querySelector('div[contenteditable=\"true\"]');"
               "  if (!ed) return 'no-editor';"
               "  var pm = ed.querySelector('.ProseMirror') || ed;"
               "  ed.focus();"
               "  var sel = window.getSelection();"
               "  sel.selectAllChildren(pm);"
               "  if (!document.execCommand('insertText', false, payload)) {"
               "   pm.textContent = payload;"
               "   pm.dispatchEvent(new InputEvent('input', {bubbles: true}));"
               "  }"
                "  var send = document.querySelector('[data-testid=\"send-button\"]')"
                "        || document.querySelector('[data-testid=\"composer-send-button\"]')"
                "        || document.querySelector('button[aria-label=\"Send prompt\"]')"
                "        || document.querySelector('button[aria-label*=\"Send\"]')"
                "        || document.querySelector('form button[type=\"submit\"]');"
               "  if (send && !send.disabled) { send.click(); return 'sent'; }"
               "  return 'filled-no-send';"
               " } catch (e) { return 'error:' + e; }"
               "})(%1)")
        .arg(payload);
}

void WebPanel::ask() {
    if (asking_) return;
    ensureLoaded();
    asking_ = true;
    QString prompt = buildPrompt(question_->text().trimmed());
    question_->clear();
    status_->setText("Sending to ChatGPT…");
    // A lost runJavaScript callback must not wedge Ask until a document
    // switch: the timer releases the asking flag after 20 s.
    if (!askTimeout_) {
        askTimeout_ = new QTimer(this);
        askTimeout_->setSingleShot(true);
        askTimeout_->setInterval(20000);
        connect(askTimeout_, &QTimer::timeout, this, [this] {
            if (!asking_) return;
            asking_ = false;
            status_->setText("Ask timed out — the page did not respond. Press ⟳ and retry.");
        });
    }
    askTimeout_->start();
    tryFill(prompt, /*retriesLeft=*/2, [this](const QString& status) {
        askTimeout_->stop();
        asking_ = false;
        if (status == "sent") {
            status_->setText("Sent ✓ — answer streams in the page below.");
            view_->setFocus();
        } else if (status == "filled-no-send") {
            status_->setText("Prompt filled — press Enter in the page to send.");
            view_->setFocus();
        }
    });
}

void WebPanel::tryFill(const QString& prompt, int retriesLeft,
                       std::function<void(const QString&)> done) {
    // Fill ChatGPT's own box and press its send button: select -> type ->
    // Ask feels like native chat, answers stream in the embedded page.
    // QPointer: a late JS callback after tab teardown is dropped, never
    // dereferenced (that use-after-free crashed the app).
    QPointer<WebPanel> guard(this);
    view_->page()->runJavaScript(
        fillScript(prompt),
        [this, guard, prompt, retriesLeft, done](const QVariant& result) {
            if (!guard) return;
            QString status = result.toString();
            if (status == "sent" || status == "filled-no-send") {
                done(status);
                return;
            }
            if (status == "no-editor" && retriesLeft > 0) {
                // Page still loading: one delayed retry before giving up.
                QTimer::singleShot(1500, this, [this, guard, prompt, retriesLeft, done] {
                    if (guard) tryFill(prompt, retriesLeft - 1, done);
                });
                return;
            }
            // Site DOM changed or not logged in: fall back to clipboard.
            asking_ = false;
            QGuiApplication::clipboard()->setText(prompt);
            status_->setText("ChatGPT box not ready (" + status +
                             ") — prompt copied, paste manually.");
        });
}

void WebPanel::cancelPending() {
    // tryFill callbacks are QPointer-guarded; resetting the flag is enough
    // to let a later ask() through after a document switch or teardown.
    asking_ = false;
}

void WebPanel::copyPrompt() {
    // Copy exactly what Ask would send: selection context + question box.
    QGuiApplication::clipboard()->setText(buildPrompt(question_->text().trimmed()));
    context_->setPlainText(context_->toPlainText() + "  · prompt copied, paste into ChatGPT");
    view_->setFocus();
}

void WebPanel::focusQuestion() {
    question_->setFocus(Qt::OtherFocusReason);
}

void WebPanel::focusQuestionWithSeed(const QString& seed) {
    question_->setFocus(Qt::OtherFocusReason);
    if (!seed.isEmpty()) question_->insert(seed);
}
