/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <contour/Config.h>
#include <contour/TerminalWindow.h>

#include <QCommandLineParser>
#include <QGuiApplication>

#include <iostream>

using namespace std;

namespace contour {
    struct CLI : public QCommandLineParser {
        CLI() {
            setApplicationDescription("Contour Terminal Emulator");
            addHelpOption();
            addVersionOption();
            addOption(configOption);
            // addOption(profileOption); // TODO: profile handling
        }

        QCommandLineOption const configOption{
            QStringList() << "c" << "config",
            QCoreApplication::translate("main", "Path to configuration file to load at startup."),
            QCoreApplication::translate("main", "PATH")
        };

        QString configPath() const { return value(configOption); }

        // QCommandLineOption const profileOption{
        //     QStringList() << "p" << "profile",
        //     QCoreApplication::translate("main", "Terminal Profile to load."),
        //     QCoreApplication::translate("main", "NAME")
        // };

        // QString profileName() const { return value(profileOption); }
    };
}

int main(int argc, char* argv[])
{
    try
    {
        QGuiApplication::setApplicationName("contour");
        QGuiApplication::setOrganizationName("contour");
        QGuiApplication::setApplicationVersion(QString::fromStdString(fmt::format(
            "{}.{}.{}", CONTOUR_VERSION_MAJOR, CONTOUR_VERSION_MINOR, CONTOUR_VERSION_PATCH
        )));
        QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
        QGuiApplication app(argc, argv);

        auto cli = contour::CLI{};
        cli.process(app);

        QString const configPath = cli.value(cli.configOption);
        // QString const profileName = cli.value(cli.profileOption); // TODO: support for profiles

        auto mainWindow = contour::TerminalWindow{
            configPath.isEmpty() ? contour::loadConfig()
                                 : contour::loadConfigFromFile(configPath.toStdString()),
            argv[0]
        };
        mainWindow.show();

        return app.exec();
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
