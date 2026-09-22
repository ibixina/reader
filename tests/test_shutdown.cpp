// Shutdown regression test: closing the window while background document
// work is in flight must quit the event loop promptly with code 0 — no
// hangs in pool joins, no use-after-free from queued document callbacks.
#include "app/Application.h"
#include "academic_fixture.h"
#include "ui/MainWindow.h"
#include "ui/PdfView.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {

int failures = 0;
#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::cerr << "FAIL line " << __LINE__ << ": " #condition << '\n';           \
            ++failures;                                                                  \
        }                                                                                 \
    } while (false)

} // namespace

int main(int argc, char** argv) {
    char homeTemplate[] = "/tmp/reader-shutdown-XXXXXX";
    char* testHome = mkdtemp(homeTemplate);
    if (!testHome) return 2;
    setenv("HOME", testHome, 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    unsetenv("OPENAI_API_KEY");
    unsetenv("PAPER_READER_MODEL");
    QApplication qt(argc, argv);
    QCoreApplication::setOrganizationName("ReaderShutdown");
    QCoreApplication::setApplicationName("ReaderShutdown");

    const std::string pdfPath = std::string(testHome) + "/academic-fixture.pdf";
    reader_test::writeAcademicPdf(pdfPath);
    reader::Application app;
    MainWindow window(&app);
    window.resize(1100, 760);
    window.show();
    window.activateWindow();
    qt.processEvents(QEventLoop::AllEvents, 50);
    window.openFile(QString::fromStdString(pdfPath));
    auto* pdf = window.findChild<PdfView*>();
    CHECK(pdf != nullptr);

    // Close from INSIDE the running loop (the real user flow): after
    // extraction publishes, arm prefetch work on another page, then close
    // on the next tick. quitOnLastWindowClosed must end exec() promptly.
    QTimer poller;
    poller.setInterval(50);
    int ticks = 0;
    bool armed = false;
    QObject::connect(&poller, &QTimer::timeout, [&] {
        if (++ticks > 600) { // ~30 s without extraction: fail loudly
            poller.stop();
            std::cerr << "FAIL: extraction never published\n";
            ++failures;
            qt.exit(2);
            return;
        }
        if (app.model.blocks.size() <= 8) return;
        if (!armed) {
            armed = true;
            if (pdf) pdf->goToPage(2); // arm prefetch work, then close into it
        } else {
            poller.stop();
            window.close();
        }
    });
    poller.start();
    QElapsedTimer closeTimer;
    closeTimer.start();
    const int code = qt.exec();
    const qint64 closeMs = closeTimer.elapsed();
    std::cout << "shutdown ms: open_to_quit=" << closeMs << " exit=" << code << '\n';
    CHECK(code == 0);
    CHECK(app.model.blocks.size() > 8);
    CHECK(closeMs < 60000);
    app.shutdown();
    std::filesystem::remove_all(testHome);
    if (failures == 0) std::cout << "ALL SHUTDOWN TESTS PASSED\n";
    return failures == 0 ? 0 : 1;
}
