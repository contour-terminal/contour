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
#include <contour/ContourGuiApp.h>

#if defined(CONTOUR_FRONTEND_GUI)
#include <contour/Config.h>
#include <contour/Controller.h>
#include <contour/opengl/TerminalWidget.h>
#endif

#if defined(CONTOUR_FRONTEND_GUI)
#include <QtWidgets/QApplication>
#include <QSurfaceFormat>
#endif

#include <iostream>

using std::bind;
using std::cerr;
using std::prev;
using std::string;
using std::string_view;

using namespace std::string_literals;

namespace CLI = crispy::cli;

namespace contour {

ContourGuiApp::ContourGuiApp() :
    ContourApp()
{
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
}

int ContourGuiApp::run(int argc, char const* argv[])
{
    argc_ = argc;
    argv_ = argv;

    return ContourApp::run(argc, argv);
}

crispy::cli::Command ContourGuiApp::parameterDefinition() const
{
    auto command = ContourApp::parameterDefinition();

    command.children.insert(
        command.children.begin(),
        CLI::Command{
            "terminal",
            "Spawns a new terminal application.",
            CLI::OptionList{
                CLI::Option{"config", CLI::Value{contour::config::defaultConfigFilePath()}, "Path to configuration file to load at startup.", "FILE"},
                CLI::Option{"profile", CLI::Value{""s}, "Terminal Profile to load (overriding config).", "NAME"},
                CLI::Option{"debug", CLI::Value{""s}, "Enables debug logging, using a comma (,) seperated list of tags.", "TAGS"},
                CLI::Option{"live-config", CLI::Value{false}, "Enables live config reloading."},
                CLI::Option{"working-directory", CLI::Value{""s}, "Sets initial working directory (overriding config).", "DIRECTORY"},
            },
            CLI::CommandList{},
            CLI::CommandSelect::Implicit,
            CLI::Verbatim{"PROGRAM ARGS...", "Executes given program instead of the configuration profided one."}
        }
    );

    return command;
}

int terminalGUI(int argc, char const* argv[], CLI::FlagStore const& _flags)
{
    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    if (auto const filterString = _flags.get<string>("contour.terminal.debug"); !filterString.empty())
    {
        if (filterString == "all")
        {
            for (auto& tag: crispy::debugtag::store())
                tag.enabled = true;
        }
        else
        {
            auto const filters = crispy::split(filterString, ',');
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

    // NB: High DPI scaling should be enabled, but that sadly also applies to QOpenGLWidget
    // which makes the text look pixelated on HighDPI screens. We want to apply HighDPI
    // manually in QOpenGLWidget.
    //QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
    #endif

    QApplication app(argc, (char**) argv);

    QSurfaceFormat::setDefaultFormat(contour::opengl::TerminalWidget::surfaceFormat());

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

int ContourGuiApp::terminalGuiAction()
{
    return terminalGUI(argc_, argv_, parameters());
}

}
