// SPDX-License-Identifier: Apache-2.0
#include <contour/Config.h>
#include <contour/ContourGuiApp.h>
#include <contour/display/TerminalWidget.h>

#include <vtpty/Process.h>

#include <text_shaper/font_locator.h>

#include <crispy/CLI.h>
#include <crispy/logstore.h>
#include <crispy/utils.h>

#include <QtCore/QProcess>
#include <QtGui/QGuiApplication>
#include <QtGui/QSurfaceFormat>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtWidgets/QApplication>

#include <filesystem>
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

using vtpty::Process;

using namespace std::string_literals;

namespace fs = std::filesystem;

namespace CLI = crispy::cli;

namespace contour
{

ContourGuiApp::ContourGuiApp(): _sessionManager(*this)
{
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
    link("contour.font-locator", bind(&ContourGuiApp::fontConfigAction, this));
}

int ContourGuiApp::run(int argc, char const* argv[])
{
    _argc = argc;
    _argv = argv;

    return ContourApp::run(argc, argv);
}

crispy::cli::command ContourGuiApp::parameterDefinition() const
{
    auto command = ContourApp::parameterDefinition();

    command.children.insert(
        command.children.begin(),
        CLI::command {
            "font-locator",
            "Inspects font locator service.",
            CLI::option_list {
                CLI::option { "config",
                              CLI::value { contour::config::defaultConfigFilePath() },
                              "Path to configuration file to load at startup.",
                              "FILE" },
                CLI::option {
                    "profile", CLI::value { ""s }, "Terminal Profile to load (overriding config).", "NAME" },
                CLI::option { "debug",
                              CLI::value { ""s },
                              "Enables debug logging, using a comma (,) seperated list of tags.",
                              "TAGS" },
            },
        });

    command.children.insert(
        command.children.begin(),
        CLI::command {
            "terminal",
            "Spawns a new terminal application.",
            CLI::option_list {
                CLI::option { "config",
                              CLI::value { contour::config::defaultConfigFilePath() },
                              "Path to configuration file to load at startup.",
                              "FILE" },
                CLI::option {
                    "profile", CLI::value { ""s }, "Terminal Profile to load (overriding config).", "NAME" },
                CLI::option { "debug",
                              CLI::value { ""s },
                              "Enables debug logging, using a comma (,) seperated list of tags.",
                              "TAGS" },
                CLI::option { "live-config", CLI::value { false }, "Enables live config reloading." },
                CLI::option {
                    "dump-state-at-exit",
                    CLI::value { ""s },
                    "Dumps internal state at exit into the given directory. This is for debugging contour.",
                    "PATH" },
                CLI::option { "early-exit-threshold",
                              CLI::value { 6u },
                              "If the spawned process exits earlier than the given threshold seconds, an "
                              "error message will be printed and the window not closed immediately." },
                CLI::option { "working-directory",
                              CLI::value { ""s },
                              "Sets initial working directory (overriding config).",
                              "DIRECTORY" },
                CLI::option {
                    "class",
                    CLI::value { ""s },
                    "Sets the class part of the WM_CLASS property for the window (overriding config).",
                    "WM_CLASS" },
                CLI::option {
                    "platform", CLI::value { ""s }, "Sets the QPA platform.", "PLATFORM[:OPTIONS]" },
                CLI::option { "session",
                              CLI::value { ""s },
                              "Sets the sessioni ID used for resuming a prior session.",
                              "SESSION_ID" },
#if defined(__linux__)
                CLI::option {
                    "display", CLI::value { ""s }, "Sets the X11 display to connect to.", "DISPLAY_ID" },
#endif
                CLI::option {
                    CLI::option_name { 'e', "execute" },
                    CLI::value { ""s },
                    "DEPRECATED: Program to execute instead of running the shell as configured.",
                    "PROGRAM",
                    CLI::presence::Optional,
                    CLI::deprecated { "Only supported for compatibility with very old KDE desktops." } },
            },
            CLI::command_list {},
            CLI::command_select::Implicit,
            CLI::verbatim { "PROGRAM ARGS...",
                            "Executes given program instead of the one provided in the configuration." } });

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

    if (!_config.defaultProfileName.empty())
        return _config.defaultProfileName;

    if (_config.profiles.size() == 1)
        return _config.profiles.begin()->first;

    return ""s;
}

std::optional<fs::path> ContourGuiApp::dumpStateAtExit() const
{
    auto const path = parameters().get<std::string>("contour.terminal.dump-state-at-exit");
    if (path.empty())
        return std::nullopt;
    return fs::path(path);
}

void ContourGuiApp::onExit(TerminalSession& session)
{
    auto const* localProcess = dynamic_cast<vtpty::Process const*>(&session.terminal().device());
    if (!localProcess)
        return;

    _exitStatus = localProcess->checkStatus();
}

QUrl ContourGuiApp::resolveResource(std::string_view path)
{
    auto const localPath = config::configHome() / fs::path(path.data());
    if (fs::exists(localPath))
        return QUrl::fromLocalFile(QString::fromStdString(localPath.generic_string()));

#if !defined(NDEBUG) && defined(CONTOUR_GUI_SOURCE_DIR)
    auto const devPath = fs::path(CONTOUR_GUI_SOURCE_DIR) / fs::path(path.data());
    if (fs::exists(devPath))
        return QUrl::fromLocalFile(QString::fromStdString(devPath.generic_string()));
#endif

    return QUrl("qrc:/contour/" + QString::fromLatin1(path.data(), static_cast<int>(path.size())));
}

bool ContourGuiApp::loadConfig(string const& target)
{
    auto const& flags = parameters();
    auto const prefix = "contour." + target + ".";

    auto configFailures = int { 0 };
    auto const configLogger = [&](string const& msg) {
        cerr << "Configuration failure. " << msg << '\n';
        ++configFailures;
    };

    if (auto const filterString = flags.get<string>(prefix + "debug"); !filterString.empty())
    {
        logstore::configure(filterString);
    }

    auto const configPath = QString::fromStdString(flags.get<string>(prefix + "config"));

    _config = configPath.isEmpty() ? contour::config::loadConfig()
                                   : contour::config::loadConfigFromFile(configPath.toStdString());

    _config.live = _config.live || parameters().boolean("contour.terminal.live-config");

    if (!_config.profile(profileName()))
    {
        auto const s =
            accumulate(begin(_config.profiles),
                       end(_config.profiles),
                       ""s,
                       [](string const& acc, auto const& profile) -> string {
                           return acc.empty() ? profile.first : fmt::format("{}, {}", acc, profile.first);
                       });
        configLogger(
            fmt::format("No profile with name '{}' found. Available profiles: {}", profileName(), s));
    }

    if (auto const wd = flags.get<string>("contour.terminal.working-directory"); !wd.empty())
        _config.profile(profileName())->shell.workingDirectory = fs::path(wd);

    config::TerminalProfile* profile = _config.profile(profileName());
    if (!profile)
        configLogger("Could not resolve configuration profile.");

    if (configFailures)
        return EXIT_FAILURE;

    // Possibly override shell to be executed
    auto exe = flags.get<string>("contour.terminal.execute");
    if (!flags.verbatim.empty() || !exe.empty())
    {
        auto& shell = profile->shell;
        shell.arguments.clear();
        if (!exe.empty())
        {
            shell.program = std::move(exe);
            for (auto i: flags.verbatim)
                shell.arguments.emplace_back(i);
        }
        else
        {
            shell.program = flags.verbatim.front();
            for (size_t i = 1; i < flags.verbatim.size(); ++i)
                shell.arguments.emplace_back(flags.verbatim.at(i));
        }
    }

    if (auto const wmClass = flags.get<string>("contour.terminal.class"); !wmClass.empty())
        _config.profile(profileName())->wmClass = wmClass;

    return true;
}

int ContourGuiApp::fontConfigAction()
{
    if (!loadConfig("font-locator"))
        return EXIT_FAILURE;

    vtrasterizer::FontDescriptions const& fonts = _config.profile(_config.defaultProfileName)->fonts;
    text::font_description const& fontDescription = fonts.regular;
    text::font_locator& fontLocator = createFontLocator(fonts.fontLocator);
    text::font_source_list fontSources = fontLocator.locate(fontDescription);

    fmt::print("Matching fonts using  : {}\n", fonts.fontLocator);
    fmt::print("Font description      : {}\n", fontDescription);
    fmt::print("Number of fonts found : {}\n", fontSources.size());
    for (text::font_source const& fontSource: fontSources)
        fmt::print("  {}\n", fontSource);

    return EXIT_SUCCESS;
}

int ContourGuiApp::terminalGuiAction()
{
    if (!loadConfig("terminal"))
        return EXIT_FAILURE;

    switch (_config.renderingBackend)
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

    auto const* profile = _config.profile(profileName());
    if (!profile)
    {
        errorLog()("Could not access configuration profile.");
        return EXIT_FAILURE;
    }
    auto appName = QString::fromStdString(profile->wmClass);
    QCoreApplication::setApplicationName(appName);
    QCoreApplication::setOrganizationName("contour");
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);

