#include "ui/WebPanel.h"
#include "ai/References.h"
#include "app/Application.h"
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
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

// Files above this are sent as text instead: stuffing tens of MB through
// runJavaScript IPC buys nothing over the prompt path.
constexpr qint64 kMaxAttachBytes = 10LL * 1024 * 1024;

// Manual JSON string literal: explicit escaping, no conversion quirks.
// Shared by fillScript (arbitrary prompt text) and attachScript args.
QString jsonQuoted(const QString& text) {
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
} // namespace

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
    auto* nav = new QHBoxLayout();
    auto* back = new QPushButton("‹", this);
    auto* forward = new QPushButton("›", this);
    auto* reload = new QPushButton("⟳", this);
    auto* home = new QPushButton("chatgpt.com", this);
    auto* copy = new QPushButton("Copy prompt", this);
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
    layout->addLayout(nav);
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

    view_->load(temporaryChatUrl());
    refreshContext();
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
    asking_ = true;
    QString prompt = buildPrompt(question_->text().trimmed());
    question_->clear();
    status_->setText("Sending to ChatGPT…");
    tryFill(prompt, /*retriesLeft=*/2, [this](const QString& status) {
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

QString WebPanel::attachScript(const QString& base64Pdf, const QString& filename) {
    return QStringLiteral(
               "(function(b64, name){"
               " try {"
               "  var inp = document.querySelector('input[type=\"file\"]');"
               "  if (!inp) return 'no-file-input';"
               "  window.__attach = 'pending';"
               "  fetch('data:application/pdf;base64,' + b64).then(function(r){"
               "   return r.arrayBuffer();"
               "  }).then(function(buf){"
               "   try {"
               "    var file = new File([buf], name, {type: 'application/pdf'});"
               "    var dt = new DataTransfer();"
               "    dt.items.add(file);"
               "    inp.files = dt.files;"
               "    inp.dispatchEvent(new Event('change', {bubbles: true}));"
               "    inp.dispatchEvent(new Event('input', {bubbles: true}));"
               "    var check = function(tries){"
               "     var has = inp.files && inp.files.length > 0;"
               // innerText needs layout (absent headless); textContent fallback.
               "     var bodyText = (document.body && (document.body.innerText ||"
               "                     document.body.textContent)) || '';"
               "     var chip = bodyText.indexOf(name) !== -1;"
               "     if (has && chip) { window.__attach = 'attached'; return; }"
               "     if (tries <= 0) { window.__attach = has ? 'attached' : 'error:no-chip';"
               "                       return; }"
               "     setTimeout(function(){ check(tries - 1); }, 500);"
               "    };"
               "    check(10);"
               "   } catch (e) { window.__attach = 'error:' + e; }"
               "  }).catch(function(e){ window.__attach = 'error:' + e; });"
               "  return 'started';"
               " } catch (e) { return 'error:' + e; }"
               "})(%1, %2)")
        .arg(jsonQuoted(base64Pdf), jsonQuoted(filename));
}

QString WebPanel::attachPollScript() {
    return QStringLiteral("(function(){ return window.__attach || 'pending'; })()");
}

QString WebPanel::confirmScript() {
    return QStringLiteral(
        "(function(){"
        " try {"
        "  var users = document.querySelectorAll('[data-message-author-role=\"user\"]');"
        "  var stop = document.querySelector('[data-testid=\"stop-button\"]')"
        "        || document.querySelector('button[aria-label=\"Stop generating\"]');"
        "  var ed = document.getElementById('prompt-textarea')"
        "        || document.querySelector('[data-testid=\"composer-text-input\"]')"
        "        || document.querySelector('form div[contenteditable=\"true\"]')"
        "        || document.querySelector('div[contenteditable=\"true\"]');"
        "  var pm = ed ? (ed.querySelector('.ProseMirror') || ed) : null;"
        "  var txt = pm ? (pm.innerText || pm.textContent || '') : '';"
        "  return JSON.stringify({users: users.length, composer: txt.trim().length,"
        "                         generating: !!stop, url: window.location.href});"
        " } catch (e) { return JSON.stringify({users: -1, composer: -1,"
        "                                     generating: false, url: ''}); }"
        "})()");
}

bool WebPanel::sendConfirmed(const QString& beforeJson, const QString& afterJson) {
    const QJsonObject before = QJsonDocument::fromJson(beforeJson.toUtf8()).object();
    const QJsonObject after = QJsonDocument::fromJson(afterJson.toUtf8()).object();
    if (after.value("users").toInt(-1) > before.value("users").toInt(-1)) return true;
    if (!after.value("url").toString().isEmpty() &&
        after.value("url").toString() != before.value("url").toString())
        return true;
    return after.value("generating").toBool(false) && after.value("composer").toInt(-1) == 0;
}

void WebPanel::confirmSend(const QString& beforeJson,
                           std::function<void(const QString&)> done) {
    QPointer<WebPanel> guard(this);
    QTimer::singleShot(2500, this, [this, guard, beforeJson, done] {
        if (!guard) return;
        view_->page()->runJavaScript(
            confirmScript(), [guard, beforeJson, done](const QVariant& after) {
                if (!guard) return;
                done(sendConfirmed(beforeJson, after.toString()) ? "sent"
                                                                : "send-unconfirmed");
            });
    });
}

void WebPanel::attachPdf(const QString& base64Pdf, const QString& filename,
                         std::function<void(const QString&)> done) {
    QPointer<WebPanel> guard(this);
    view_->page()->runJavaScript(
        attachScript(base64Pdf, filename), [this, guard, done](const QVariant& result) {
            if (!guard) return;
            const QString status = result.toString();
            if (status != "started") {
                done("noattach:" + status);
                return;
            }
            auto* timer = new QTimer(this);
            timer->setInterval(1000);
            auto attempts = std::make_shared<int>(25);
            connect(timer, &QTimer::timeout, this,
                    [this, guard, timer, attempts, done] {
                        if (!guard) {
                            timer->stop();
                            timer->deleteLater();
                            return;
                        }
                        view_->page()->runJavaScript(
                            attachPollScript(),
                            [guard, timer, attempts, done](const QVariant& state) {
                                if (!guard) return;
                                const QString s = state.toString();
                                if (s == "attached" || s.startsWith("error")) {
                                    timer->stop();
                                    timer->deleteLater();
                                    done(s == "attached" ? "attached" : "noattach:" + s);
                                } else if (--(*attempts) <= 0) {
                                    timer->stop();
                                    timer->deleteLater();
                                    done("noattach:timeout");
                                }
                            });
                    });
            timer->start();
        });
}

void WebPanel::startPolling(quint64 requestId) {
    pollAttempts_ = 0;
    if (!pollTimer_) {
        pollTimer_ = new QTimer(this);
        connect(pollTimer_, &QTimer::timeout, this, [this] { pollChatResponse(); });
    }
    pollTimer_->start(3000);
    (void)requestId;
}

QString WebPanel::pollScript() {
    return QStringLiteral(
        "(function(){"
        " try {"
        "  var stop = document.querySelector('[data-testid=\"stop-button\"]')"
        "        || document.querySelector('button[aria-label=\"Stop generating\"]');"
        "  var nodes = document.querySelectorAll('[data-message-author-role=\"assistant\"]');"
        "  if (!nodes.length) nodes = document.querySelectorAll('.markdown');"
        "  var text = '';"
        "  if (nodes.length) {"
        "   var el = nodes[nodes.length - 1];"
        "   text = el.innerText || el.textContent || '';"
        "  }"
        "  return JSON.stringify({generating: !!stop, text: text});"
        " } catch (e) { return JSON.stringify({generating: true, text: '', error: String(e)}); }"
        "})()");
}

void WebPanel::cancelPending() {
    if (pollTimer_) pollTimer_->stop();
    asking_ = false;
    activeIngestRequestId_ = 0;
}

void WebPanel::ingestViaChat(const QString& pdfPath, const QString& fullPrompt,
                             const QString& skeletonPrompt, quint64 requestId) {
    if (asking_) return;
    asking_ = true;
    activeIngestRequestId_ = requestId;
    // Manual fallback always stays available: even when auto-send fails, the
    // user pastes + sends by hand and the poller below still picks up the
    // answer. Nothing is ever reported "sent" without send evidence.
    auto manualFallback = [this, requestId, fullPrompt](const QString& why) {
        if (requestId != activeIngestRequestId_) return;
        QGuiApplication::clipboard()->setText(fullPrompt);
        status_->setText("Auto-send failed (" + why +
                         ") — full prompt copied. Attach the PDF yourself, paste, press Enter;"
                         " I'll pick up the answer.");
        startPolling(requestId);
    };
    auto sendSkeleton = [this, requestId, manualFallback, skeletonPrompt] {
        if (requestId != activeIngestRequestId_) return;
        status_->setText("Sending analysis prompt…");
        QPointer<WebPanel> guard(this);
        view_->page()->runJavaScript(
            confirmScript(), [this, guard, requestId, manualFallback,
                              skeletonPrompt](const QVariant& before) {
                if (!guard || requestId != activeIngestRequestId_) return;
                const QString beforeJson = before.toString();
                tryFill(skeletonPrompt, /*retriesLeft=*/2,
                        [this, guard, requestId, manualFallback,
                         beforeJson](const QString& fillStatus) {
                            if (!guard || requestId != activeIngestRequestId_) return;
                            if (fillStatus != "sent") {
                                manualFallback(fillStatus);
                                return;
                            }
                            confirmSend(beforeJson,
                                        [this, requestId, manualFallback](const QString& confirmed) {
                                            if (requestId != activeIngestRequestId_) return;
                                            if (confirmed != "sent") {
                                                manualFallback(confirmed);
                                                return;
                                            }
                                            status_->setText(
                                                "Prompt sent ✓ — waiting for ChatGPT to finish…");
                                            startPolling(requestId);
                                        });
                        });
            });
    };
    QFileInfo info(pdfPath);
    if (!info.exists() || info.size() <= 0 || info.size() > kMaxAttachBytes) {
        status_->setText("Paper file not attachable — sending text instead…");
        tryFill(fullPrompt, /*retriesLeft=*/2,
                [this, requestId, manualFallback](const QString& fillStatus) {
                    if (requestId != activeIngestRequestId_) return;
                    if (fillStatus != "sent") {
                        manualFallback(fillStatus);
                        return;
                    }
                    status_->setText("Prompt sent — waiting for ChatGPT to finish…");
                    startPolling(requestId);
                });
        return;
    }
    QFile file(pdfPath);
    if (!file.open(QIODevice::ReadOnly)) {
        manualFallback("unreadable-pdf");
        return;
    }
    const QString base64 = QString::fromLatin1(file.readAll().toBase64());
    QString base = info.baseName();
    QString clean;
    for (QChar c : base) {
        if (c.isLetterOrNumber() || c == ' ' || c == '-' || c == '_') clean += c;
    }
    clean = clean.trimmed().left(24);
    if (clean.isEmpty()) clean = "paper";
    status_->setText("Attaching paper PDF…");
    attachPdf(base64, clean + ".pdf",
              [this, requestId, manualFallback, sendSkeleton](const QString& attachStatus) {
                  if (requestId != activeIngestRequestId_) return;
                  if (attachStatus != "attached") {
                      manualFallback(attachStatus);
                      return;
                  }
                  status_->setText("Paper attached ✓ — sending analysis prompt…");
                  sendSkeleton();
              });
}

void WebPanel::pollChatResponse() {
    QPointer<WebPanel> guard(this);
    view_->page()->runJavaScript(pollScript(), [this, guard](const QVariant& result) {
        if (!guard) return;
        QJsonDocument doc = QJsonDocument::fromJson(result.toString().toUtf8());
        QJsonObject obj = doc.object();
        bool generating = obj.value("generating").toBool(true);
        QString text = obj.value("text").toString();
        ++pollAttempts_;
        if (generating && pollAttempts_ <= 2)
            status_->setText("ChatGPT is working…");
        if ((!generating && text.size() > 100) || pollAttempts_ >= 120) {
            if (pollTimer_) pollTimer_->stop();
            asking_ = false;
            emit chatIngestResponse((!generating && !text.isEmpty()) ? text : QString(),
                                    activeIngestRequestId_);
        }
    });
}

void WebPanel::copyPrompt() {
    QGuiApplication::clipboard()->setText(buildPrompt(""));
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
