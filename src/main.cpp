#include "app/application_controller.h"
#include "ui/styles/Theme.hpp"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("PhosChat"));
    QCoreApplication::setApplicationName(QStringLiteral("PhosChat"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PHOSCHAT_VERSION));
    QApplication::setApplicationDisplayName(QStringLiteral("PhosChat"));
    QApplication::setWindowIcon(phoschat::ui::brandIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("All your conversations. One place."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    phoschat::ui::applyTheme(app, phoschat::ui::preferredTheme());
    phoschat::ApplicationController controller(app);
    if (!controller.start())
        return 1;
    return app.exec();
}
