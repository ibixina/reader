// Exercises composer submission and independent sessions against local pages.
#include "app/Application.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QJsonDocument>
#include <QJsonObject>
#include <memory>
#include <iostream>

#include "ui/WebPanel.h"

static int failures = 0;
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n"; \
            ++failures; \
        } \
    } while (0)

static QVariant runJsSync(QWebEngineView& view, const QString& script, int timeoutMs = 15000) {
    struct State {
        QVariant result;
    };
    const auto state = std::make_shared<State>();
    QEventLoop loop;
    const QPointer<QEventLoop> loopGuard(&loop);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    view.page()->runJavaScript(script, [state, loopGuard](const QVariant& r) {
        if (!loopGuard) return;
        state->result = r;
        loopGuard->quit();
    });
    timer.start(timeoutMs);
    loop.exec();
    return state->result;
}

class LocalRequestInterceptor final : public QWebEngineUrlRequestInterceptor {
public:
    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        const QString host = info.requestUrl().host();
        if (!host.isEmpty() && host != "reader.test") info.block(true);
    }
};

template <typename Predicate>
static bool waitFor(QApplication& app, Predicate predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        app.processEvents(QEventLoop::AllEvents, 25);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(5);
    }
    return predicate();
}

static void loadMock(QWebEngineView& view, const QString& html) {
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&view, &QWebEngineView::loadFinished, &loop, &QEventLoop::quit);
    view.setHtml(html, QUrl("http://reader.test/chat"));
    timer.start(10000);
    loop.exec();
    CHECK(timer.isActive());
}

static void pressEnter(QLineEdit& edit) {
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(&edit, &press);
    QApplication::sendEvent(&edit, &release);
}

static QString chatMock() {
    return QStringLiteral(R"HTML(
        <fieldset>
          <div class="ProseMirror" contenteditable="true"></div>
          <button aria-label="Send message" disabled aria-disabled="true">Send</button>
        </fieldset>
        <script>
          window.__fills = 0; window.__sends = 0; window.__enableDelay = 350;
          var editor = document.querySelector('.ProseMirror');
          var button = document.querySelector('fieldset button');
          editor.addEventListener('input', function() {
            // ProseMirror can emit several input events for a multiline insertion.
            clearTimeout(window.__inputTimer);
            window.__inputTimer = setTimeout(function() {
              ++window.__fills;
              if (window.__enableDelay < 0) return;
              setTimeout(function() { button.disabled = false; button.removeAttribute('aria-disabled'); },
                         window.__enableDelay);
            }, 0);
          });
          button.addEventListener('click', function() {
            ++window.__sends;
            window.__prompt = editor.innerText;
            setTimeout(function() {
              var message = document.createElement('div');
              message.setAttribute('data-testid', 'user-message');
              message.textContent = window.__prompt;
              document.body.appendChild(message);
              editor.textContent = '';
              button.disabled = true;
            }, 150);
          });
        </script>
    )HTML");
}

