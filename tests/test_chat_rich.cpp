#include "ui/ChatTranscript.h"
#include <QApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstdlib>
#include <iostream>

namespace {
int fail(const char* message) {
    std::cerr << "rich chat transcript check failed: " << message << '\n';
    return 1;
}
}

int main(int argc, char** argv) {
    setenv("QT_QPA_PLATFORM", "offscreen", 0);
    QApplication application(argc, argv);
    ChatTranscript transcript;
    transcript.resize(480, 520);
    transcript.show();
    if (!transcript.richMathEnabled()) return fail("WebEngine/KaTeX renderer is not enabled");

    int activated = -1;
    QObject::connect(&transcript, &ChatTranscript::sourceActivated,
                     [&](int index) { activated = index; });
    transcript.appendRole("AI");
    transcript.appendMarkdown(
        "A **grounded** explanation with code:\n\n```cpp\nint n = 4;\n"
        "const char* raw = \"\\\\alpha $not_math$\";\n```\n\n"
        "Inline $\\frac{a}{b}$ and display math:\n\n"
        "$$\\begin{bmatrix}1 & 2 \\\\ 3 & 4\\end{bmatrix}$$\n\n"
        "Source [eq_4]. Cost \\$5.",
        {{"eq_4", 3}});

    auto* view = transcript.findChild<QWebEngineView*>("richChatThread");
    if (!view) return fail("rich transcript view was not created");
    QEventLoop loaded;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loaded, &QEventLoop::quit);
    QObject::connect(view, &QWebEngineView::loadFinished, &loaded, &QEventLoop::quit);
    timeout.start(8000);
    loaded.exec();

    QVariant result;
    QEventLoop script;
    view->page()->runJavaScript(
        "({math:document.querySelectorAll('.katex').length,"
        "code:document.querySelectorAll('pre').length,"
        "source:document.querySelectorAll('a[href^=\"reader-source:\"]').length,"
        "matrixRows:document.querySelectorAll('mtr').length,"
        "bodyText:document.body.innerText,"
        "mathText:[...document.querySelectorAll('.katex')].map(e=>e.textContent).join('|'),"
        "mathWidth:[...document.querySelectorAll('.katex')].reduce((n,e)=>n+e.getBoundingClientRect().width,0)})",
        [&](const QVariant& value) {
            result = value;
            script.quit();
        });
    QTimer::singleShot(5000, &script, &QEventLoop::quit);
    script.exec();
    const QJsonObject counts = QJsonObject::fromVariantMap(result.toMap());
    if (counts.value("math").toInt() < 2 || counts.value("code").toInt() != 1 ||
        counts.value("source").toInt() != 1 || counts.value("mathText").toString().isEmpty() ||
        counts.value("mathWidth").toDouble() < 20 || counts.value("matrixRows").toInt() < 2 ||
        !counts.value("bodyText").toString().contains("grounded") ||
        !counts.value("bodyText").toString().contains("int n = 4") ||
        !counts.value("bodyText").toString().contains("\\alpha $not_math$") ||
        !counts.value("bodyText").toString().contains("Cost $5")) {
        std::cerr << QJsonDocument(counts).toJson(QJsonDocument::Compact).toStdString() << '\n';
        return fail("KaTeX, code, or source markup did not render from local resources");
    }

    QEventLoop click;
    view->page()->runJavaScript("document.querySelector('a[href^=\"reader-source:\"]').click()",
                                [&](const QVariant&) { click.quit(); });
    QTimer::singleShot(2000, &click, &QEventLoop::quit);
    click.exec();
    application.processEvents();
    if (activated != 3) return fail("local source link did not return to the application");
    if (transcript.width() > 480) return fail("rich transcript exceeds the narrow pane width");
    QEventLoop painted;
    QTimer::singleShot(750, &painted, &QEventLoop::quit);
    painted.exec();
    transcript.grab().save("/tmp/reader-chat-katex.png");
    std::cout << "rich chat transcript checks passed; screenshot /tmp/reader-chat-katex.png\n";
    return 0;
}
