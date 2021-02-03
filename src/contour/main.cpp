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
#include <crispy/logger.h>
#include <crispy/indexed.h>
#include <crispy/utils.h>

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
            addOption(liveConfigOption);
            addOption(parserTable);
            addOption(enableDebugLogging);
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

        QCommandLineOption const enableDebugLogging{
            QStringList() << "d" << "enable-debug-logging",
            QCoreApplication::translate("main", "Enables debug logging.")
        };

        QCommandLineOption const liveConfigOption{
            QStringList() << "live-config",
            QCoreApplication::translate("main", "Enables live config reloading.")
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
    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    try
    {
        QCoreApplication::setApplicationName("contour");
        QCoreApplication::setOrganizationName("contour");
        QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

        QApplication app(argc, argv);

        QSurfaceFormat::setDefaultFormat(contour::TerminalWidget::surfaceFormat());

        auto cli = contour::CLI{};
        cli.process(app);

        if (cli.isSet(cli.parserTable))
        {
            terminal::parser::dot(std::cout, terminal::parser::ParserTable::get());
            return EXIT_SUCCESS;
        }

        // customize debuglog transform to shorten the file_name output a bit
        crispy::logging_sink::for_debug().set_transform([](crispy::log_message const& _msg) -> std::string
        {
            auto const srcIndex = string_view(_msg.location().file_name()).find("src");
            auto const fileName = string(srcIndex != string_view::npos
                ? string_view(_msg.location().file_name()).substr(srcIndex + 4)
                : string(_msg.location().file_name()));

            auto result = string{};

            for (auto const [i, line] : crispy::indexed(crispy::split(_msg.text(), '\n')))
            {
                if (i != 0)
                    result += "        ";
                else
                    result += fmt::format("[{}:{}:{}] ",
                                          fileName,
                                          _msg.location().line(),
                                          _msg.location().function_name());

                result += line;
                result += '\n';
            }

            return result;
        });

        if (cli.isSet(cli.enableDebugLogging))
            crispy::logging_sink::for_debug().enable(true);

        QString const configPath = cli.value(cli.configOption);

        auto config =
            configPath.isEmpty() ? contour::config::loadConfig()
                                 : contour::config::loadConfigFromFile(configPath.toStdString());

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

        bool const liveConfig = cli.isSet(cli.liveConfigOption);

        // Possibly override shell to be executed
        if (auto const positionalArgs = cli.positionalArguments(); !positionalArgs.empty())
        {
            auto& shell = config.profile(profileName)->shell;
            shell.program = positionalArgs.at(0).toStdString();
            auto args = vector<string>{};
            for (int i = 1; i < positionalArgs.size(); ++i)
                shell.arguments.push_back(positionalArgs.at(i).toStdString());
        }

        contour::Controller controller(argv[0], config, liveConfig, profileName);
        controller.start();

        auto const rv = app.exec();

        controller.exit();
        controller.wait();

        return rv;
    }
    catch (exception const& e)
    {
        configLogger(fmt::format("Unhandled error caught. {}", e.what()));
        return EXIT_FAILURE;
    }
}
