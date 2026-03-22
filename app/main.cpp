#include "mainwindow.h"
#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Graph Draw"));
    app.setOrganizationName(QStringLiteral("GraphDraw"));

    MainWindow window;
    window.show();

    return app.exec();
}
