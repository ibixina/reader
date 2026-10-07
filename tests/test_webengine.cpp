// Exercises composer submission and independent sessions against local pages.
#include "app/Application.h"
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
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

namespace {
// Former WebPanel ingest-via-chat helpers, removed from the shipped app
// when the slim-down dropped that flow. They encode the chatgpt.com DOM
// contract the tests pin, so they live here as test fixtures now.

// JS that stuffs a PDF into the composer's file input. Synchronous
// result: 'started' (async attach running, outcome in window.__attach),
// 'no-file-input', or 'error:...'.
QString attachScript(const QString& base64Pdf, const QString& filename) {
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
        .arg(WebPanel::jsonQuoted(base64Pdf), WebPanel::jsonQuoted(filename));
}

// Reads window.__attach ('pending' when unset).
QString attachPollScript() {
    return QStringLiteral("(function(){ return window.__attach || 'pending'; })()");
}

// JS snapshot of send evidence: {"users":int,"composer":int,"generating":bool,"url":"..."}.
QString confirmScript() {
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

// Pure decision rule over two confirmScript snapshots.
bool sendConfirmed(const QString& beforeJson, const QString& afterJson) {
    const QJsonObject before = QJsonDocument::fromJson(beforeJson.toUtf8()).object();
    const QJsonObject after = QJsonDocument::fromJson(afterJson.toUtf8()).object();
    if (after.value("users").toInt(-1) > before.value("users").toInt(-1)) return true;
    if (!after.value("url").toString().isEmpty() &&
        after.value("url").toString() != before.value("url").toString())
        return true;
    return after.value("generating").toBool(false) && after.value("composer").toInt(-1) == 0;
}

// JS polling the latest assistant message: {"generating":bool,"text":"..."}.
QString pollScript() {
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
} // namespace

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
        <form>
          <div id="prompt-textarea" class="ProseMirror" contenteditable="true"></div>
          <button id="composer-submit-button" type="submit" disabled aria-disabled="true">Send</button>
        </form>
        <script>
          window.__fills = 0; window.__sends = 0; window.__enableDelay = 350;
          var editor = document.getElementById('prompt-textarea');
          var button = document.getElementById('composer-submit-button');
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
          document.querySelector('form').addEventListener('submit', function(event) {
            event.preventDefault();
            ++window.__sends;
            window.__prompt = editor.innerText;
            setTimeout(function() {
              var message = document.createElement('div');
              message.setAttribute('data-message-author-role', 'user');
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
                QMetaObject::invokeMethod(dialog, "accept");
            }
        });
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &panel, [] {
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
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
        <div id="prompt-textarea"><div class="ProseMirror" contenteditable="true"></div></div>
        <button data-testid="send-button" onclick="window.__sent=(window.__sent || 0)+1">send</button>
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
                                   " var pm=document.querySelector('#prompt-textarea .ProseMirror');"
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
    runJsSync(view, "document.body.insertAdjacentHTML('beforeend', '<div data-message-author-role=\"user\">sent</div>')");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "sent");

    // Missing editor degrades to a diagnosable status, never silent.
    view.setHtml("<p>login wall</p>", QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    CHECK(runJsSync(view, WebPanel::fillScript("hi")).toString() == "no-editor");

    // Redesigned composer contract: alternate editor + submit selectors.
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
        <form><textarea id="prompt-textarea"></textarea>
        <button data-testid="composer-send-button" disabled>Send</button></form>
        <button data-testid="stop-button">Stop</button>
        <script>
          document.querySelector('textarea').addEventListener('input', function() {
            document.querySelector('form button').disabled = false;
          });
          document.querySelector('form').addEventListener('submit', function(event) {
            event.preventDefault();
            document.body.insertAdjacentHTML('beforeend', '<div data-message-author-role="user">sent</div>');
          });
        </script>
    )HTML");
    CHECK(runJsSync(view, WebPanel::fillScript(prompt)).toString() == "busy");
    CHECK(runJsSync(view, "document.querySelector('textarea').value").toString().isEmpty());
    runJsSync(view, "document.querySelector('[data-testid=\"stop-button\"]').remove()");
    CHECK(runJsSync(view, WebPanel::fillScript(prompt)).toString() == "filled");
    CHECK(runJsSync(view, "document.querySelector('textarea').value").toString() == prompt);
    CHECK(runJsSync(view, WebPanel::sendScript("older-request")).toString() == "cancelled");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "clicked");
    CHECK(runJsSync(view, WebPanel::sendScript()).toString() == "sent");

    // Ingest polling: stop button visible -> generating; gone -> final text.
    view.setHtml(R"HTML(
        <div data-message-author-role="assistant"><div class="markdown">{"overview":1}</div></div>
        <button data-testid="stop-button">stop</button>
    )HTML",
                 QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    {
        QString poll = runJsSync(view, pollScript()).toString();
        CHECK(poll.contains("\"generating\":true"));
    }
    runJsSync(view, "document.querySelector('[data-testid=\"stop-button\"]').remove()");
    {
        QString poll = runJsSync(view, pollScript()).toString();
        CHECK(poll.contains("\"generating\":false"));
        CHECK(poll.contains("overview"));
    }

    // Login persistence regression: a cookie written through a
    // preconfigured named profile must reach disk and be readable again
    // through a fresh profile on the same storage (the relogin-every-time
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
                QEventLoop settle;
                QTimer::singleShot(3000, &settle, &QEventLoop::quit);
                settle.exec();
            }
            delete pa; // views/pages are gone before the profile is destroyed
        }
        CHECK(!cookieFile().isEmpty());
        {
            auto* pb = new QWebEngineProfile("persist-b", &app);
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

    // Send confirmation rule: a mechanical 'sent' means nothing until the
    // conversation shows it. Pure logic, no page needed.
    CHECK(sendConfirmed("{\"users\":0,\"composer\":0,\"generating\":false,"
                                  "\"url\":\"https://chatgpt.com/\"}",
                                  "{\"users\":1,\"composer\":0,\"generating\":true,"
                                  "\"url\":\"https://chatgpt.com/\"}"));
    CHECK(!sendConfirmed("{\"users\":0,\"composer\":0,\"generating\":false,"
                                   "\"url\":\"https://chatgpt.com/\"}",
                                   "{\"users\":0,\"composer\":140,\"generating\":false,"
                                   "\"url\":\"https://chatgpt.com/\"}"));
    // The screenshot bug: composer still full, nothing generating, no new
    // user message -> unconfirmed, never reported as sent.
    CHECK(!sendConfirmed("{\"users\":0,\"composer\":0,\"generating\":false,"
                                   "\"url\":\"https://chatgpt.com/?temporary-chat=true\"}",
                                   "{\"users\":0,\"composer\":0,\"generating\":false,"
                                   "\"url\":\"https://chatgpt.com/?temporary-chat=true\"}"));

    // confirmScript snapshot shape on a mock conversation.
    view.setHtml(R"HTML(
        <div data-message-author-role="user">hello</div>
        <div id="prompt-textarea"><div class="ProseMirror" contenteditable="true">draft</div></div>
    )HTML",
                 QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    {
        QString snap = runJsSync(view, confirmScript()).toString();
        CHECK(snap.contains("\"users\":1"));
        CHECK(snap.contains("\"generating\":false"));
        CHECK(!snap.contains("\"composer\":0"));
    }

    // PDF attach: file input receives the file and its name renders as a
    // chip, exactly the evidence the floating flow waits for.
    view.setHtml(R"HTML(
        <input type="file" id="up">
        <script>
        document.getElementById('up').addEventListener('change', function() {
            var d = document.createElement('div');
            d.id = 'chip';
            d.textContent = this.files[0].name;
            document.body.appendChild(d);
        });
        </script>
    )HTML",
                 QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    CHECK(runJsSync(view, attachScript("SGVsbG8=", "paper.pdf")).toString() ==
          "started");
    {
        QString state;
        for (int i = 0; i < 15; ++i) {
            state = runJsSync(view, attachPollScript()).toString();
            if (state == "attached") break;
            QEventLoop pause;
            QTimer::singleShot(1000, &pause, &QEventLoop::quit);
            pause.exec();
        }
        CHECK(state == "attached");
        CHECK(runJsSync(view, "document.getElementById('up').files.length").toInt() == 1);
    }

    // No file input degrades to a diagnosable status, never silent.
    view.setHtml("<p>login wall</p>", QUrl("http://reader.test/"));
    QObject::connect(view.page(), &QWebEnginePage::loadFinished, &loaded, &QEventLoop::quit);
    loadTimer.start(20000);
    loaded.exec();
    CHECK(runJsSync(view, attachScript("SGVsbG8=", "paper.pdf")).toString() ==
          "no-file-input");

    testFilePicker(app);
    testSessions(app);
    if (failures == 0) std::cout << "ALL WEBENGINE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
