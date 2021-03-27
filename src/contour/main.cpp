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
#include <contour/CaptureScreen.h>

#if defined(CONTOUR_FRONTEND_GUI)
#include <contour/Config.h>
#include <contour/Controller.h>
#include <contour/TerminalWidget.h>
#endif

#include <terminal/Capabilities.h>
#include <terminal/Parser.h>

#include <crispy/CLI.h>
#include <crispy/debuglog.h>
#include <crispy/indexed.h>
#include <crispy/utils.h>

#if defined(CONTOUR_FRONTEND_GUI)
#include <QtWidgets/QApplication>
#include <QSurfaceFormat>
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iomanip>
#include <iostream>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/ioctl.h>
#endif

using namespace std;

namespace CLI = crispy::cli;

CLI::HelpStyle helpStyle()
{
    auto style = CLI::HelpStyle{};

    style.optionStyle = CLI::OptionStyle::Natural;

#if !defined(_WIN32)
    if (!isatty(STDOUT_FILENO))
    {
        style.colors.reset();
        style.hyperlink = false;
    }
#endif

     return style;
}

int screenWidth()
{
    constexpr auto DefaultWidth = 80;

#if !defined(_WIN32)
    auto ws = winsize{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1)
        return ws.ws_col;
#endif

    return DefaultWidth;
}

#if defined(CONTOUR_FRONTEND_GUI) // {{{
int terminalGUI(int argc, char* argv[], CLI::FlagStore const& _flags)
{
    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << endl;
        ++configFailures;
    };

    if (auto const filterString = _flags.get<string>("contour.terminal.debug"); !filterString.empty())
    {
        if (filterString == "all")
        {
            crispy::logging_sink::for_debug().enable(true);
            for (auto& tag: crispy::debugtag::store())
                tag.enabled = true;
        }
        else
        {
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
    }

    auto const configPath = QString::fromStdString(_flags.get<string>("contour.terminal.config"));

    auto config =
        configPath.isEmpty() ? contour::config::loadConfig()
                             : contour::config::loadConfigFromFile(configPath.toStdString());

    string const profileName = [&]() {
        if (auto profile = _flags.get<string>("contour.terminal.profile"); !profile.empty())
            return profile;

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

    if (auto const wd = _flags.get<string>("contour.terminal.working-directory"); !wd.empty())
        config.profile(profileName)->shell.workingDirectory = FileSystem::path(wd);

    if (configFailures)
        return EXIT_FAILURE;

    bool const liveConfig = _flags.get<bool>("contour.terminal.live-config");

    // Possibly override shell to be executed
    if (!_flags.verbatim.empty())
    {
        auto& shell = config.profile(profileName)->shell;
        shell.program = _flags.verbatim.front();
        shell.arguments.clear();
        for (size_t i = 1; i < _flags.verbatim.size(); ++i)
             shell.arguments.push_back(string(_flags.verbatim.at(i)));
    }

    QCoreApplication::setApplicationName("contour");
    QCoreApplication::setOrganizationName("contour");
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    QApplication app(argc, argv);

    QSurfaceFormat::setDefaultFormat(contour::TerminalWidget::surfaceFormat());

    contour::Controller controller(argv[0], config, liveConfig, profileName);
    controller.start();

    // auto const HTS = "\033H";
    // auto const TBC = "\033[g";
    // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

    auto const rv = app.exec();

    controller.exit();
    controller.wait();

    // printf("\r%s", TBC);
    return rv;
}
#endif // }}}

// customize debuglog transform to shorten the file_name output a bit
void customizeDebugLog()
{
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
}

void listDebugTags()
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
}

