#include "ui/ChatTranscript.h"
#include <QEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <vector>

#ifdef HAVE_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

namespace {
class LocalOnlyInterceptor final : public QWebEngineUrlRequestInterceptor {
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;
    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        const QString scheme = info.requestUrl().scheme().toLower();
        if (scheme != "qrc" && scheme != "data" && scheme != "about") info.block(true);
    }
};

class TranscriptPage final : public QWebEnginePage {
public:
    std::function<void(int)> activate;
    std::function<void(int)> hover;
    std::function<void()> clearHover;

    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType type, bool mainFrame) override {
        if (type == NavigationTypeLinkClicked && url.scheme() == "reader-source") {
            bool ok = false;
            const int index = url.toString().mid(14).toInt(&ok);
            if (ok && activate) activate(index);
            return false;
        }
        const QString scheme = url.scheme().toLower();
        return mainFrame && (scheme == "qrc" || scheme == "data" || scheme == "about");
    }

    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString& message, int,
                                  const QString&) override {
        if (message == "reader-clear") {
            if (clearHover) clearHover();
            return;
        }
        if (message.startsWith("reader-activate:")) {
            bool ok = false;
            const int index = message.mid(16).toInt(&ok);
            if (ok && activate) activate(index);
            return;
        }
        if (!message.startsWith("reader-hover:")) return;
        bool ok = false;
        const int index = message.mid(13).toInt(&ok);
        if (ok && hover) hover(index);
    }
};
} // namespace
#endif

ChatTranscript::ChatTranscript(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    fallback_ = new QTextBrowser(this);
    fallback_->setObjectName("chatThread");
    fallback_->setOpenLinks(false);
    fallback_->setMouseTracking(true);
    fallback_->installEventFilter(this);
    fallback_->viewport()->setMouseTracking(true);
    fallback_->viewport()->installEventFilter(this);
    connect(fallback_, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        QString value = url.toString();
        if (value.startsWith("reader-source:")) value = value.mid(14);
        bool ok = false;
        const int index = value.toInt(&ok);
        if (ok) emit sourceActivated(index);
    });
    layout->addWidget(fallback_);

    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(30);
    connect(renderTimer_, &QTimer::timeout, this, &ChatTranscript::renderRich);

#ifdef HAVE_WEBENGINE
    profile_ = new QWebEngineProfile(this);
    profile_->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile_->setUrlRequestInterceptor(new LocalOnlyInterceptor(profile_));
    rich_ = new QWebEngineView(this);
    rich_->setObjectName("richChatThread");
    auto* page = new TranscriptPage(profile_, rich_);
    page->activate = [this](int index) { emit sourceActivated(index); };
    page->hover = [this](int index) { emit sourceHovered(index); };
    page->clearHover = [this] { emit sourceHoverCleared(); };
    rich_->setPage(page);
    layout->replaceWidget(fallback_, rich_);
    fallback_->hide();
    rich_->show();
#endif
}

ChatTranscript::~ChatTranscript() {
#ifdef HAVE_WEBENGINE
    delete rich_;
    rich_ = nullptr;
    delete profile_;
    profile_ = nullptr;
#endif
}

bool ChatTranscript::richMathEnabled() const {
#ifdef HAVE_WEBENGINE
    return rich_ != nullptr;
#else
    return false;
#endif
}

void ChatTranscript::clear() {
    bodyHtml_.clear();
    fallback_->clear();
    scheduleRender();
}

void ChatTranscript::appendRole(const QString& role) {
    const QString safe = role.toHtmlEscaped();
    bodyHtml_ += "<div class=\"role\">" + safe + "</div>";
    fallback_->append("<p><b>" + safe + "</b></p><p></p>");
    scheduleRender();
}

void ChatTranscript::appendMarkdown(
    const QString& markdown, const std::vector<std::pair<QString, int>>& sourceLinks) {
    bodyHtml_ += "<div class=\"message\">" + markdownHtml(markdown, sourceLinks) + "</div>";
    fallback_->append(markdownHtml(fallbackMath(markdown), sourceLinks));
    scheduleRender();
}

void ChatTranscript::appendHtml(const QString& html) {
    bodyHtml_ += html;
    fallback_->append(html);
    scheduleRender();
}

void ChatTranscript::appendPlainText(const QString& text) {
    bodyHtml_ += text.toHtmlEscaped();
    fallback_->moveCursor(QTextCursor::End);
    fallback_->insertPlainText(text);
    scheduleRender();
}

void ChatTranscript::scheduleRender() {
#ifdef HAVE_WEBENGINE
    renderTimer_->start();
#endif
}

