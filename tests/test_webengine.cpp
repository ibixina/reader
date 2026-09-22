// Verifies the seamless-handoff mechanism: WebPanel::fillScript fills an
// editor exposing ChatGPT's contract (#prompt-textarea + ProseMirror +
// [data-testid="send-button"]) and presses send. Headless/offscreen.
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
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

int main(int argc, char** argv) {
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
        <button data-testid="send-button" onclick="window.__sent=true">send</button>
    )HTML",
                 QUrl("http://reader.test/"));
    loadTimer.start(20000);
    loaded.exec();

    QString prompt = "Explain \"amortization\" [12] & why p.6 matters.\nSecond line\ttab.";
    QString status = runJsSync(view, WebPanel::fillScript(prompt)).toString();
    CHECK(status == "sent");
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
    bool sent = runJsSync(view, "window.__sent === true").toBool();
    CHECK(sent);

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
    CHECK(runJsSync(view, WebPanel::fillScript("fallback check")).toString() == "sent");
    CHECK(runJsSync(view, "document.querySelector('[data-testid=\"composer-text-input\"]')"
                          ".textContent")
              .toString() == "fallback check");
    CHECK(runJsSync(view, "window.__sent2 === true").toBool());

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
        QString poll = runJsSync(view, WebPanel::pollScript()).toString();
        CHECK(poll.contains("\"generating\":true"));
    }
    runJsSync(view, "document.querySelector('[data-testid=\"stop-button\"]').remove()");
    {
        QString poll = runJsSync(view, WebPanel::pollScript()).toString();
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
    CHECK(WebPanel::sendConfirmed("{\"users\":0,\"composer\":0,\"generating\":false,"
                                  "\"url\":\"https://chatgpt.com/\"}",
                                  "{\"users\":1,\"composer\":0,\"generating\":true,"
                                  "\"url\":\"https://chatgpt.com/\"}"));
    CHECK(!WebPanel::sendConfirmed("{\"users\":0,\"composer\":0,\"generating\":false,"
                                   "\"url\":\"https://chatgpt.com/\"}",
                                   "{\"users\":0,\"composer\":140,\"generating\":false,"
                                   "\"url\":\"https://chatgpt.com/\"}"));
    // The screenshot bug: composer still full, nothing generating, no new
    // user message -> unconfirmed, never reported as sent.
    CHECK(!WebPanel::sendConfirmed("{\"users\":0,\"composer\":0,\"generating\":false,"
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
        QString snap = runJsSync(view, WebPanel::confirmScript()).toString();
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
    CHECK(runJsSync(view, WebPanel::attachScript("SGVsbG8=", "paper.pdf")).toString() ==
          "started");
    {
        QString state;
        for (int i = 0; i < 15; ++i) {
            state = runJsSync(view, WebPanel::attachPollScript()).toString();
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
    CHECK(runJsSync(view, WebPanel::attachScript("SGVsbG8=", "paper.pdf")).toString() ==
          "no-file-input");

    if (failures == 0) std::cout << "ALL WEBENGINE TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
