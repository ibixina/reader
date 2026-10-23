#include "ui/WebPanel.h"
#include "ai/References.h"
#include "app/Application.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QHideEvent>
#include <QShowEvent>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
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

class ReaderWebPage final : public QWebEnginePage {
public:
    ReaderWebPage(reader::Application* app, QWebEngineProfile* profile, QWebEngineView* view)
        : QWebEnginePage(profile, view), app_(app), view_(view) {}

protected:
    QStringList chooseFiles(FileSelectionMode mode, const QStringList& oldFiles,
                            const QStringList& acceptedMimeTypes) override {
        const QFileInfo pdf(QString::fromStdString(app_->model.document.filePath));
        if ((mode != FileSelectOpen && mode != FileSelectOpenMultiple) ||
            !pdf.isFile() || !pdf.isReadable())
            return QWebEnginePage::chooseFiles(mode, oldFiles, acceptedMimeTypes);

        QStringList patterns;
        const QMimeDatabase mimeDatabase;
        for (const auto& acceptedType : acceptedMimeTypes) {
            const QString type = acceptedType.trimmed().toLower();
            if (type.startsWith('.')) {
                patterns.append('*' + type);
            } else if (type.endsWith("/*")) {
                const QString prefix = type.chopped(1);
                for (const auto& mime : mimeDatabase.allMimeTypes())
                    if (mime.name().startsWith(prefix)) patterns.append(mime.globPatterns());
            } else {
                patterns.append(mimeDatabase.mimeTypeForName(type).globPatterns());
            }
        }
        patterns.removeDuplicates();
        QStringList filters;
        if (!patterns.isEmpty())
            filters.append(tr("Accepted files (%1)").arg(patterns.join(' ')));
        filters.append(tr("All files (*)"));

        QFileDialog dialog(view_, tr("Select files"), pdf.absolutePath());
        dialog.setAcceptMode(QFileDialog::AcceptOpen);
        dialog.setFileMode(mode == FileSelectOpenMultiple ? QFileDialog::ExistingFiles
                                                         : QFileDialog::ExistingFile);
        dialog.setNameFilters(filters);
        dialog.setOption(QFileDialog::HideNameFilterDetails);
        dialog.selectFile(pdf.absoluteFilePath());
        if (dialog.exec() != QDialog::Accepted) return {};
        return dialog.selectedFiles();
    }

private:
    reader::Application* app_;
    QWebEngineView* view_;
};

} // namespace