QString ChatTranscript::markdownHtml(
    const QString& markdown, const std::vector<std::pair<QString, int>>& sourceLinks) {
    QString protectedMarkdown;
    std::vector<std::pair<QString, QString>> replacements;
    const auto protect = [&](const QString& replacement) {
        const QString token = QString("CHATPROTECTEDTOKEN%1Z").arg(replacements.size());
        replacements.emplace_back(token, replacement);
        return token;
    };
    for (qsizetype i = 0; i < markdown.size();) {
        if (markdown.mid(i, 3) == "```") {
            const qsizetype end = markdown.indexOf("```", i + 3);
            if (end >= 0) {
                const qsizetype firstLineEnd = markdown.indexOf('\n', i + 3);
                const bool hasLanguageLine = firstLineEnd >= 0 && firstLineEnd < end;
                const QString language =
                    hasLanguageLine ? markdown.mid(i + 3, firstLineEnd - i - 3).trimmed()
                                    : QString();
                const qsizetype codeStart = hasLanguageLine ? firstLineEnd + 1 : i + 3;
                QString code = markdown.mid(codeStart, end - codeStart);
                if (code.endsWith('\n')) code.chop(1);
                const QString languageClass =
                    language.isEmpty()
                        ? QString()
                        : QString(" class=\"language-%1\"").arg(language.toHtmlEscaped());
                protectedMarkdown +=
                    protect(QString("<pre><code%1>%2</code></pre>")
                                .arg(languageClass, code.toHtmlEscaped()));
                i = end + 3;
                continue;
            }
        }
        if (markdown[i] == '`') {
            const qsizetype end = markdown.indexOf('`', i + 1);
            if (end >= 0 && !markdown.mid(i + 1, end - i - 1).contains('\n')) {
                const QString code = markdown.mid(i + 1, end - i - 1);
                protectedMarkdown += protect("<code>" + code.toHtmlEscaped() + "</code>");
                i = end + 1;
                continue;
            }
        }
        if (markdown[i] == '$' && (i == 0 || markdown[i - 1] != '\\')) {
            const bool display = i + 1 < markdown.size() && markdown[i + 1] == '$';
            const QString delimiter = display ? "$$" : "$";
            const qsizetype end = markdown.indexOf(delimiter, i + delimiter.size());
            if (end >= 0 && (display || !markdown.mid(i + 1, end - i - 1).contains('\n'))) {
                protectedMarkdown += protect(
                    markdown.mid(i, end + delimiter.size() - i).toHtmlEscaped());
                i = end + delimiter.size();
                continue;
            }
        }
        if (markdown[i] == '[') {
            const qsizetype end = markdown.indexOf(']', i + 1);
            if (end >= 0) {
                const QString citation = markdown.mid(i + 1, end - i - 1);
                const auto it = std::find_if(sourceLinks.begin(), sourceLinks.end(),
                                             [&](const auto& link) { return link.first == citation; });
                if (it != sourceLinks.end()) {
                    protectedMarkdown += protect(
                        QString("<a href=\"reader-source:%1\">[%2]</a>")
                            .arg(it->second)
                            .arg(citation.toHtmlEscaped()));
                    i = end + 1;
                    continue;
                }
            }
        }
        protectedMarkdown += markdown[i++];
    }
    QString html = QTextDocumentFragment::fromMarkdown(protectedMarkdown).toHtml();
    for (const auto& [token, replacement] : replacements) {
        if (replacement.startsWith("<pre>")) {
            html.replace(QRegularExpression(
                             QString("<p[^>]*>\\s*%1\\s*</p>")
                                 .arg(QRegularExpression::escape(token))),
                         replacement);
        }
        html.replace(token, replacement);
    }
    return html;
}

