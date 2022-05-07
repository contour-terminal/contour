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
#include <contour/ContourGuiApp.h>
#include <contour/TerminalWindow.h>
#include <contour/opengl/TerminalWidget.h>

#include <terminal/Process.h>

#include <text_shaper/font_locator.h>

#include <crispy/logstore.h>

#include <QtCore/QProcess>
#include <QtGui/QSurfaceFormat>
#include <QtWidgets/QApplication>

#include <iostream>
#include <vector>

using std::bind;
using std::cerr;
using std::get;
using std::holds_alternative;
using std::make_unique;
using std::prev;
using std::string;
using std::string_view;
using std::vector;

using terminal::Process;

using namespace std::string_literals;

namespace CLI = crispy::cli;

namespace
{
std::vector<std::string> getSessions()
{
    std::vector<std::string> sessionFiles;
    for (auto dirContents: FileSystem::directory_iterator(crispy::App::instance()->localStateDir()))
    {
        // NB: Cannot use  dirContents.is_regular_file() here,
        // because on OS/X, boost::filesystem is used, and that doesn't have that yet.
        bool const isRegularFile = dirContents.status().type() == FileSystem::file_type::regular;
        if (isRegularFile && dirContents.path().extension() == ".session")
        {
            sessionFiles.emplace_back(dirContents.path().string());
        }
    }
    return sessionFiles;
}
} // namespace

namespace contour
{

ContourGuiApp::ContourGuiApp()
{
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
    link("contour.font-locator", bind(&ContourGuiApp::fontConfigAction, this));
}

ContourGuiApp::~ContourGuiApp()
{
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
        CLI::Command {
            "font-locator",
            "Inspects font locator service.",
            CLI::OptionList {
                CLI::Option { "config",
                              CLI::Value { contour::config::defaultConfigFilePath() },
                              "Path to configuration file to load at startup.",
                              "FILE" },
                CLI::Option {
                    "profile", CLI::Value { ""s }, "Terminal Profile to load (overriding config).", "NAME" },
                CLI::Option { "debug",
                              CLI::Value { ""s },
                              "Enables debug logging, using a comma (,) seperated list of tags.",
                              "TAGS" },
            },
        });

    command.children.insert(
        command.children.begin(),
        CLI::Command {
            "terminal",
            "Spawns a new terminal application.",
            CLI::OptionList {
                CLI::Option { "config",
                              CLI::Value { contour::config::defaultConfigFilePath() },
                              "Path to configuration file to load at startup.",
                              "FILE" },
                CLI::Option {
                    "profile", CLI::Value { ""s }, "Terminal Profile to load (overriding config).", "NAME" },
                CLI::Option { "debug",
                              CLI::Value { ""s },
                              "Enables debug logging, using a comma (,) seperated list of tags.",
                              "TAGS" },
                CLI::Option { "live-config", CLI::Value { false }, "Enables live config reloading." },
                CLI::Option {
                    "dump-state-at-exit",
                    CLI::Value { ""s },
                    "Dumps internal state at exit into the given directory. This is for debugging contour.",
                    "PATH" },
                CLI::Option { "early-exit-threshold",
                              CLI::Value { 6u },
                              "If the spawned process exits earlier than the given threshold seconds, an "
                              "error message will be printed and the window not closed immediately." },
                CLI::Option { "working-directory",
                              CLI::Value { ""s },
                              "Sets initial working directory (overriding config).",
                              "DIRECTORY" },
                CLI::Option {
                    "class",
                    CLI::Value { ""s },
                    "Sets the class part of the WM_CLASS property for the window (overriding config).",
                    "WM_CLASS" },
                CLI::Option {
                    "platform", CLI::Value { ""s }, "Sets the QPA platform.", "PLATFORM[:OPTIONS]" },
                CLI::Option { "session",
                              CLI::Value { ""s },
                              "Sets the sessioni ID used for resuming a prior session.",
                              "SESSION_ID" },
#if defined(__linux__)
                CLI::Option {
                    "display", CLI::Value { ""s }, "Sets the X11 display to connect to.", "DISPLAY_ID" },
#endif
                CLI::Option {
                    CLI::OptionName { 'e', "execute" },
                    CLI::Value { ""s },
                    "DEPRECATED: Program to execute instead of running the shell as configured.",
                    "PROGRAM",
                    CLI::Presence::Optional,
                    CLI::Deprecated { "Only supported for compatibility with very old KDE desktops." } },
            },
            CLI::CommandList {},
            CLI::CommandSelect::Implicit,
            CLI::Verbatim { "PROGRAM ARGS...",
                            "Executes given program instead of the configuration profided one." } });