int main(int argc, char* argv[])
{
    try
    {
        customizeDebugLog();

        auto const cliDef = CLI::Command{
            "contour",
            "Contour Terminal Emulator " CONTOUR_VERSION_STRING " - https://github.com/christianparpart/contour/ ;-)",
            CLI::OptionList{},
            CLI::CommandList{
#if defined(CONTOUR_FRONTEND_GUI)
                CLI::Command{
                    "terminal",
                    "Spawns a new terminal application.",
                    CLI::OptionList{
                        CLI::Option{"config", CLI::Value{contour::config::defaultConfigFilePath()}, "Path to configuration file to load at startup."},
                        CLI::Option{"profile", CLI::Value{""s}, "Terminal Profile to load (overriding config)."},
                        CLI::Option{"debug", CLI::Value{""s}, "Enables debug logging, using a comma seperated list of tags."},
                        CLI::Option{"live-config", CLI::Value{false}, "Enables live config reloading."},
                        CLI::Option{"working-directory", CLI::Value{""s}, "Sets initial working directory (overriding config)."},
                    },
                    CLI::CommandList{},
                    CLI::CommandSelect::Implicit,
                    CLI::Verbatim{"PROGRAM ARGS...", "Executes given program instead of the configuration profided one."}
                },
#endif
                CLI::Command{"help", "Shows this help and exits."},
                CLI::Command{"version", "Shows The version and exits."},
                CLI::Command{"parser-table", "Dumps parser table"},
                CLI::Command{"list-debug-tags", "Lists all available debug tags and exits."},
                CLI::Command{
                    "terminfo",
                    "Generates the terminfo source file that will reflect the features of this version of contour. Using - as value will write to stdout instead.",
                    {
                        CLI::Option{"output", CLI::Value{""s}, "Output file name to store the screen capture to. If - (dash) is given, the capture will be written to standard output.", "FILE", CLI::Presence::Required},
                    }
                },
                CLI::Command{
                    "capture",
                    "Captures the screen buffer of the currently running terminal.",
                    {
                        CLI::Option{"logical", CLI::Value{false}, "Tells the terminal to use logical lines for counting and capturing."},
                        CLI::Option{"timeout", CLI::Value{1.0}, "Sets timeout seconds to wait for terminal to respond."},
                        CLI::Option{"lines", CLI::Value{0u}, "The number of lines to capture"},
                        CLI::Option{"output", CLI::Value{""s}, "Output file name to store the screen capture to. If - (dash) is given, the capture will be written to standard output.", "FILE", CLI::Presence::Required},
                    }
                },
                CLI::Command{
                    "profile",
                    "Profile configuration for the current terminal.",
                    {
                        CLI::Option{"set", CLI::Value{""s}, "Changes the terminal profile of the currently attached terminal to the given value.", "NAME", CLI::Presence::Required}
                    }
                }
            }
        };

        optional<CLI::FlagStore> const flagsOpt = CLI::parse(cliDef, argc, argv);
        if (!flagsOpt.has_value())
        {
            std::cerr << "Failed to parse command line parameters.\n";
            return EXIT_FAILURE;
        }
        CLI::FlagStore const& flags = flagsOpt.value();

        // std::cout << fmt::format("Flags: {}\n", flags.values.size());
        // for (auto const & [k, v] : flags.values)
        //     std::cout << fmt::format(" - {}: {}\n", k, v);

        if (flags.get<bool>("contour.version"))
        {
            std::cout << fmt::format("Contour Terminal Emulator {}\n\n", CONTOUR_VERSION_STRING);
            return EXIT_SUCCESS;
        }

        if (flags.get<bool>("contour.help"))
        {
            std::cout << CLI::helpText(cliDef, helpStyle(), screenWidth());
            return EXIT_SUCCESS;
        }

        if (flags.get<bool>("contour.terminfo"))
        {
            auto const& outputFileName = flags.get<string>("contour.terminfo.output");
            auto ownedOutput = unique_ptr<std::ostream>{};
            std::ostream* out = &std::cout;
            if (outputFileName != "-")
            {
                ownedOutput = make_unique<std::ofstream>(outputFileName);
                out = ownedOutput.get();
            }
            *out << terminal::capabilities::StaticDatabase{}.terminfo();
            return EXIT_SUCCESS;
        }

        if (flags.get<bool>("contour.capture"))
        {
            auto captureSettings = contour::CaptureSettings{};
            captureSettings.logicalLines = flags.get<bool>("contour.capture.logical");
            captureSettings.timeout = flags.get<double>("contour.capture.timeout");
            captureSettings.lineCount = flags.get<unsigned>("contour.capture.lines");
            captureSettings.outputFile = flags.get<string>("contour.capture.output");

            if (contour::captureScreen(captureSettings))
                return EXIT_SUCCESS;
            else
                return EXIT_FAILURE;
        }

        if (flags.get<bool>("contour.profile"))
        {
            auto const profileName = flags.get<string>("contour.profile.set");
            std::cout << fmt::format("\033P$p{}\033\\", profileName);
            std::cout.flush();
            return EXIT_SUCCESS;
        }

        if (flags.get<bool>("contour.parser-table"))
        {
            terminal::parser::dot(std::cout, terminal::parser::ParserTable::get());
            return EXIT_SUCCESS;
        }

        if (flags.get<bool>("contour.list-debug-tags"))
        {
            listDebugTags();
            return EXIT_SUCCESS;
        }

#if defined(CONTOUR_FRONTEND_GUI)
        if (flags.get<bool>("contour.terminal"))
            return terminalGUI(argc, argv, flags);
#endif

        std::cerr << fmt::format("Usage error.\n");
        return EXIT_FAILURE;
    }
    catch (exception const& e)
    {
        std::cerr << fmt::format("Unhandled error caught. {}", e.what());
        return EXIT_FAILURE;
    }
}
