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
#include <crispy/debuglog.h>
#include <crispy/indexed.h>
#include <crispy/utils.h>

#include <QtCore/QCommandLineParser>
#include <QtCore/QThread>
#include <QtWidgets/QApplication>
#include <QSurfaceFormat>

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>

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
            addOption(listDebugTags);
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
            QStringList() << "d" << "enable-debug",
            QCoreApplication::translate("main", "Enables debug logging, using a comma seperated list of tags."),
            QCoreApplication::translate("main", "TAGS")
        };

        QCommandLineOption const listDebugTags{
            QStringList() << "D" << "list-debug-tags",
            QCoreApplication::translate("main", "Lists all available debug tags and exits.")
        };

        QCommandLineOption const liveConfigOption{
            QStringList() << "live-config",
            QCoreApplication::translate("main", "Enables live config reloading.")
        };

        QString profileName() const { return value(profileOption); }
        std::string debuglogFilter() const { return value(enableDebugLogging).toStdString(); }

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
        // auto const HTS = "\033H";
        // auto const TBC = "\033[g";
        // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

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

        if (cli.isSet(cli.listDebugTags))
        {
            auto tags = crispy::debugtag::store();
            sort(
                begin(tags),
                end(tags),
                [](crispy::debugtag::tag_info const& a, crispy::debugtag::tag_info const& b) {
                   return a.name < b.name;
                }
            );
            auto const maxNameLength = std::accumulate(
                begin(tags),
                end(tags),
                size_t{0},
                [&](auto _acc, auto const& _tag) { return max(_acc, _tag.name.size()); }
            );
            auto const column1Length = maxNameLength + 2u;
            for (auto const& tag: tags)
            {
                std::cout
                    << left << setw(int(column1Length)) << tag.name
                    << "; " << tag.description << '\n';
            }
            return EXIT_SUCCESS;
        }

        if (cli.isSet(cli.enableDebugLogging))
        {
            auto const filterString = cli.debuglogFilter();
            auto const filters = crispy::split(filterString, ',');
            crispy::logging_sink::for_debug().enable(true);
            for (auto& tag: crispy::debugtag::store())
            {
                tag.enabled = crispy::any_of(filters, [&](string_view const& filterPattern) -> bool {
                    if (filterPattern.back() != '*')
                        return tag.name == filterPattern;
                    return std::equal(
                        begin(filterPattern),
                        prev(end(filterPattern)),
                        begin(tag.name)
                    );
                });
            }
        }

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

        // printf("\r%s", TBC);
        return rv;
    }
    catch (exception const& e)
    {
        configLogger(fmt::format("Unhandled error caught. {}", e.what()));
        return EXIT_FAILURE;
    }
}