    vector<string> qtArgsStore;
    vector<char const*> qtArgsPtr;
    qtArgsPtr.push_back(_argv[0]);

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
        if (!_config.platformPlugin.empty())
        {
            static constexpr auto PlatformArg = string_view("-platform");
            qtArgsPtr.push_back(PlatformArg.data());
            qtArgsPtr.push_back(_config.platformPlugin.c_str());
        }
    }

#if defined(__linux__)
    addQtArgIfSet("contour.terminal.display", "-display");
#endif

    auto qtArgsCount = static_cast<int>(qtArgsPtr.size());

    // NB: We use QApplication over QGuiApplication because we want to use SystemTrayIcon.
    QApplication app(qtArgsCount, (char**) qtArgsPtr.data());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Enforce OpenGL over any other. As much as I'd love to provide other backends, too.
    // We currently only support OpenGL.
    // If anyone feels happy about it, I'd love to at least provide Vulkan. ;-)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif

    QGuiApplication::setWindowIcon(QIcon(":/contour/logo-256.png"));

    QSurfaceFormat::setDefaultFormat(display::createSurfaceFormat());

    ensureTermInfoFile();

    // clang-format off
    qmlRegisterType<display::TerminalWidget>("Contour.Terminal", 1, 0, "ContourTerminal");
    qmlRegisterUncreatableType<TerminalSession>("Contour.Terminal", 1, 0, "TerminalSession", "Use factory.");
    qmlRegisterUncreatableType<TerminalSessionManager>("Contour.Terminal", 1, 0, "TerminalSessionManager", "Do not use me directly.");
    qRegisterMetaType<TerminalSession*>("TerminalSession*");
    // clang-format on

    _qmlEngine = make_unique<QQmlApplicationEngine>();

    QQmlContext* context = _qmlEngine->rootContext();
    context->setContextProperty("terminalSessions", &_sessionManager);

    // auto const HTS = "\033H";
    // auto const TBC = "\033[g";
    // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

    // Spawn initial window.
    newWindow();

    if (auto const& bell = config().profile().bell; bell == "off")
    {
        if (auto* bellAudioOutput = _qmlEngine->rootObjects().first()->findChild<QObject*>("BellAudioOutput");
            bellAudioOutput)
            bellAudioOutput->setProperty("muted", true);
    }
    else if (bell != "default")
    {
        QUrl const path(bell.c_str());
        if (auto* bellObject = _qmlEngine->rootObjects().first()->findChild<QObject*>("Bell"); bellObject)
            bellObject->setProperty("source", path);
    }

    auto rv = QApplication::exec();

    if (_exitStatus.has_value())
    {
        if (holds_alternative<Process::NormalExit>(*_exitStatus))
            rv = get<Process::NormalExit>(*_exitStatus).exitCode;
        else if (holds_alternative<Process::SignalExit>(*_exitStatus))
            rv = EXIT_FAILURE;
    }

    // Explicitly destroy QML engine here to ensure it's being destructed before QGuiApplication.
    _qmlEngine.reset();

    // printf("\r%s", TBC);
    return rv;
}