static void testFilePicker(QApplication& qt) {
    QTemporaryDir files(QDir::homePath() + "/uploads-XXXXXX");
    CHECK(files.isValid());
    if (!files.isValid()) return;
    const QString firstPdf = files.filePath("Current paper.pdf");
    const QString secondPdf = files.filePath("Different paper.PDF");
    QDir().mkpath(files.filePath("other"));
    const QString otherFile = files.filePath("other/Notes.txt");
    for (const auto& path : {firstPdf, secondPdf, otherFile}) {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write("Upload fixture");
    }

    reader::Application app;
    app.model.document.filePath = firstPdf.toStdString();
    LocalRequestInterceptor interceptor;
    WebPanel panel(&app);
    panel.findChild<QWebEngineProfile*>()->setUrlRequestInterceptor(&interceptor);
    auto* first = panel.currentView();
    const QString uploadMock = QStringLiteral(R"HTML(
        <input type="file" id="upload" multiple>
        <script>
          window.__changes = 0;
          document.getElementById('upload').addEventListener('change', function() {
            ++window.__changes;
          });
        </script>
    )HTML");
    loadMock(*first, uploadMock);
    panel.resize(750, 850);
    panel.show();
    qt.processEvents();

    auto choose = [&](QWebEngineView& view, const QString& expectedPdf,
                      const QString& choice, QFileDialog::FileMode mode) {
        panel.activateWindow();
        view.setFocus();
        runJsSync(view,
            "window.__pickerReady = false;"
            "requestAnimationFrame(() => requestAnimationFrame(() => window.__pickerReady = true))");
        CHECK(waitFor(qt, [&] { return runJsSync(view, "window.__pickerReady").toBool(); }));
        const auto coordinates = runJsSync(view,
            "(function() { var r = document.getElementById('upload').getBoundingClientRect();"
            " return [r.left + 10, r.top + r.height / 2]; })()").toList();
        CHECK(coordinates.size() == 2);
        if (coordinates.size() != 2) return;
        const QPointF point(coordinates[0].toDouble(), coordinates[1].toDouble());
        auto* target = view.focusProxy() ? view.focusProxy() : &view;
        bool seen = false;
        QTimer observer;
        observer.setInterval(10);
        QObject::connect(&observer, &QTimer::timeout, &panel, [&] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            observer.stop();
            seen = true;
            CHECK(dialog->fileMode() == mode);
            CHECK(dialog->directory().absolutePath() == QFileInfo(expectedPdf).absolutePath());
            CHECK(dialog->selectedFiles() == QStringList{expectedPdf});
            if (choice.isEmpty()) {
                dialog->reject();
            } else {
                dialog->setDirectory(QFileInfo(choice).absolutePath());
                dialog->selectFile(choice);
                // selectFile leaves the name field alone while it has focus
                // in a visible dialog (Qt 6.10), so accept() would look for
                // the old name in the new folder and stop on a nested "File
                // not found" box. Type the name, as a user would.
                if (auto* name = dialog->findChild<QLineEdit*>("fileNameEdit"))
                    name->setText(QFileInfo(choice).fileName());
                QMetaObject::invokeMethod(dialog, "accept");
            }
        });
        QTimer timeout;
        timeout.setSingleShot(true);
        // Close every modal, nested ones included (a message box raised by
        // the picker): a stuck dialog must fail the check, never hang.
        QObject::connect(&timeout, &QTimer::timeout, &panel, [] {
            for (int i = 0; i < 4; ++i)
                if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    modal->reject();
        });
        observer.start();
        timeout.start(5000);
        QMouseEvent press(QEvent::MouseButtonPress, point, target->mapToGlobal(point.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, point, target->mapToGlobal(point.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QApplication::sendEvent(target, &release);
        CHECK(waitFor(qt, [&] { return seen; }));
        if (!seen) std::cout << "Picker did not open for " << choice.toStdString() << '\n';
    };
    auto filenames = [](QWebEngineView& view) {
        return runJsSync(view,
            "Array.from(document.getElementById('upload').files, f => f.name).join('|')").toString();
    };

    // Preselection never attaches anything until the user confirms.
    choose(*first, firstPdf, {}, QFileDialog::ExistingFiles);
    CHECK(filenames(*first).isEmpty());
    CHECK(runJsSync(*first, "window.__changes").toInt() == 0);
    choose(*first, firstPdf, firstPdf, QFileDialog::ExistingFiles);
    CHECK(waitFor(qt, [&] { return filenames(*first) == "Current paper.pdf"; }));

    // The suggested PDF can be replaced with a non-PDF in another folder.
    choose(*first, firstPdf, otherFile, QFileDialog::ExistingFiles);
    CHECK(waitFor(qt, [&] { return filenames(*first) == "Notes.txt"; }));

    // Existing and new chats use the paper open when the picker is invoked.
    app.model.document.filePath = secondPdf.toStdString();
    choose(*first, secondPdf, secondPdf, QFileDialog::ExistingFiles);
    CHECK(waitFor(qt, [&] { return filenames(*first) == "Different paper.PDF"; }));
    panel.newSession();
    auto* second = panel.currentView();
    loadMock(*second, uploadMock);
    runJsSync(*second,
        "document.getElementById('upload').multiple = false;"
        "document.getElementById('upload').accept = '.pdf,application/pdf'");
    choose(*second, secondPdf, secondPdf, QFileDialog::ExistingFile);
    CHECK(waitFor(qt, [&] { return filenames(*second) == "Different paper.PDF"; }));
}

static void testSessions(QApplication& qt) {
    reader::Application app;
    app.model.document.title = "Selection paper";
    reader::ContextReference ref;
    ref.displayName = "Selected passage";
    ref.anchor.page = 2;
    ref.extractedText = "Original selection about amortization.";
    app.context.setCurrentSelection(ref);
    LocalRequestInterceptor interceptor;
    WebPanel panel(&app);
    auto* profile = panel.findChild<QWebEngineProfile*>();
    CHECK(profile);
    if (!profile) return;
    profile->setUrlRequestInterceptor(&interceptor);
    auto* tabs = panel.findChild<QTabWidget*>("browserSessions");
    auto* question = panel.findChild<QLineEdit*>("browserQuestion");
    auto* ask = panel.findChild<QPushButton*>("browserAskButton");
    auto* newChat = panel.findChild<QPushButton*>("browserNewChat");
    CHECK(tabs && question && ask && newChat);
    if (!tabs || !question || !ask || !newChat) return;
    CHECK(tabs->count() == 1);
    auto* first = panel.currentView();
    loadMock(*first, chatMock());
    panel.resize(750, 850);
    panel.show();
    qt.processEvents();

    // One Enter waits for async enablement, submits once, and retains the draft until confirmed.
    question->setText("Explain this selection");
    pressEnter(*question);
    CHECK(!ask->isEnabled());
    CHECK(question->text() == "Explain this selection");
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(question->text().isEmpty());
    CHECK(runJsSync(*first, "window.__fills").toInt() == 1);
    CHECK(runJsSync(*first, "window.__sends").toInt() == 1);
    QString prompt = runJsSync(*first, "window.__prompt").toString();
    CHECK(prompt.contains("Selection paper"));
    CHECK(prompt.contains("page 3"));
    CHECK(prompt.contains("Original selection about amortization."));
    CHECK(prompt.contains("Question: Explain this selection"));

    // Keep one request pending while a second tab submits a different immutable context.
    runJsSync(*first, "window.__enableDelay = -1");
    question->setText("First topic");
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return runJsSync(*first, "window.__fills").toInt() == 2; }));
    newChat->click();
    CHECK(tabs->count() == 2);
    auto* second = panel.currentView();
    CHECK(first != second && first->page() != second->page());
    CHECK(first->page()->profile() == second->page()->profile());
    loadMock(*second, chatMock());
    CHECK(question->text().isEmpty());
    CHECK(ask->isEnabled());
    runJsSync(*first, "document.cookie='shared-login=yes;path=/'");
    CHECK(runJsSync(*second, "document.cookie").toString().contains("shared-login=yes"));
    ref.extractedText = "Different selection about convergence.";
    app.context.setCurrentSelection(ref);
    question->setText("Second topic");
    pressEnter(*question);
    question->setText("Unsent follow-up");
    runJsSync(*first, "button.disabled = false; button.removeAttribute('aria-disabled')");
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(question->text() == "Unsent follow-up");
    CHECK(runJsSync(*second, "window.__sends").toInt() == 1);
    prompt = runJsSync(*second, "window.__prompt").toString();
    CHECK(prompt.contains("Second topic"));
    CHECK(prompt.contains("Different selection about convergence."));
    tabs->setCurrentWidget(first);
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(question->text().isEmpty());
    CHECK(runJsSync(*first, "window.__sends").toInt() == 2);
    prompt = runJsSync(*first, "window.__prompt").toString();
    CHECK(prompt.contains("First topic"));
    CHECK(prompt.contains("Original selection about amortization."));
    CHECK(!prompt.contains("Different selection about convergence."));
    tabs->setCurrentWidget(second);
    CHECK(question->text() == "Unsent follow-up");

    // Moving tabs must preserve the mapping between the composer and its web page.
    tabs->tabBar()->moveTab(tabs->indexOf(second), 0);
    CHECK(panel.currentView() == second);
    question->setText("After moving tabs");
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(runJsSync(*second, "window.__sends").toInt() == 2);
    CHECK(runJsSync(*first, "window.__sends").toInt() == 2);

    // Timeout preserves the question; enabling later must not send a stale request.
    auto* timeout = second->findChild<QTimer*>("browserAskTimeout");
    CHECK(timeout);
    timeout->setInterval(400);
    runJsSync(*second, "window.__enableDelay = -1");
    question->setText("Retry this question");
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(question->text() == "Retry this question");
    runJsSync(*second, "button.disabled = false; button.removeAttribute('aria-disabled')");
    QEventLoop settle;
    QTimer::singleShot(300, &settle, &QEventLoop::quit);
    settle.exec();
    CHECK(runJsSync(*second, "window.__sends").toInt() == 2);
    timeout->setInterval(20000);
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(question->text().isEmpty());
    CHECK(runJsSync(*second, "window.__sends").toInt() == 3);

    // Closing a pending tab cannot submit into another tab, and the last close creates a fresh chat.
    question->setText("Closing topic");
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return runJsSync(*second, "window.__fills").toInt() == 5; }));
    QPointer<QWebEngineView> closed(second);
    panel.closeSession(tabs->indexOf(second));
    CHECK(tabs->count() == 1);
    CHECK(panel.currentView() == first);
    CHECK(waitFor(qt, [&] { return closed.isNull(); }));
    CHECK(ask->isEnabled());
    CHECK(question->text().isEmpty());
    CHECK(runJsSync(*first, "window.__sends").toInt() == 2);
    panel.closeSession(0);
    CHECK(tabs->count() == 1);
    CHECK(panel.currentView() != first);
    CHECK(panel.currentView()->page()->profile() == profile);
    // Changing selections clears the old draft. A late confirmation for
    // that request cannot clear a freshly typed question with the same words.
    auto* fresh = panel.currentView();
    loadMock(*fresh, chatMock());
    runJsSync(*fresh, "window.__enableDelay = -1");
    question->setText("Explain this passage");
    panel.refreshContext();
    CHECK(question->text() == "Explain this passage");
    pressEnter(*question);
    CHECK(waitFor(qt, [&] { return runJsSync(*fresh, "window.__fills").toInt() == 1; }));
    panel.clearQuestionDrafts();
    CHECK(question->text().isEmpty());
    question->setText("Explain this passage");
    runJsSync(*fresh, "button.disabled = false; button.removeAttribute('aria-disabled')");
    CHECK(waitFor(qt, [&] { return ask->isEnabled(); }));
    CHECK(question->text() == "Explain this passage");
    CHECK(runJsSync(*fresh, "window.__sends").toInt() == 1);
}