struct WebPanel::Session {
    QPointer<QWebEngineView> view;
    ChatSite site = ChatSite::ChatGPT;
    QPointer<QTimer> timeout;
    // Fires while the tab is hidden to step it down to Frozen, and later to
    // Discarded when the chat can be reopened from its saved URL.
    QPointer<QTimer> lifecycle;
    QString question;
    QString sentQuestion;
    QString status;
    QString requestId;
    unsigned long generation = 0;
    unsigned long draftRevision = 0;
    unsigned long sentDraftRevision = 0;
    bool loaded = false;
    bool initialLoad = false;
    bool asking = false;
    bool clicked = false;
    bool closed = false;
    bool named = false;
    // Paper this chat belongs to (empty until one is open) and, once the
    // site assigns one, the conversation URL remembered for that paper.
    QString document;
    QString chatUrl;
    QString title;
    QUrl startUrl;
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

QString WebPanel::siteName(ChatSite site) {
    return site == ChatSite::Claude ? QStringLiteral("Claude") : QStringLiteral("ChatGPT");
}

QUrl WebPanel::homeUrl(ChatSite site) {
    return site == ChatSite::Claude ? QUrl("https://claude.ai/new")
                                    : QUrl("https://chatgpt.com/?temporary-chat=true");
}

std::optional<WebPanel::ChatSite> WebPanel::siteForUrl(const QUrl& url) {
    const QString host = url.host().toLower();
    auto under = [&](const char* domain) {
        const QString d = QString::fromLatin1(domain);
        return host == d || host.endsWith('.' + d);
    };
    if (under("claude.ai")) return ChatSite::Claude;
    if (under("chatgpt.com") || under("chat.openai.com")) return ChatSite::ChatGPT;
    return std::nullopt;
}

// DOM contract per site, as a JS object literal for the fill/send scripts:
// editor candidates in priority order, busy (stop) button, sent user
// messages, send buttons, the composer container that scopes the send
// lookup, and whether Enter submits when no send button exists at all.
QString WebPanel::claudeChatUrl(const QUrl& url) {
    if (siteForUrl(url) != ChatSite::Claude) return {};
    static const QRegularExpression chatPath("^/chat/([A-Za-z0-9-]+)/?$");
    const auto match = chatPath.match(url.path());
    return match.hasMatch() ? "https://claude.ai/chat/" + match.captured(1) : QString();
}

QString WebPanel::chatTitle(const QString& pageTitle) {
    QString title = pageTitle.trimmed();
    for (const QString suffix : {" - Claude", " | Claude", " — Claude"})
        if (title.endsWith(suffix)) title.chop(suffix.size());
    title = title.trimmed();
    return title == "Claude" ? QString() : title;
}

QString WebPanel::siteConfig(ChatSite site) {
    QStringList editors;
    QString stop, users, send, scope;
    bool enter = false;
    if (site == ChatSite::Claude) {
        editors = {"div.ProseMirror[contenteditable=\"true\"]", "[data-testid=\"chat-input\"]",
                   "fieldset [contenteditable=\"true\"]", "[contenteditable=\"true\"]",
                   "fieldset textarea"};
        stop = "button[aria-label=\"Stop response\"], button[aria-label=\"Stop Response\"], "
               "button[aria-label^=\"Stop\"]";
        users = "[data-testid=\"user-message\"]";
        send = "button[aria-label=\"Send message\"], button[aria-label=\"Send Message\"], "
               "button[aria-label*=\"Send\"], button[type=\"submit\"]";
        scope = "fieldset, form";
        enter = true;
    } else {
        editors = {"#prompt-textarea", "[data-testid=\"composer-text-input\"]",
                   "form [contenteditable=\"true\"], form textarea", "[contenteditable=\"true\"]"};
        stop = "[data-testid=\"stop-button\"], button[aria-label=\"Stop generating\"]";
        users = "[data-message-author-role=\"user\"]";
        send = "#composer-submit-button, [data-testid=\"send-button\"], "
               "[data-testid=\"composer-send-button\"], button[aria-label=\"Send prompt\"], "
               "button[aria-label*=\"Send\"], button[type=\"submit\"]";
        scope = "form";
    }
    QStringList quotedEditors;
    for (const QString& e : editors) quotedEditors << jsonQuoted(e);
    return QString("{editors: [%1], stop: %2, users: %3, send: %4, scope: %5, enter: %6}")
        .arg(quotedEditors.join(", "), jsonQuoted(stop), jsonQuoted(users), jsonQuoted(send),
             jsonQuoted(scope), enter ? "true" : "false");
}

WebPanel::WebPanel(reader::Application* app, QWidget* parent)
    : QWidget(parent), app_(app) {
    {
        QSettings settings;
        if (settings.value("reader/chatSite").toString() == "claude")
            defaultSite_ = ChatSite::Claude;
    }
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
    auto* home = new QPushButton(navBar_);
    homeButton_ = home;
    auto* copy = new QPushButton("Copy prompt", navBar_);
    copy->setToolTip("Copy selection + question as a chat-ready prompt");
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
    auto* corner = new QWidget(tabs_);
    auto* cornerLayout = new QHBoxLayout(corner);
    cornerLayout->setContentsMargins(0, 0, 0, 0);
    siteBox_ = new QComboBox(corner);
    siteBox_->setObjectName("browserChatSite");
    siteBox_->addItem("ChatGPT", int(ChatSite::ChatGPT));
    siteBox_->addItem("Claude", int(ChatSite::Claude));
    siteBox_->setToolTip("Chat site for this tab and for new chats");
    auto* newChat = new QPushButton("New chat", corner);
    newChat->setObjectName("browserNewChat");
    newChat->setToolTip("Open another chat (Ctrl+T). Double-click a tab to name it.");
    chatsButton_ = new QPushButton("Chats", corner);
    chatsButton_->setObjectName("browserChatListButton");
    chatsButton_->setToolTip("Claude chats saved for this paper");
    chatsButton_->hide();
    cornerLayout->addWidget(chatsButton_);
    cornerLayout->addWidget(siteBox_);
    cornerLayout->addWidget(newChat);
    tabs_->setCornerWidget(corner, Qt::TopRightCorner);
    connect(siteBox_, &QComboBox::activated, this, [this](int index) {
        setCurrentSite(static_cast<ChatSite>(siteBox_->itemData(index).toInt()));
    });
    // Session list: shown over the tabs when a paper with saved Claude chats
    // opens, so the reader picks one up again or starts a new one.
    listPage_ = new QWidget(this);
    listPage_->setObjectName("browserChatListPage");
    auto* listLayout = new QVBoxLayout(listPage_);
    listLayout->setContentsMargins(0, 6, 0, 0);
    auto* listHeader = new QHBoxLayout();
    auto* listTitle = new QLabel("Claude chats for this paper", listPage_);
    QFont listTitleFont = listTitle->font();
    listTitleFont.setBold(true);
    listTitle->setFont(listTitleFont);
    listBack_ = new QPushButton("Back to chat", listPage_);
    listBack_->setObjectName("browserChatListBack");
    auto* listNew = new QPushButton("New chat", listPage_);
    listNew->setObjectName("browserChatListNew");
    listNew->setToolTip("Start a new Claude chat for this paper");
    listHeader->addWidget(listTitle, 1);
    listHeader->addWidget(listBack_);
    listHeader->addWidget(listNew);
    listLayout->addLayout(listHeader);
    chatList_ = new QListWidget(listPage_);
    chatList_->setObjectName("browserChatList");
    chatList_->setContextMenuPolicy(Qt::CustomContextMenu);
    chatList_->setToolTip("Click to reopen. Right-click to remove from this list "
                          "(the chat stays on claude.ai).");
    listLayout->addWidget(chatList_, 1);
    stack_ = new QStackedWidget(this);
    stack_->addWidget(tabs_);
    stack_->addWidget(listPage_);
    stack_->setCurrentWidget(tabs_);
    layout->addWidget(stack_, 1);
    auto openItem = [this](QListWidgetItem* item) {
        if (item && !item->data(Qt::UserRole).toString().isEmpty())
            openSavedChat(item->data(Qt::UserRole).toString());
    };
    connect(chatList_, &QListWidget::itemClicked, this, openItem);
    connect(chatList_, &QListWidget::itemActivated, this, openItem);
    connect(chatList_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = chatList_->itemAt(pos);
        const QString url = item ? item->data(Qt::UserRole).toString() : QString();
        if (url.isEmpty() || !app_->webChats) return;
        QMenu menu(this);
        QAction* open = menu.addAction("Open");
        QAction* remove = menu.addAction("Remove from list");
        QAction* chosen = menu.exec(chatList_->viewport()->mapToGlobal(pos));
        if (chosen == open) {
            openSavedChat(url);
        } else if (chosen == remove) {
            app_->webChats->deleteWebChat(document_.toStdString(), url.toStdString());
            rebuildChatList();
        }
    });
    connect(listNew, &QPushButton::clicked, this, &WebPanel::startChatFromList);
    connect(listBack_, &QPushButton::clicked, this, &WebPanel::hideChatList);
    connect(chatsButton_, &QPushButton::clicked, this, &WebPanel::showChatList);
    connect(newChat, &QPushButton::clicked, this, &WebPanel::newSession);
    connect(tabs_, &QTabWidget::currentChanged, this, &WebPanel::activateSession);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &WebPanel::closeSession);
    connect(tabs_->tabBar(), &QTabBar::tabBarDoubleClicked, this, &WebPanel::renameSession);
    connect(question_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (current_) {
            current_->question = text;
            ++current_->draftRevision;
        }
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
        if (auto* view = currentView()) view->load(homeUrl(currentSite()));
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
    syncSiteControls();
    if (isVisible()) {
        wake(session);
        ensureLoaded();
    }
}