    return command;
}

std::chrono::seconds ContourGuiApp::earlyExitThreshold() const
{
    return std::chrono::seconds(parameters().get<unsigned>("contour.terminal.early-exit-threshold"));
}

string ContourGuiApp::profileName() const
{
    if (auto profile = parameters().get<string>("contour.terminal.profile"); !profile.empty())
        return profile;

    if (!config_.defaultProfileName.empty())
        return config_.defaultProfileName;

    if (config_.profiles.size() == 1)
        return config_.profiles.begin()->first;

    return ""s;
}

std::optional<FileSystem::path> ContourGuiApp::dumpStateAtExit() const
{
    auto const path = parameters().get<std::string>("contour.terminal.dump-state-at-exit");
    if (path.empty())
        return std::nullopt;
    return FileSystem::path(path);
}

void ContourGuiApp::onExit(TerminalSession& _session)
{
    auto const* localProcess = dynamic_cast<terminal::Process const*>(&_session.terminal().device());
    if (!localProcess)
        return;

    exitStatus_ = localProcess->checkStatus();
}

bool ContourGuiApp::loadConfig(string const& target)
{
    auto const& flags = parameters();
    auto const prefix = "contour." + target + ".";

    auto configFailures = int { 0 };
    auto const configLogger = [&](string const& _msg) {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    if (auto const filterString = flags.get<string>(prefix + "debug"); !filterString.empty())
    {
        logstore::configure(filterString);
    }

    auto const configPath = QString::fromStdString(flags.get<string>(prefix + "config"));

    config_ = configPath.isEmpty() ? contour::config::loadConfig()
                                   : contour::config::loadConfigFromFile(configPath.toStdString());

    if (!config_.profile(profileName()))
    {
        auto const s =
            accumulate(begin(config_.profiles),
                       end(config_.profiles),
                       ""s,
                       [](string const& acc, auto const& profile) -> string {
                           return acc.empty() ? profile.first : fmt::format("{}, {}", acc, profile.first);
                       });
        configLogger(
            fmt::format("No profile with name '{}' found. Available profiles: {}", profileName(), s));
    }

    if (auto const wd = flags.get<string>("contour.terminal.working-directory"); !wd.empty())
        config_.profile(profileName())->shell.workingDirectory = FileSystem::path(wd);

    if (configFailures)
        return EXIT_FAILURE;

    // Possibly override shell to be executed
    auto exe = flags.get<string>("contour.terminal.execute");
    if (!flags.verbatim.empty() || !exe.empty())
    {
        auto& shell = config_.profile(profileName())->shell;
        shell.arguments.clear();
        if (!exe.empty())
        {
            shell.program = move(exe);
            for (auto i: flags.verbatim)
                shell.arguments.emplace_back(string(i));
        }
        else
        {
            shell.program = flags.verbatim.front();
            for (size_t i = 1; i < flags.verbatim.size(); ++i)
                shell.arguments.emplace_back(string(flags.verbatim.at(i)));
        }
    }

    if (auto const wmClass = flags.get<string>("contour.terminal.class"); !wmClass.empty())
        config_.profile(profileName())->wmClass = wmClass;

    return true;
}

int ContourGuiApp::fontConfigAction()
{
    if (!loadConfig("font-locator"))
        return EXIT_FAILURE;

    terminal::renderer::FontDescriptions const& fonts = config_.profile(config_.defaultProfileName)->fonts;
    text::font_description const& fontDescription = fonts.regular;
    std::unique_ptr<text::font_locator> fontLocator = createFontLocator(fonts.fontLocator);
    text::font_source_list fontSources = fontLocator->locate(fontDescription);

    fmt::print("Matching fonts using  : {}\n", fonts.fontLocator);
    fmt::print("Font description      : {}\n", fontDescription);
    fmt::print("Number of fonts found : {}\n", fontSources.size());
    for (text::font_source const& fontSource: fontSources)
        fmt::print("  {}\n", fontSource);

    return EXIT_SUCCESS;
}

int ContourGuiApp::terminalGuiAction()
{
    if (auto givenSessionId = parameters().get<std::string>("contour.terminal.session");
        givenSessionId.empty())
    {
        auto sessions = getSessions();
        for (const auto& session: sessions)
        {
            auto sessionId = session.substr(0, session.find_last_of('.'));
            QString const program = QString::fromUtf8(this->argv_[0]);
            QStringList args;
            args << "session" << QString::fromStdString(sessionId);
            QProcess::startDetached(program, args);
        }
        if (!sessions.empty())
            return 0;
    }

    if (!loadConfig("terminal"))
        return EXIT_FAILURE;

    switch (config_.renderingBackend)
    {
        case config::RenderingBackend::OpenGL:
            QGuiApplication::setAttribute(Qt::AA_UseSoftwareOpenGL, false);
            break;
        case config::RenderingBackend::Software:
            QGuiApplication::setAttribute(Qt::AA_UseSoftwareOpenGL, true);
            break;
        case config::RenderingBackend::Default:
            // Don't do anything.
            break;
    }

    auto appName = QString::fromStdString(config_.profile(profileName())->wmClass);
    QCoreApplication::setApplicationName(appName);
    QCoreApplication::setOrganizationName("contour");
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);

    vector<string> qtArgsStore;
    vector<char const*> qtArgsPtr;
    qtArgsPtr.push_back(argv_[0]);

    auto const addQtArgIfSet = [&](string const& key, char const* arg) -> bool {
        if (string const& s = parameters().get<string>(key); !s.empty())
        {
            qtArgsPtr.push_back(arg);
            qtArgsStore.push_back(s);
            qtArgsPtr.push_back(qtArgsStore.back().c_str());
            return true;
        }
        return false;
    };

    addQtArgIfSet("contour.terminal.session", "-session");
    if (!addQtArgIfSet("contour.terminal.platform", "-platform"))
    {
        if (!config_.platformPlugin.empty())
        {
            static constexpr auto platformArg = string_view("-platform");
            qtArgsPtr.push_back(platformArg.data());
            qtArgsPtr.push_back(config_.platformPlugin.c_str());
        }
    }

#if defined(__linux__)
    addQtArgIfSet("contour.terminal.display", "-display");
#endif

    auto qtArgsCount = static_cast<int>(qtArgsPtr.size());
    QApplication app(qtArgsCount, (char**) qtArgsPtr.data());

    QSurfaceFormat::setDefaultFormat(contour::opengl::TerminalWidget::surfaceFormat());

    // auto const HTS = "\033H";
    // auto const TBC = "\033[g";
    // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

    // Spawn initial window.
    newWindow();

    auto rv = app.exec();

    terminalWindows_.clear();

    if (exitStatus_.has_value())
    {
        if (holds_alternative<Process::NormalExit>(*exitStatus_))
            rv = get<Process::NormalExit>(*exitStatus_).exitCode;
        else if (holds_alternative<Process::SignalExit>(*exitStatus_))
            rv = EXIT_FAILURE;
    }

    // printf("\r%s", TBC);
    return rv;
}