QString ChatTranscript::fallbackMath(QString markdown) {
    QString protectedMarkdown;
    std::vector<std::pair<QString, QString>> code;
    const auto protect = [&](const QString& value) {
        const QString token = QString("CHATCODETOKEN%1Z").arg(code.size());
        code.emplace_back(token, value);
        return token;
    };
    for (qsizetype i = 0; i < markdown.size();) {
        const bool fenced = markdown.mid(i, 3) == "```";
        const bool inlineCode = markdown[i] == '`';
        if (fenced || inlineCode) {
            const QString delimiter = fenced ? "```" : "`";
            const qsizetype end = markdown.indexOf(delimiter, i + delimiter.size());
            if (end >= 0 && (fenced || !markdown.mid(i + 1, end - i - 1).contains('\n'))) {
                const qsizetype length = end + delimiter.size() - i;
                protectedMarkdown += protect(markdown.mid(i, length));
                i += length;
                continue;
            }
        }
        if (markdown.mid(i, 2) == "\\$") {
            protectedMarkdown += protect("\\$");
            i += 2;
            continue;
        }
        protectedMarkdown += markdown[i++];
    }
    markdown = protectedMarkdown;
    const std::pair<const char*, const char*> symbols[] = {
        {"\\alpha", "α"}, {"\\beta", "β"},   {"\\gamma", "γ"}, {"\\delta", "δ"},
        {"\\theta", "θ"}, {"\\lambda", "λ"}, {"\\mu", "μ"},    {"\\pi", "π"},
        {"\\sigma", "σ"}, {"\\phi", "φ"},    {"\\omega", "ω"}, {"\\times", "×"},
        {"\\cdot", "·"},  {"\\pm", "±"},     {"\\leq", "≤"},   {"\\geq", "≥"},
        {"\\neq", "≠"},   {"\\infty", "∞"}, {"\\sum", "∑"},   {"\\int", "∫"},
    };
    for (const auto& [tex, glyph] : symbols) markdown.replace(tex, glyph);
    markdown.replace(QRegularExpression(R"(\\frac\{([^{}]+)\}\{([^{}]+)\})"),
                     "(\\1)⁄(\\2)");
    markdown.replace(QRegularExpression(R"(\\sqrt\{([^{}]+)\})"), "√(\\1)");
    const QString normal = "0123456789+-=()";
    const QString superscript = "⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾";
    const QString subscript = "₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎";
    for (const auto& [marker, glyphs] :
         std::initializer_list<std::pair<QChar, QString>>{{'^', superscript}, {'_', subscript}}) {
        QRegularExpression expression(QString("\\%1\\{([0-9+\\-=()]+)\\}").arg(marker));
        QRegularExpressionMatch match;
        int offset = 0;
        while ((offset = markdown.indexOf(expression, offset, &match)) >= 0) {
            QString replacement;
            for (const QChar character : match.captured(1))
                replacement += glyphs[normal.indexOf(character)];
            markdown.replace(offset, match.capturedLength(), replacement);
            offset += replacement.size();
        }
    }
    markdown.replace(QRegularExpression(R"(\$\$([^$]+)\$\$)"), "\n> \\1\n");
    markdown.replace(QRegularExpression(R"(\$([^$\n]+)\$)"), "`\\1`");
    for (const auto& [token, value] : code) markdown.replace(token, value);
    return markdown;
}

bool ChatTranscript::eventFilter(QObject* watched, QEvent* event) {
    const bool fallbackSurface = watched == fallback_ || watched == fallback_->viewport();
    if (fallbackSurface && event->type() == QEvent::MouseMove) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        QString value = fallback_->anchorAt(mouse->position().toPoint());
        if (value.startsWith("reader-source:")) value = value.mid(14);
        bool ok = false;
        const int index = value.toInt(&ok);
        if (ok)
            emit sourceHovered(index);
        else
            emit sourceHoverCleared();
    } else if (fallbackSurface && event->type() == QEvent::Leave) {
        emit sourceHoverCleared();
    }
    return QWidget::eventFilter(watched, event);
}

void ChatTranscript::renderRich() {
#ifdef HAVE_WEBENGINE
    if (!rich_) return;
    QString body = bodyHtml_;
    if (body.size() > 400000) body = body.left(400000) + "<p>Transcript truncated for display.</p>";
    const QString page = QStringLiteral(R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="qrc:/chat/katex/katex.css">
<style>
body{font:14px system-ui,sans-serif;color:#202124;margin:12px;overflow-wrap:anywhere}
.role{font-weight:650;margin-top:14px}.message{margin:5px 0 10px}pre{white-space:pre-wrap;background:#f4f4f5;padding:8px;border-radius:5px}
a{color:#1558d6}.katex-display{overflow-x:auto;overflow-y:hidden}
</style><script src="qrc:/chat/katex/katex.js"></script></head><body>%1
<script>
(()=>{let budget=200000;const walk=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);const nodes=[];
while(walk.nextNode())nodes.push(walk.currentNode);for(const node of nodes){if(!budget)break;
if(node.parentElement.closest('pre,code,.katex'))continue;const text=node.nodeValue;if(text.length>budget)continue;budget-=text.length;
const re=/\$\$([\s\S]{1,4096}?)\$\$|\$([^$\n]{1,4096}?)\$/g;let match,last=0,changed=false;const part=document.createDocumentFragment();
while((match=re.exec(text))){changed=true;part.append(document.createTextNode(text.slice(last,match.index)));const span=document.createElement('span');
try{katex.render(match[1]||match[2],span,{displayMode:!!match[1],throwOnError:false,trust:false,maxExpand:1000,strict:'warn'});}catch(e){span.textContent=match[0];}
part.append(span);last=re.lastIndex;}if(changed){part.append(document.createTextNode(text.slice(last)));node.replaceWith(part);}}
for(const link of document.querySelectorAll('a[href^="reader-source:"]')){link.addEventListener('click',e=>{e.preventDefault();console.log('reader-activate:'+link.getAttribute('href').slice(14));});link.addEventListener('mouseenter',()=>console.log('reader-hover:'+link.getAttribute('href').slice(14)));link.addEventListener('mouseleave',()=>console.log('reader-clear'));}
})();</script></body></html>)HTML").arg(body);
    rich_->setHtml(page, QUrl("qrc:/chat/"));
#endif
}