WebPanel::ChatSite WebPanel::currentSite() const {
    return current_ ? current_->site : defaultSite_;
}

void WebPanel::syncSiteControls() {
    const ChatSite site = currentSite();
    if (siteBox_) {
        const QSignalBlocker blocker(siteBox_);
        siteBox_->setCurrentIndex(siteBox_->findData(int(site)));
    }
    if (homeButton_) homeButton_->setText(homeUrl(site).host());
}

// Picking a site makes it the default for new chats and switches the
// current tab to it (Back returns to the previous conversation).
void WebPanel::setCurrentSite(ChatSite site) {
    defaultSite_ = site;
    QSettings settings;
    settings.setValue("reader/chatSite", site == ChatSite::Claude ? "claude" : "chatgpt");
    if (current_ && current_->site != site) {
        cancel(current_);
        current_->site = site;
        if (current_->loaded) current_->view->load(homeUrl(site));
        else if (isVisible()) ensureLoaded();
    }
    syncSiteControls();
}

void WebPanel::newSession() {
    if (shuttingDown_) return;
    hideChatList();
    createSession(defaultSite_);
}

WebPanel::SessionPtr WebPanel::createSession(ChatSite site, const QString& url,
                                             const QString& title, bool named) {
    auto session = std::make_shared<Session>();
    session->site = site;
    session->document = document_;
    session->chatUrl = claudeChatUrl(QUrl(url));
    session->startUrl = QUrl(url);
    session->title = title;
    session->named = named;
    auto* view = new QWebEngineView(tabs_);
    view->setObjectName(QString("browserChatView_%1").arg(++nextSession_));
    view->setPage(new ReaderWebPage(app_, profile_, view));
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
            : siteName(session->site) +
                  " isn't ready to send yet — your question is kept. Sign in or reload and retry.");
    });
    // Hidden chat pages keep a full renderer running site scripts. Step them
    // down after a grace period, never below Qt's recommended state (which
    // stays Active while visible or audible, and Frozen while a composer
    // holds unsent input).
    session->lifecycle = new QTimer(view);
    session->lifecycle->setSingleShot(true);
    connect(session->lifecycle, &QTimer::timeout, this, [this, session] { stepLifecycle(session); });
    connect(view->page(), &QWebEnginePage::recommendedStateChanged, this,
            [this, session](QWebEnginePage::LifecycleState state) {
                if (shuttingDown_ || session->closed || !session->lifecycle) return;
                if (state == QWebEnginePage::LifecycleState::Active) {
                    session->lifecycle->stop();
                    return;
                }
                if (!session->lifecycle->isActive()) session->lifecycle->start(kFreezeDelayMs);
            });
    connect(view, &QWebEngineView::urlChanged, this, [this, session](const QUrl& url) {
        if (shuttingDown_ || session->closed) return;
        if (const auto site = siteForUrl(url); site && *site != session->site) {
            session->site = *site;
            if (current_ == session) syncSiteControls();
        }
        if (const QString chat = claudeChatUrl(url); !chat.isEmpty() && chat != session->chatUrl) {
            session->chatUrl = chat;
            rememberChat(session);
        }
    });
    connect(view, &QWebEngineView::loadStarted, this, [this, session] {
        if (shuttingDown_ || session->closed) return;
        const bool initialLoad = session->initialLoad;
        session->initialLoad = false;
        session->loaded = true;
        if (session->asking && !initialLoad) cancel(session);
        if (!session->asking) setStatus(session, "Loading " + siteName(session->site) + "…");
    });
    connect(view, &QWebEngineView::loadFinished, this, [this, session](bool ok) {
        if (shuttingDown_ || session->closed || session->asking) return;
        const QString name = siteName(session->site);
        setStatus(session, ok ? name + " ready — your login is shared across chats."
                              : "Couldn't load " + name + " — check the connection and reload.");
    });
    connect(view, &QWebEngineView::titleChanged, this, [this, session](const QString& pageTitle) {
        if (shuttingDown_ || session->closed) return;
        const QString title = session->site == ChatSite::Claude ? chatTitle(pageTitle) : pageTitle;
        if (title.isEmpty() || title == "ChatGPT" || title == "Claude") return;
        if (session->named) return;
        session->title = title;
        rememberChat(session);
        const int index = tabs_->indexOf(session->view);
        if (index < 0) return;
        tabs_->setTabText(index, title.left(32));
        tabs_->setTabToolTip(index, title);
    });
    sessions_.push_back(session);
    const int index = tabs_->addTab(
        view, title.isEmpty() ? QString("Chat %1").arg(nextSession_) : title.left(32));
    if (!title.isEmpty()) tabs_->setTabToolTip(index, title);
    tabs_->setCurrentIndex(index);
    activateSession(index);
    return session;
}