void ContourGuiApp::ensureTermInfoFile()
{
    if (!vtpty::Process::isFlatpak())
        return;

    auto const hostTerminfoBaseDirectory =
        Process::homeDirectory() / ".var/app/org.contourterminal.Contour/terminfo/c";
    if (!fs::is_directory(hostTerminfoBaseDirectory))
        fs::create_directories(hostTerminfoBaseDirectory);

    auto const sandboxTerminfoFile = fs::path("/app/share/terminfo/c/contour");
    if (!fs::is_regular_file(hostTerminfoBaseDirectory / "contour"))
        fs::copy_file(sandboxTerminfoFile, hostTerminfoBaseDirectory / "contour");
}

void ContourGuiApp::newWindow()
{
    _qmlEngine->load(resolveResource("ui/main.qml"));
}

void ContourGuiApp::showNotification(std::string_view title, std::string_view content)
{
    // systrayIcon_->showMessage(
    //     title,
    //     content,
    //     QSystemTrayIcon::MessageIcon::Information,
    //     10 * 1000
    // );

#if defined(__linux__)
    // XXX requires notify-send to be installed.
    QStringList args;
    args.append("--urgency=low");
    args.append("--expire-time=10000");
    args.append("--category=terminal");
    args.append(QString::fromStdString(string(title)));
    args.append(QString::fromStdString(string(content)));
    QProcess::execute(QString::fromLatin1("notify-send"), args);
#elif defined(__APPLE__)
    // TODO: use Growl?
    (void) title;
    (void) content;
#elif defined(_WIN32)
    // TODO: use Toast
    (void) title;
    (void) content;
#else
    crispy::ignore_unused(title, content);
#endif
}

} // namespace contour
