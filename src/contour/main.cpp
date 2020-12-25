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
#include <contour/Controller.h>
#include <contour/TerminalWidget.h>

#include <terminal/Parser.h>

#include <QtCore/QCommandLineParser>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QSurfaceFormat>

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
            addOption(workingDirectoryOption);
            addOption(parserTable);
            addPositionalArgument("executable", "path to executable to execute.");
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

        QCommandLineOption const parserTable{
            QStringList() << "t" << "parser-table",
            QCoreApplication::translate("main", "Dumps parser table")
        };

        QString profileName() const { return value(profileOption); }

        QCommandLineOption const workingDirectoryOption{
            QStringList() << "w" << "working-directory",
            QCoreApplication::translate("main", "Sets initial working directory."),
            QCoreApplication::translate("main", "NAME")
        };

        QString workingDirectory() const { return value(workingDirectoryOption); }
    };
}

int main(int argc, char* argv[])
{
    try
    {
        QCoreApplication::setApplicationName("contour");
        QCoreApplication::setOrganizationName("contour");
        QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

        QSurfaceFormat::setDefaultFormat(contour::TerminalWidget::surfaceFormat());

        QApplication app(argc, argv);

        auto cli = contour::CLI{};
        cli.process(app);

        if (cli.isSet(cli.parserTable))
        {
            terminal::parser::dot(std::cout, terminal::parser::ParserTable::get());
            return EXIT_SUCCESS;
        }

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

        if (!cli.workingDirectory().isEmpty())
            config.profile(profileName)->shell.workingDirectory =
                FileSystem::path(cli.workingDirectory().toUtf8().toStdString());

        if (configFailures)
            return EXIT_FAILURE;

        // Possibly override shell to be executed
        if (auto const positionalArgs = cli.positionalArguments(); !positionalArgs.empty())
        {
            auto& shell = config.profile(profileName)->shell;
            shell.program = positionalArgs.at(0).toStdString();
            auto args = vector<string>{};
            for (int i = 1; i < positionalArgs.size(); ++i)
                shell.arguments.push_back(positionalArgs.at(i).toStdString());
        }

        contour::Controller controller(argv[0], config, profileName);
        controller.start();

        auto const rv = app.exec();

        controller.exit();
        controller.wait();

        return rv;
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
