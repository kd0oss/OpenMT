#include <QApplication>

#include "mainwindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName("OpenMT");
    QCoreApplication::setApplicationName("OpenMT Radio");

    MainWindow window;
    window.show();

    return app.exec();
}
