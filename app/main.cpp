#include "app/Application.h"
#include "ui/MainWindow.h"
#include <QApplication>

int main(int argc, char** argv) {
    QApplication qt(argc, argv);
    reader::Application app;
    MainWindow window(&app);
    window.show();
    if (argc > 1) window.openFile(argv[1]);
    return qt.exec();
}