static void loadClaudeMock(QApplication& qt, QWebEngineView& view, const QString& title) {
    bool done = false;
    auto connection = QObject::connect(&view, &QWebEngineView::loadFinished,
                                       [&](bool) { done = true; });
    view.setHtml("<title>" + title + "</title><p>chat</p>", QUrl("https://claude.ai/new"));
    CHECK(waitFor(qt, [&] { return done; }, 10000));
    QObject::disconnect(connection);
}

// Claude chats are remembered per paper: reopening a paper lists them,
// a pick reopens that conversation, and New starts one that joins the list.
static void testSavedChats(QApplication& qt) {
    CHECK(WebPanel::claudeChatUrl(QUrl("https://claude.ai/chat/abc-123?x=1#m")) ==
          "https://claude.ai/chat/abc-123");
    CHECK(WebPanel::claudeChatUrl(QUrl("https://claude.ai/new")).isEmpty());
    CHECK(WebPanel::claudeChatUrl(QUrl("https://claude.ai/chat/")).isEmpty());
    CHECK(WebPanel::claudeChatUrl(QUrl("https://chatgpt.com/chat/abc")).isEmpty());
    CHECK(WebPanel::chatTitle("Synapse questions - Claude") == "Synapse questions");
    CHECK(WebPanel::chatTitle(" Claude ").isEmpty());

    reader::Application app;
    CHECK(app.webChats);
    if (!app.webChats) return;
    LocalRequestInterceptor interceptor;
    WebPanel panel(&app);
    panel.findChild<QWebEngineProfile*>()->setUrlRequestInterceptor(&interceptor);
    auto* tabs = panel.findChild<QTabWidget*>("browserSessions");
    auto* list = panel.findChild<QListWidget*>("browserChatList");
    auto* listNew = panel.findChild<QPushButton*>("browserChatListNew");
    auto* listBack = panel.findChild<QPushButton*>("browserChatListBack");
    auto* chatsButton = panel.findChild<QPushButton*>("browserChatListButton");
    CHECK(tabs && list && listNew && listBack && chatsButton);
    if (!tabs || !list || !listNew || !listBack || !chatsButton) return;
    auto savedFor = [&](const char* doc) { return app.webChats->webChatsFor(doc, "claude"); };

    // No history yet: the paper opens straight into a chat.
    panel.setDocument("paper-a");
    CHECK(!panel.chatListVisible());
    CHECK(tabs->count() == 1);
    CHECK(!chatsButton->isHidden());
    auto* first = panel.currentView();
    loadClaudeMock(qt, *first, "Synapse questions - Claude");
    CHECK(savedFor("paper-a").empty()); // not saved before the site assigns a chat URL
    runJsSync(*first, "history.pushState({}, '', '/chat/aaa-111')");
    CHECK(waitFor(qt, [&] { return savedFor("paper-a").size() == 1; }));
    if (savedFor("paper-a").size() != 1) return;
    CHECK(savedFor("paper-a")[0].url == "https://claude.ai/chat/aaa-111");
    CHECK(savedFor("paper-a")[0].title == "Synapse questions");

    // Another paper closes paper-a's tab and has no history of its own.
    QPointer<QWebEngineView> closedFirst(first);
    panel.setDocument("paper-b");
    CHECK(!panel.chatListVisible());
    CHECK(tabs->count() == 1 && panel.currentView() != first);
    CHECK(waitFor(qt, [&] { return closedFirst.isNull(); }));
    CHECK(savedFor("paper-b").empty());

    // Reopening paper-a shows its saved chat; clicking reopens it.
    panel.setDocument("paper-a");
    CHECK(panel.chatListVisible());
    CHECK(tabs->count() == 0);
    CHECK(listBack->isHidden());
    CHECK(list->count() == 1 && list->item(0)->text().startsWith("Synapse questions"));
    emit list->itemClicked(list->item(0));
    CHECK(!panel.chatListVisible());
    CHECK(tabs->count() == 1 && tabs->tabText(0) == "Synapse questions");
    panel.ensureLoaded();
    CHECK(waitFor(qt, [&] {
        return panel.currentView()->url().toString().startsWith("https://claude.ai/chat/aaa-111");
    }));

    // Picking an open chat again switches to its tab instead of duplicating it.
    chatsButton->click();
    CHECK(panel.chatListVisible() && !listBack->isHidden());
    emit list->itemClicked(list->item(0));
    CHECK(tabs->count() == 1);

    // New from the list starts a fresh Claude chat that joins the list once used.
    chatsButton->click();
    listNew->click();
    CHECK(!panel.chatListVisible());
    CHECK(tabs->count() == 2);
    auto* second = panel.currentView();
    loadClaudeMock(qt, *second, "Gradient follow-up - Claude");
    runJsSync(*second, "history.pushState({}, '', '/chat/bbb-222')");
    CHECK(waitFor(qt, [&] { return savedFor("paper-a").size() == 2; }));
    CHECK(savedFor("paper-b").empty());

    // Closing every tab brings the list back with both chats.
    panel.closeSession(0);
    panel.closeSession(0);
    CHECK(panel.chatListVisible() && tabs->count() == 0);
    CHECK(list->count() == 2);

    // Removing forgets the chat for this paper only.
    app.webChats->deleteWebChat("paper-a", "https://claude.ai/chat/bbb-222");
    panel.showChatList();
    CHECK(list->count() == 1);

    // Asking straight from the list starts a new chat instead of dropping the question.
    panel.ask();
    CHECK(!panel.chatListVisible() && tabs->count() == 1);
    panel.cancelPending();
}

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QWebEngineView view;
    auto* interceptor = new LocalRequestInterceptor;
    view.page()->profile()->setUrlRequestInterceptor(interceptor);
    QEventLoop loaded;
    QTimer loadTimer;
    loadTimer.setSingleShot(true);
    QObject::connect(&loadTimer, &QTimer::timeout, &loaded, &QEventLoop::quit);
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    view.setHtml(R"HTML(
        <fieldset><div class="ProseMirror" contenteditable="true"></div>
        <button aria-label="Send message" onclick="window.__sent=(window.__sent || 0)+1">send</button></fieldset>
    )HTML",
                 QUrl("http://reader.test/"));
    loadTimer.start(20000);
    loaded.exec();

    QString prompt = "Explain \"amortization\" [12] & why p.6 matters.\nSecond line\ttab.";
    QString status = runJsSync(view, WebPanel::fillScript(prompt)).toString();
    CHECK(status == "filled");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "clicked");
    // innerText needs layout (absent headless); walk text nodes explicitly.
    QString editorText = runJsSync(view,
                                   "(function(){"
                                   " var pm=document.querySelector('fieldset .ProseMirror');"
                                   " var out=[];"
                                   " function walk(n){"
                                   "  if(n.nodeType===3) out.push(n.nodeValue);"
                                   "  else if(n.nodeType===1){"
                                   "   var b=/^(DIV|P|LI)$/.test(n.tagName);"
                                   "   if(b&&out.length) out.push('\\n');"
                                   "   Array.prototype.forEach.call(n.childNodes,walk);"
                                   "  }}"
                                   " walk(pm);"
                                   " return out.join('');"
                                   "})()")
                             .toString();
    CHECK(editorText == prompt);
    if (editorText != prompt) {
        auto dump = [](const QString& s) {
            std::string o;
            for (QChar c : s) {
                if (c == '\n') o += "\\n";
                else if (c == '\t') o += "\\t";
                else o += c.toLatin1();
            }
            return o;
        };
        std::cout << "WANT=[" << dump(prompt) << "]\nGOT=[" << dump(editorText) << "]\n";
    }
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "waiting-confirmation");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "waiting-confirmation");
    CHECK(runJsSync(view, "window.__sent").toInt() == 1);
    runJsSync(view, "document.body.insertAdjacentHTML('beforeend', '<div data-testid=\"user-message\">sent</div>')");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "sent");

    // Missing editor degrades to a diagnosable status, never silent.
    view.setHtml("<p>login wall</p>", QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    CHECK(runJsSync(view, WebPanel::fillScript("hi")).toString() == "no-editor");

    // Fallback contract: any contenteditable editor and its form's submit button.
    view.setHtml(R"HTML(
        <form><div data-testid="composer-text-input" contenteditable="true"></div>
        <button type="submit" onclick="event.preventDefault(); window.__sent2=true">send</button></form>
    )HTML",
                 QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    CHECK(runJsSync(view, WebPanel::fillScript("fallback check")).toString() == "filled");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "clicked");
    CHECK(runJsSync(view, "document.querySelector('[data-testid=\"composer-text-input\"]')"
                          ".textContent")
              .toString() == "fallback check");
    CHECK(runJsSync(view, "window.__sent2 === true").toBool());

    // Textarea composers need their native value setter and an input event.
    loadMock(view, R"HTML(
        <fieldset><textarea></textarea>
        <button aria-label="Send message" disabled>Send</button></fieldset>
        <button aria-label="Stop response">Stop</button>
        <script>
          document.querySelector('textarea').addEventListener('input', function() {
            document.querySelector('fieldset button').disabled = false;
          });
          document.querySelector('fieldset button').addEventListener('click', function() {
            document.body.insertAdjacentHTML('beforeend', '<div data-testid="user-message">sent</div>');
          });
        </script>
    )HTML");
    CHECK(runJsSync(view, WebPanel::fillScript(prompt)).toString() == "busy");
    CHECK(runJsSync(view, "document.querySelector('textarea').value").toString().isEmpty());
    runJsSync(view, "document.querySelector('[aria-label=\"Stop response\"]').remove()");
    CHECK(runJsSync(view, WebPanel::fillScript(prompt)).toString() == "filled");
    CHECK(runJsSync(view, "document.querySelector('textarea').value").toString() == prompt);
    CHECK(runJsSync(view, WebPanel::sendScript("older-request")).toString() == "cancelled");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "clicked");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "sent");

    // claude.ai composer: ProseMirror inside a fieldset, a "Send message"
    // button that enables on input, "Stop response" while streaming, and
    // user turns marked data-testid="user-message".
    {
        loadMock(view, R"HTML(
            <fieldset>
              <div contenteditable="true" class="ProseMirror" aria-label="Write your prompt to Claude"></div>
              <button aria-label="Send message" disabled>send</button>
            </fieldset>
            <button aria-label="Stop response">stop</button>
            <button aria-label="Send feedback">unrelated</button>
            <script>
              var pm = document.querySelector('.ProseMirror');
              var send = document.querySelector('fieldset button');
              pm.addEventListener('input', function() { send.disabled = !pm.textContent.trim(); });
              send.addEventListener('click', function() {
                window.__claudeSends = (window.__claudeSends || 0) + 1;
                setTimeout(function() {
                  document.body.insertAdjacentHTML('beforeend', '<div data-testid="user-message">sent</div>');
                  pm.textContent = '';
                }, 50);
              });
            </script>
        )HTML");
        CHECK(runJsSync(view, WebPanel::idleScript()).toString() == "busy");
        CHECK(runJsSync(view, WebPanel::fillScript(prompt, "c1")).toString() == "busy");
        runJsSync(view, "document.querySelector('[aria-label=\"Stop response\"]').remove()");
        CHECK(runJsSync(view, WebPanel::idleScript()).toString() == "idle");
        CHECK(runJsSync(view, WebPanel::fillScript(prompt, "c1")).toString() == "filled");
        CHECK(runJsSync(view, WebPanel::idleScript()).toString() == "draft");
        CHECK(runJsSync(view, "document.querySelector('.ProseMirror').textContent").toString()
                  .contains("amortization"));
        CHECK(runJsSync(view, WebPanel::sendScript("c1")).toString() == "clicked");
        CHECK(runJsSync(view, "window.__claudeSends").toInt() == 1);
        CHECK(waitFor(app, [&] {
            return runJsSync(view, WebPanel::sendScript("c1")).toString() == "sent";
        }));
        CHECK(runJsSync(view, "window.__claudeSends").toInt() == 1);

        // Enter-only composer: no send button at all, Enter submits.
        loadMock(view, R"HTML(
            <fieldset><div contenteditable="true" class="ProseMirror"></div></fieldset>
            <script>
              var pm = document.querySelector('.ProseMirror');
              pm.addEventListener('keydown', function(event) {
                if (event.key !== 'Enter') return;
                event.preventDefault();
                window.__enterSends = (window.__enterSends || 0) + 1;
                history.pushState({}, '', '/chat/abc');
                pm.textContent = '';
              });
            </script>
        )HTML");
        CHECK(runJsSync(view, WebPanel::fillScript("enter only", "c2")).toString() == "filled");
        CHECK(runJsSync(view, WebPanel::sendScript("c2")).toString() == "clicked");
        CHECK(runJsSync(view, "window.__enterSends").toInt() == 1);
        CHECK(runJsSync(view, WebPanel::sendScript("c2")).toString() == "sent");

        CHECK(WebPanel::isClaudeUrl(QUrl("https://claude.ai/chat/abc")));
        CHECK(!WebPanel::isClaudeUrl(QUrl("https://chatgpt.com/c/1")));
        CHECK(!WebPanel::isClaudeUrl(QUrl("http://reader.test/chat")));
        CHECK(!WebPanel::isClaudeUrl(QUrl("https://notclaude.ai/")));
        CHECK(WebPanel::homeUrl().host() == "claude.ai");
    }

    // Login persistence regression: a cookie written through a
    // preconfigured named profile must reach disk and be readable again
    // through a fresh instance of the same named profile (the relogin-every-time
    // bug was a profile that never persisted anything).
    {
        const QString cookieRoot = QDir::homePath() + "/cookietest";
        QDir().mkpath(cookieRoot);
        auto clearStore = [&] {
            QDir d(cookieRoot);
            d.removeRecursively();
            QDir().mkpath(cookieRoot);
        };
        clearStore();
        auto cookieFile = [&] {
            QDirIterator it(cookieRoot, {"Cookies"},
                            QDir::Files, QDirIterator::Subdirectories);
            return it.hasNext() ? it.next() : QString();
        };
        {
            auto* pa = new QWebEngineProfile("persist-a", &app);
            pa->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);
            pa->setCachePath(cookieRoot + "/cache");
            pa->setPersistentStoragePath(cookieRoot + "/storage");
            {
                QWebEngineView va;
                va.setPage(new QWebEnginePage(pa, &va));
                va.setHtml("<p>cookie probe</p>", QUrl("http://reader.test/"));
                QObject::connect(va.page(), &QWebEnginePage::loadFinished, &loaded,
                                 &QEventLoop::quit);
                loadTimer.start(20000);
                loaded.exec();
                runJsSync(va, "document.cookie='persist-test=abc123;max-age=3600;path=/'");
                CHECK(runJsSync(va, "document.cookie").toString().contains("persist-test=abc123"));
                QEventLoop settle;
                QTimer::singleShot(3000, &settle, &QEventLoop::quit);
                settle.exec();
            }
            delete pa; // views/pages are gone before the profile is destroyed
        }
        // Deleting the profile flushes its cookie store asynchronously. On a
        // real disk (fsync) the flush can land after a same-process
        // successor has already read the store, so wait for the cookie to
        // reach the file — the property this test is about — before
        // reopening. (Across an app restart the flush completes at exit.)
        auto cookieOnDisk = [&] {
            QDirIterator it(cookieRoot, {"Cookies", "Cookies-journal"}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QFile file(it.next());
                if (file.open(QIODevice::ReadOnly) && file.readAll().contains("persist-test"))
                    return true;
            }
            return false;
        };
        CHECK(waitFor(app, cookieOnDisk, 15000));
        CHECK(!cookieFile().isEmpty());
        {
            auto* pb = new QWebEngineProfile("persist-a", &app);
            pb->setPersistentCookiesPolicy(QWebEngineProfile::AllowPersistentCookies);
            pb->setCachePath(cookieRoot + "/cache");
            pb->setPersistentStoragePath(cookieRoot + "/storage");
            {
                QWebEngineView vb;
                vb.setPage(new QWebEnginePage(pb, &vb));
                vb.setHtml("<p>cookie probe</p>", QUrl("http://reader.test/"));
                QObject::connect(vb.page(), &QWebEnginePage::loadFinished, &loaded,
                                 &QEventLoop::quit);
                loadTimer.start(20000);
                loaded.exec();
                QString jar;
                for (int i = 0; i < 10; ++i) {
                    jar = runJsSync(vb, "document.cookie").toString();
                    if (jar.contains("persist-test=abc123")) break;
                    QEventLoop pause;
                    QTimer::singleShot(1000, &pause, &QEventLoop::quit);
                    pause.exec();
                }
                CHECK(jar.contains("persist-test=abc123"));
            }
            delete pb;
        }
        clearStore();
    }

    testFilePicker(app);
    testSessions(app);
    testSavedChats(app);
    if (failures == 0) std::cout << "ALL WEBENGINE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
