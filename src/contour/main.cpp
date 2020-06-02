/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
            addOption(profileOption);
        }

        QCommandLineOption const configOption{
            QStringList() << "c" << "config",
            QCoreApplication::translate("main", "Path to configuration file to load at startup."),
            QCoreApplication::translate("main", "PATH")
        };

        QString configPath() const { return value(configOption); }

        QCommandLineOption const profileOption{
            QStringList() << "p" << "profile",
            QCoreApplication::translate("main", "Terminal Profile to load."),
            QCoreApplication::translate("main", "NAME")
        };

        QString profileName() const { return value(profileOption); }
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

        auto configFailures = int{0};
        auto const configLogger = [&](string const& _msg)
        {
            cerr << "Configuration failure. " << _msg << '\n';
            ++configFailures;
        };

        QString const configPath = cli.value(cli.configOption);

        auto config =
            configPath.isEmpty() ? contour::config::loadConfig(configLogger)
                                 : contour::config::loadConfigFromFile(configPath.toStdString(), configLogger);

        string const profileName = [&]() {
            if (!cli.value(cli.profileOption).isEmpty())
                return cli.value(cli.profileOption).toStdString();

            if (!config.defaultProfileName.empty())
                return config.defaultProfileName;

            if (config.profiles.size() == 1)
                return config.profiles.begin()->first;

            return ""s;
        }();

        if (!config.profile(profileName))
        {
            auto const s = accumulate(
                begin(config.profiles),
                end(config.profiles),
                ""s,
                [](string const& acc, auto const& profile) -> string {
                    return acc.empty() ? profile.first
                                       : fmt::format("{}, {}", acc, profile.first);
                }
            );
            configLogger(fmt::format("No profile with name '{}' found. Available profiles: {}", profileName, s));
        }

        if (configFailures)
            return EXIT_FAILURE;

        //QSurfaceFormat::setDefaultFormat(contour::TerminalWindow::surfaceFormat());

        auto mainWindow = contour::TerminalWindow{
            config,
            profileName,
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