void WebPanel::renameSession(int index) {
    const auto session = sessionAt(index);
    if (!session) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, "Rename chat", "Chat name:",
        QLineEdit::Normal, tabs_->tabText(index), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    session->named = true;
    session->title = name;
    tabs_->setTabText(index, name.left(32));
    tabs_->setTabToolTip(index, name);
    rememberChat(session);
}

void WebPanel::closeSession(int index) {
    const auto session = sessionAt(index);
    if (!session) return;
    removeSession(session);
    if (tabs_->count() > 0) return;
    // Closing the last tab offers this paper's saved chats when it has any.
    if (app_->webChats && !document_.isEmpty() &&
        !app_->webChats->webChatsFor(document_.toStdString(), "claude").empty())
        showChatList();
    else
        newSession();
}

void WebPanel::removeSession(const SessionPtr& session) {
    cancel(session);
    session->closed = true;
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session), sessions_.end());
    if (const int index = tabs_->indexOf(session->view); index >= 0) tabs_->removeTab(index);
    if (session->view) session->view->deleteLater();
    if (current_ == session) current_.reset();
    if (tabs_->count() > 0) activateSession(tabs_->currentIndex());
}

// Saved only once the site has assigned a conversation URL, so chats that
// were opened but never used don't fill the list.
void WebPanel::rememberChat(const SessionPtr& session) {
    if (!app_->webChats || session->site != ChatSite::Claude || session->chatUrl.isEmpty() ||
        session->document.isEmpty())
        return;
    reader::WebChat chat;
    chat.site = "claude";
    chat.url = session->chatUrl.toStdString();
    chat.title = session->title.toStdString();
    chat.named = session->named;
    app_->webChats->saveWebChat(session->document.toStdString(), chat);
}