TerminalWindow* ContourGuiApp::newWindow(contour::config::Config const& _config)
{
    auto const liveConfig = parameters().get<bool>("contour.terminal.live-config");
    auto mainWindow = new TerminalWindow(earlyExitThreshold(),
                                         _config,
                                         liveConfig,
                                         profileName(),
                                         config_.profile(profileName())->shell.workingDirectory.string(),
                                         *this);

    mainWindow->show();

    terminalWindows_.emplace_back(mainWindow);
    // TODO: Remove window from list when destroyed.

    // QObject::connect(mainWindow, &TerminalWindow::showNotification,
    //                  this, &ContourGuiApp::showNotification);

    return terminalWindows_.back();
}

TerminalWindow* ContourGuiApp::newWindow()
{
    auto const liveConfig = parameters().get<bool>("contour.terminal.live-config");
    auto mainWindow =
        new TerminalWindow(earlyExitThreshold(), config_, liveConfig, profileName(), argv_[0], *this);
    mainWindow->show();

    terminalWindows_.emplace_back(mainWindow);
    return terminalWindows_.back();
}

void ContourGuiApp::showNotification(std::string_view _title, std::string_view _content)
{
    // systrayIcon_->showMessage(
    //     _title,
    //     _content,
    //     QSystemTrayIcon::MessageIcon::Information,
    //     10 * 1000
    // );

#if defined(__linux__)
    // XXX requires notify-send to be installed.
    QStringList args;
    args.append("--urgency=low");
    args.append("--expire-time=10000");
    args.append("--category=terminal");
    args.append(QString::fromStdString(string(_title)));
    args.append(QString::fromStdString(string(_content)));
    QProcess::execute(QString::fromLatin1("notify-send"), args);
#elif defined(__APPLE__)
    // TODO: use Growl?
    (void) _title;
    (void) _content;
#elif defined(_WIN32)
    // TODO: use Toast
    (void) _title;
    (void) _content;
#endif
}

} // namespace contour