void WebPanel::setDocument(const QString& documentId) {
    if (shuttingDown_ || documentId == document_) return;
    document_ = documentId;
    std::vector<SessionPtr> stale;
    for (const auto& session : sessions_) {
        if (session->site != ChatSite::Claude) continue;
        if (session->document.isEmpty()) {
            session->document = document_;
            rememberChat(session);
        } else if (session->document != document_) {
            stale.push_back(session);
        }
    }
    for (const auto& session : stale) removeSession(session);
    chatsButton_->setVisible(!document_.isEmpty() && app_->webChats);
    const bool hasSaved = app_->webChats && !document_.isEmpty() &&
                          !app_->webChats->webChatsFor(document_.toStdString(), "claude").empty();
    if (hasSaved && defaultSite_ == ChatSite::Claude) {
        showChatList();
        return;
    }
    hideChatList();
    if (tabs_->count() == 0) newSession();
}

bool WebPanel::chatListVisible() const {
    return stack_ && stack_->currentWidget() == listPage_;
}

void WebPanel::rebuildChatList() {
    chatList_->clear();
    std::vector<reader::WebChat> chats;
    if (app_->webChats && !document_.isEmpty())
        chats = app_->webChats->webChatsFor(document_.toStdString(), "claude");
    for (const auto& chat : chats) {
        const QString title = chat.title.empty() ? QString("Untitled chat")
                                                 : QString::fromStdString(chat.title);
        const QString when = QLocale().toString(
            QDateTime::fromMSecsSinceEpoch(chat.lastOpened), QLocale::ShortFormat);
        auto* item = new QListWidgetItem(title + "\n" + when, chatList_);
        item->setData(Qt::UserRole, QString::fromStdString(chat.url));
        item->setToolTip(QString::fromStdString(chat.url));
    }
    if (chats.empty()) {
        auto* item = new QListWidgetItem("No saved Claude chats for this paper yet.", chatList_);
        item->setFlags(Qt::NoItemFlags);
    }
}

void WebPanel::showChatList() {
    if (shuttingDown_) return;
    rebuildChatList();
    listBack_->setVisible(tabs_->count() > 0);
    stack_->setCurrentWidget(listPage_);
}

void WebPanel::hideChatList() {
    if (stack_) stack_->setCurrentWidget(tabs_);
}

void WebPanel::openSavedChat(const QString& url) {
    if (shuttingDown_) return;
    hideChatList();
    const QString chat = claudeChatUrl(QUrl(url));
    if (chat.isEmpty()) return;
    for (const auto& session : sessions_) {
        if (session->chatUrl != chat) continue;
        tabs_->setCurrentWidget(session->view);
        return;
    }
    QString title;
    bool named = false;
    if (app_->webChats) {
        for (const auto& saved : app_->webChats->webChatsFor(document_.toStdString(), "claude")) {
            if (saved.url != chat.toStdString()) continue;
            title = QString::fromStdString(saved.title);
            named = saved.named;
        }
        app_->webChats->touchWebChat(document_.toStdString(), chat.toStdString());
    }
    createSession(ChatSite::Claude, chat, title, named);
}

// A blank Claude tab of this paper is reused rather than stacking another.
void WebPanel::startChatFromList() {
    if (shuttingDown_) return;
    hideChatList();
    for (const auto& session : sessions_) {
        if (session->site != ChatSite::Claude || !session->chatUrl.isEmpty() || session->asking ||
            session->document != document_)
            continue;
        session->startUrl = QUrl();
        if (session->loaded && session->view->url() != homeUrl(ChatSite::Claude))
            session->view->load(homeUrl(ChatSite::Claude));
        tabs_->setCurrentWidget(session->view);
        return;
    }
    createSession(ChatSite::Claude);
}

void WebPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (current_) wake(current_);
    ensureLoaded();
    updateNavTimer();
}

void WebPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    updateNavTimer();
}

void WebPanel::setRevealPolling(bool enabled) {
    revealPolling_ = enabled;
    updateNavTimer();
}

void WebPanel::updateNavTimer() {
    if (!navTimer_) return;
    const bool on = revealPolling_ && isVisible() && !shuttingDown_;
    if (on == navTimer_->isActive()) return;
    if (on) navTimer_->start();
    else navTimer_->stop();
}

void WebPanel::ensureLoaded() {
    if (!current_ || current_->loaded) return;
    current_->loaded = true;
    current_->initialLoad = true;
    current_->view->load(current_->startUrl.isValid() ? current_->startUrl
                                                       : homeUrl(current_->site));
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
    for (const auto& session : sessions_) {
        session->question.clear();
        ++session->draftRevision;
    }
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

QString WebPanel::fillScript(const QString& prompt, const QString& requestId, ChatSite site) {
    return QStringLiteral(R"JS(
(function(payload, id, site) {
    try {
        if (document.querySelector(site.stop)) return 'busy';
        var ed = null;
        for (var selector of site.editors) {
            ed = document.querySelector(selector);
            if (ed) break;
        }
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
})(%1, %2, %3)
)JS").arg(jsonQuoted(prompt), jsonQuoted(requestId), siteConfig(site));
}

QString WebPanel::sendScript(const QString& requestId, ChatSite site) {
    return QStringLiteral(R"JS(
(function(id, site) {
    try {
        var request = window.__paperReaderAsk;
        if (!request || request.id !== id) return 'cancelled';
        var ed = null;
        for (var selector of site.editors) {
            ed = document.querySelector(selector);
            if (ed) break;
        }
        var input = ed ? (ed.querySelector('.ProseMirror, textarea, [contenteditable="true"]') || ed) : null;
        var text = input ? (input.value === undefined ? (input.innerText || input.textContent || '') : input.value) : '';
        var users = document.querySelectorAll(site.users).length;
        var stop = document.querySelector(site.stop);
        if (request.clicked) {
            if (users > request.users || (input && !text.trim() && (stop || location.href !== request.url)))
                return 'sent';
            return 'waiting-confirmation';
        }
        if (location.href !== request.url) return 'cancelled';
        if (stop) return 'busy';
        if (!input) return 'no-editor';
        if (!text.trim()) return 'empty-editor';
        var scope = input.closest(site.scope) || document;
        var buttons = scope.querySelectorAll(site.send);
        for (var button of buttons) {
            if (button.disabled || button.getAttribute('aria-disabled') === 'true' || !button.getClientRects().length)
                continue;
            request.clicked = true;
            request.users = users;
            request.url = location.href;
            button.click();
            return 'clicked';
        }
        // Composers without any send button submit on Enter. A present but
        // disabled button means the site is not ready: keep waiting.
        if (!buttons.length && site.enter) {
            request.clicked = true;
            request.users = users;
            request.url = location.href;
            input.focus();
            input.dispatchEvent(new KeyboardEvent('keydown', {key: 'Enter', code: 'Enter',
                keyCode: 13, which: 13, bubbles: true, cancelable: true}));
            return 'clicked';
        }
        return 'waiting-send';
    } catch (error) { return 'error:' + error; }
})(%1, %2)
)JS").arg(jsonQuoted(requestId), siteConfig(site));
}

void WebPanel::stepLifecycle(const SessionPtr& session) {
    if (shuttingDown_ || session->closed || !session->view) return;
    QWebEnginePage* page = session->view->page();
    using State = QWebEnginePage::LifecycleState;
    const State recommended = page->recommendedState();
    if (session->asking || recommended == State::Active) return;
    // A discarded page reloads its current URL when shown again, so only a
    // page sitting on a saved Claude conversation is discarded; a temporary
    // ChatGPT chat would be lost.
    const bool discardable = !claudeChatUrl(page->url()).isEmpty();
    if (page->lifecycleState() == State::Active) {
        page->setLifecycleState(State::Frozen);
        if (discardable) session->lifecycle->start(kDiscardDelayMs);
        return;
    }
    if (page->lifecycleState() == State::Frozen && recommended == State::Discarded && discardable)
        page->setLifecycleState(State::Discarded);
}

void WebPanel::wake(const SessionPtr& session) {
    if (!session->view) return;
    if (session->lifecycle) session->lifecycle->stop();
    QWebEnginePage* page = session->view->page();
    if (page->lifecycleState() != QWebEnginePage::LifecycleState::Active)
        page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
}

void WebPanel::ask() {
    // Asking from the session list starts a new chat for this paper.
    if (chatListVisible()) startChatFromList();
    else if (!current_) newSession();
    if (!current_ || current_->asking) return;
    ensureLoaded();
    const auto session = current_;
    // Scripts never run in a frozen page: an ask from a hidden pane wakes it.
    wake(session);
    session->asking = true;
    session->clicked = false;
    session->sentQuestion = question_->text();
    session->sentDraftRevision = session->draftRevision;
    session->requestId = QString::number(++nextRequest_);
    const unsigned long generation = ++session->generation;
    askButton_->setEnabled(false);
    setStatus(session, "Sending to " + siteName(session->site) + "…");
    session->timeout->start();
    tryFill(session, buildPrompt(session->sentQuestion.trimmed()), generation);
}

void WebPanel::tryFill(const SessionPtr& session, const QString& prompt, unsigned long generation) {
    if (session->closed || !session->asking || session->generation != generation || !session->view) return;
    QPointer<WebPanel> guard(this);
    session->view->page()->runJavaScript(fillScript(prompt, session->requestId, session->site),
        [this, guard, session, prompt, generation](const QVariant& result) {
            if (!guard || session->closed || !session->asking || session->generation != generation) return;
            const QString status = result.toString();
            if (status == "filled") {
                // Let the site's input handler enable its send button first.
                QTimer::singleShot(100, this, [this, session, generation] { trySend(session, generation); });
            } else if (status == "no-editor" || status == "busy") {
                QTimer::singleShot(250, this, [this, session, prompt, generation] { tryFill(session, prompt, generation); });
            } else {
                finishAsk(session, false, "Couldn't prepare the " + siteName(session->site) +
                                              " composer — your question is kept. Reload and retry.");
            }
        });
}

void WebPanel::trySend(const SessionPtr& session, unsigned long generation) {
    if (session->closed || !session->asking || session->generation != generation || !session->view) return;
    QPointer<WebPanel> guard(this);
    session->view->page()->runJavaScript(sendScript(session->requestId, session->site),
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
                    : siteName(session->site) +
                          " couldn't send the prompt — your question is kept. Reload and retry.");
            }
        });
}

void WebPanel::finishAsk(const SessionPtr& session, bool sent, const QString& status) {
    session->asking = false;
    ++session->generation;
    session->timeout->stop();
    if (!sent && session->view)
        session->view->page()->runJavaScript("window.__paperReaderAsk = null;");
    if (sent && session->draftRevision == session->sentDraftRevision &&
        session->question == session->sentQuestion) {
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
    context_->setPlainText(context_->toPlainText() + "  · prompt copied, paste into " + siteName(currentSite()));
    if (auto* view = currentView()) view->setFocus();
}

void WebPanel::focusQuestion() {
    question_->setFocus(Qt::OtherFocusReason);
}

void WebPanel::focusQuestionWithSeed(const QString& seed) {
    question_->setFocus(Qt::OtherFocusReason);
    if (!seed.isEmpty()) question_->insert(seed);
}
