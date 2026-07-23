// SPDX-License-Identifier: Apache-2.0
#include <contour/CommandPaletteModel.h>
#include <contour/Config.h>
#include <contour/ContourGuiApp.h>
#include <contour/GuiTheme.h>
#include <contour/PaneProxy.h>
#include <contour/QtExternalLauncher.h>
#include <contour/RenderingBackendSelection.h>
#include <contour/SessionFactory.h>
#include <contour/SettingsController.h>
#include <contour/WindowController.h>
#include <contour/display/ContentScale.h>
#include <contour/display/TerminalAccessible.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/mux/AttachController.h>
#include <contour/mux/RemoteLayout.h>
#include <contour/mux/RoutingSessionFactory.h>
#include <contour/mux/TmuxController.h>

#include <vtpty/Process.h>

#include <text_shaper/font_locator.h>

#include <crispy/CLI.h>
#include <crispy/logstore.h>
#include <crispy/utils.h>

#include <QtCore/QEventLoop>
#include <QtCore/QProcess>
#include <QtQml/qqmlextensionplugin.h>

#include <muxserver/SocketPath.h>
#if !defined(__APPLE__) && !defined(_WIN32)
    #include <QtDBus/QDBusConnection>
#endif
#include <QtCore/QtPlugin>
#include <QtDBus/QtDBus>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtGui/QSurfaceFormat>
#include <QtMultimedia/QMediaDevices>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

#include <QtQuickControls2/QQuickStyle>

using std::bind;
using std::cerr;
using std::make_unique;
using std::string;
using std::string_view;
using std::vector;

using vtpty::Process;
using vtpty::SshSession;

using namespace std::string_literals;

namespace fs = std::filesystem;

namespace CLI = crispy::cli;

namespace contour
{

ContourGuiApp::ContourGuiApp(std::unique_ptr<SessionFactory> sessionFactory,
                             std::unique_ptr<ExternalLauncher> externalLauncher,
                             std::unique_ptr<LayoutStore> layoutStore,
                             std::unique_ptr<CommandHistoryStore> commandHistoryStore):
    _sessionFactory(std::make_unique<RoutingSessionFactory>(
        sessionFactory ? std::move(sessionFactory) : std::make_unique<AppSessionFactory>(*this))),
    _routingFactory(static_cast<RoutingSessionFactory*>(_sessionFactory.get())),
    _externalLauncher(externalLauncher ? std::move(externalLauncher)
                                       : std::make_unique<QtExternalLauncher>()),
    _layoutStore(layoutStore ? std::move(layoutStore) : std::make_unique<FileLayoutStore>()),
    _commandHistoryStore(commandHistoryStore ? std::move(commandHistoryStore)
                                             : std::make_unique<FileCommandHistoryStore>()),
    _sessionManager(*this, *_sessionFactory, *_layoutStore, *_commandHistoryStore)
{
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
    link("contour.font-locator", bind(&ContourGuiApp::fontConfigAction, this));
    link("contour.info.config", bind(&ContourGuiApp::checkConfig, this));
    link("contour.attach", bind(&ContourGuiApp::attachAction, this));
}

ContourGuiApp::~ContourGuiApp() = default;

int ContourGuiApp::attachAction()
{
    auto const wantsTmux = parameters().get<bool>("contour.attach.tmux");
    if (!wantsTmux && !parameters().get<bool>("contour.attach.gui"))
        return ContourApp::attachAction();

    // Resolve every attach-verb parameter BEFORE the re-parse below drops them.
    auto const socketOption = parameters().get<string>("contour.attach.socket");
    auto const label = parameters().get<string>("contour.attach.label");
    auto const attachProfile = parameters().get<string>("contour.attach.profile");
    auto const attachConfig = parameters().get<string>("contour.attach.config");
    auto const tmuxSocket = parameters().get<string>("contour.attach.tmux-socket");
    auto const connectTcp = parameters().get<string>("contour.attach.connect-tcp");
    auto const tcpToken = parameters().get<string>("contour.attach.token");
    auto const tlsCaPath = parameters().get<string>("contour.attach.tls-ca");

    auto adopt = std::function<void()> {};
    if (wantsTmux)
    {
        _tmuxController = std::make_unique<TmuxController>(tmuxSocket);
        if (auto connected = _tmuxController->connectAndWait(std::chrono::seconds(10)); !connected)
        {
            cerr << std::format("contour attach --tmux: {}\n", connected.error());
            _tmuxController.reset();
            return EXIT_FAILURE;
        }
        _routingFactory->setDelegate(_tmuxController.get());
        adopt = [this] {
            if (auto const window = _sessionManager.focusedWindow())
                _tmuxController->adoptPendingPanes(_sessionManager, *window);
        };
        connect(
            _tmuxController.get(), &TmuxController::remotePaneDiscovered, this, adopt, Qt::QueuedConnection);
        // A %window-renamed reflects onto the owning tab's title (a tmux window is a tab).
        connect(
            _tmuxController.get(),
            &TmuxController::tabTitleChanged,
            this,
            [this] { _tmuxController->applyPendingRenames(_sessionManager); },
            Qt::QueuedConnection);
        connect(
            _tmuxController.get(),
            &TmuxController::connectionClosed,
            this,
            [this] { _tmuxController->stop(); },
            Qt::QueuedConnection);
    }
    else
    {
        // Build the daemon endpoint: a TLS-encrypted TCP connection when
        // --connect-tcp is given, otherwise the local control socket (connectAttach
        // resolves the native socket beside it).
        auto endpoint = muxserver::AttachEndpoint {};
        auto endpointLabel = std::string {};
        if (!connectTcp.empty())
        {
            auto const hostPort = muxserver::parseHostPort(connectTcp);
            if (!hostPort)
            {
                cerr << std::format("contour attach: invalid --connect-tcp '{}' (expected HOST:PORT)\n",
                                    connectTcp);
                return EXIT_FAILURE;
            }
            auto tcp = muxserver::TcpEndpoint {
                .host = hostPort->first, .port = hostPort->second, .token = tcpToken, .caPem = {}
            };
            if (!tlsCaPath.empty())
            {
                auto const path = std::filesystem::path(tlsCaPath);
                // An empty CA read must hard-fail exactly like the thin client:
                // makeTlsClientContext treats "" as the TOFU/no-verify posture, so
                // accepting it would silently strip the pinning the user asked for.
                auto caPem = std::string {};
                if (std::filesystem::is_regular_file(path))
                    caPem = crispy::readFileAsString(path);
                if (caPem.empty())
                {
                    cerr << std::format("contour attach: cannot read --tls-ca file '{}'\n", tlsCaPath);
                    return EXIT_FAILURE;
                }
                tcp.caPem = std::move(caPem);
            }
            endpointLabel = std::format("{}:{}", tcp.host, tcp.port);
            endpoint = std::move(tcp);
        }
        else
        {
            auto const controlPath = muxserver::muxSocketPath(label, socketOption);
            endpointLabel = controlPath.string();
            endpoint = muxserver::UnixEndpoint { .socketPath = controlPath };
        }

        _attachController = std::make_unique<AttachController>(std::move(endpoint));
        if (auto connected = _attachController->connectAndWait(std::chrono::seconds(5)); !connected)
        {
            cerr << std::format("contour attach: {} ({})\n", connected.error(), endpointLabel);
            _attachController.reset();
            return EXIT_FAILURE;
        }
        _routingFactory->setDelegate(_attachController.get());
        // A window spawned to host a daemon window binds itself here (from its
        // main.qml, via consumeAttachWindow) instead of creating a fresh first tab.
        _sessionManager.setAttachWindowBinder(
            [this](WindowController* controller) { return bindPendingAttachWindow(controller); });
        // Reconcile the GUI against the daemon's authoritative window→tab→split tree
        // (B2/B4): whenever a layout arrives or changes (GUI boot, or a window/tab/
        // split authored on the daemon, here or by another client), map each daemon
        // window onto an OS window and realize any tab not yet shown, each pane bound
        // to its remote session. Incremental — what is already shown is left untouched.
        // A dying connection ends every mirror session (stop() closes the ptys),
        // closing the tabs through the shell-exit teardown.
        adopt = [this] {
            reconcileAttachWindows();
        };
        connect(_attachController.get(), &AttachController::layoutChanged, this, adopt, Qt::QueuedConnection);
        connect(
            _attachController.get(),
            &AttachController::connectionClosed,
            this,
            [this] { _attachController->stop(); },
            Qt::QueuedConnection);
    }

    // Boot the GUI under the terminal verb's parameter surface: re-parse a
    // synthetic argv so every contour.terminal.* key resolves, carrying the
    // attach-specific profile/config choices over.
    auto argv = std::vector<char const*> { "contour", "terminal" };
    if (!attachProfile.empty())
    {
        argv.push_back("profile");
        argv.push_back(attachProfile.c_str());
    }
    if (!attachConfig.empty())
    {
        argv.push_back("config");
        argv.push_back(attachConfig.c_str());
    }
    auto const stopControllers = [this] {
        if (_attachController)
            _attachController->stop();
        if (_tmuxController)
            _tmuxController->stop();
    };
    if (!reparseParameters(static_cast<int>(argv.size()), argv.data()))
    {
        _routingFactory->setDelegate(nullptr);
        stopControllers();
        return EXIT_FAILURE;
    }

    _onGuiBooted = adopt;
    auto const rv = terminalGuiAction();

    _onGuiBooted = nullptr;
    _routingFactory->setDelegate(nullptr);
    stopControllers();
    return rv;
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

    // NOLINTBEGIN
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
                              CLI::value { -1 },
                              "If the spawned process exits earlier than the given threshold seconds, an "
                              "error message will be printed and the window not closed immediately." },
                CLI::option { "working-directory",
                              CLI::value { ""s },
                              "Sets initial working directory (overriding config).",
                              "DIRECTORY" },
                CLI::option { "layout", CLI::value { ""s }, "Opens the named layout at startup.", "NAME" },
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

    // NOLINTEND
    return command;
}

std::chrono::seconds ContourGuiApp::earlyExitThreshold() const
{
    auto const configThreshold = config().earlyExitThreshold.value();
    auto const parameterThreshold = parameters().get<int>("contour.terminal.early-exit-threshold");

    // default threshold is config::documentation::DefaultEarlyExitThreshold seconds
    if (parameterThreshold >= 0)
        return std::chrono::seconds(parameterThreshold);

    if (configThreshold != config::documentation::DefaultEarlyExitThreshold)
        return std::chrono::seconds(configThreshold);

    return std::chrono::seconds(config::documentation::DefaultEarlyExitThreshold);
}

string ContourGuiApp::profileName() const
{
    if (auto profile = parameters().get<string>("contour.terminal.profile"); !profile.empty())
        return profile;

    if (!_config.defaultProfileName.value().empty())
        return _config.defaultProfileName.value();

    if (_config.profiles.value().size() == 1)
        return _config.profiles.value().begin()->first;

    return ""s;
}

std::string ContourGuiApp::layoutName() const
{
    if (auto name = parameters().get<std::string>("contour.terminal.layout"); !name.empty())
        return name;
    return config().defaultLayoutName.value();
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
    if (auto const* localProcess = dynamic_cast<vtpty::Process const*>(&session.terminal().device()))
        _exitStatus = localProcess->checkStatus();
#ifdef VTPTY_LIBSSH2
    else if (auto const* sshSession = dynamic_cast<vtpty::SshSession const*>(&session.terminal().device()))
        _exitStatus = sshSession->exitStatus();
#endif
}

void ContourGuiApp::applyGuiTheme(config::GuiTheme theme)
{
    // qtColorSchemeFor() is the pure decision (see GuiTheme.h): a forced scheme for dark/light,
    // std::nullopt for system.
    auto const scheme = qtColorSchemeFor(theme);

    // Force the chrome palette explicitly. This is the load-bearing step: QStyleHints::setColorScheme
    // does NOT regenerate QGuiApplication::palette() on platforms whose platform-theme plugin owns the
    // palette (KDE Plasma, GNOME, …), so relying on it alone leaves the chrome uncolored on the Linux
    // desktop. Setting the application palette via setPalette does recolor it, and every QML
    // SystemPalette follows — so all chrome recolors without touching any component.
    if (scheme)
    {
        // Capture the OS palette once, before the first override, so System can later restore it.
        if (!_guiPaletteOverridden)
        {
            _guiSystemPalette = QGuiApplication::palette();
            _guiPaletteOverridden = true;
        }
        displayLog()("Applying GUI theme override: {}", theme);
        QGuiApplication::setPalette(buildThemePalette(*scheme));
    }
    else if (_guiPaletteOverridden)
    {
        // Returning to System after a prior dark/light override: restore the captured OS palette.
        // Note: once an explicit palette has been set, Qt no longer auto-tracks live OS theme
        // switches for the chrome until the next restart (the documented System-mode tradeoff).
        displayLog()("Applying GUI theme: system (restore OS palette)");
        QGuiApplication::setPalette(_guiSystemPalette);
        _guiPaletteOverridden = false;
    }
    else
        displayLog()("Applying GUI theme: system (follow OS color scheme)");

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Also drive QStyleHints so QStyleHints::colorScheme() reports the pinned scheme: Qt Quick
    // Controls internals and the platforms that DO regenerate their own palette (Windows/macOS) honor
    // it. On the palette-owning Linux platform themes this is inert on its own — hence the explicit
    // setPalette above. Setting the scheme emits no colorSchemeChanged to the terminal grid unless the
    // reported scheme actually changes, and the grid handler is gated to System mode regardless.
    if (scheme)
        QGuiApplication::styleHints()->setColorScheme(*scheme);
    else
        QGuiApplication::styleHints()->unsetColorScheme();
#endif
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

    return { "qrc:/contour/" + QString::fromLatin1(path.data(), static_cast<int>(path.size())) };
}

int ContourGuiApp::checkConfig()
{
    auto const& flags = parameters();
    auto const configPath = QString::fromStdString(flags.get<string>("contour.terminal.config"));

    _config = configPath.isEmpty() ? contour::config::loadConfig()
                                   : contour::config::loadConfigFromFile(configPath.toStdString());

    contour::config::compareEntries(
        _config, logstore::category("", "Console Logger", logstore::category::state::Enabled));

    return EXIT_SUCCESS;
}

bool ContourGuiApp::loadConfig(string const& target)
{
    auto const& flags = parameters();
    auto const prefix = "contour." + target + ".";

    auto configFailures = 0;
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

    _config.live.value() = _config.live.value() || parameters().boolean("contour.terminal.live-config");

    if (!_config.profile(profileName()))
    {
        auto const s =
            accumulate(begin(_config.profiles.value()),
                       end(_config.profiles.value()),
                       ""s,
                       [](string const& acc, auto const& profile) -> string {
                           return acc.empty() ? profile.first : std::format("{}, {}", acc, profile.first);
                       });
        configLogger(
            std::format("No profile with name '{}' found. Available profiles: {}", profileName(), s));
    }

    if (auto const wd = flags.get<string>("contour.terminal.working-directory"); !wd.empty())
        _config.profile(profileName())->shell.value().workingDirectory = fs::path(wd);

    config::TerminalProfile* profile = _config.profile(profileName());
    if (!profile)
        configLogger("Could not resolve configuration profile.");

    if (configFailures)
        return EXIT_FAILURE;

    // Possibly override shell to be executed
    auto exe = flags.get<string>("contour.terminal.execute");
    if (!flags.verbatim.empty() || !exe.empty())
    {
        auto& shell = profile->shell.value();
        auto const profileShellProgram = shell.program;
        shell.arguments.clear();
        if (!exe.empty())
        {
            shell.program = std::move(exe);
            for (auto i: flags.verbatim)
                shell.arguments.emplace_back(i);
        }
        else
        {
            auto frontCommand = flags.verbatim.front();

            // check if this is a file
            if (fs::exists(frontCommand) && fs::is_regular_file(frontCommand))
            {
                // check if this is an executable file
                if ((fs::status(frontCommand).permissions() & fs::perms::owner_exec) != fs::perms::none)
                    shell.program = frontCommand;
                else // find a path to file and open shell in this path
                    shell.workingDirectory = fs::path(frontCommand).parent_path();
            }
            else if (fs::exists(frontCommand) && fs::is_directory(frontCommand))
            {
                shell.workingDirectory = fs::path(frontCommand);
            }
            else
            {
                errorLog()("Do not know what to do with `{}` will use it as a program", frontCommand);
                shell.program = frontCommand;
            }

            for (size_t i = 1; i < flags.verbatim.size(); ++i)
                shell.arguments.emplace_back(flags.verbatim.at(i));
        }

        // Record the effective CLI command (the directory-only branches above leave the shell
        // program untouched and carry no command to record) — see cliCommand().
        if (shell.program != profileShellProgram)
        {
            auto command = vtpty::Process::ExecInfo {};
            command.program = shell.program;
            command.arguments = shell.arguments;
            _cliCommand = std::move(command);
        }
    }

    if (auto const wmClass = flags.get<string>("contour.terminal.class"); !wmClass.empty())
    {
        auto* profile = _config.profile(profileName());
        if (profile)
        {
            profile->wmClass = wmClass;
        }
    }

    return true;
}

int ContourGuiApp::fontConfigAction()
{
    if (!loadConfig("font-locator"))
        return EXIT_FAILURE;

    vtrasterizer::FontDescriptions const& fonts =
        _config.profile(_config.defaultProfileName.value())->fonts.value();
    text::font_description const& fontDescription = fonts.regular;
    text::font_locator& fontLocator = createFontLocator(fonts.fontLocator);
    text::font_source_list const fontSources = fontLocator.locate(fontDescription);

    std::cout << std::format("Matching fonts using  : {}\n", fonts.fontLocator);
    std::cout << std::format("Font description      : {}\n", fontDescription);
    std::cout << std::format("Number of fonts found : {}\n", fontSources.size());
    for (text::font_source const& fontSource: fontSources)
        std::cout << std::format("  {}\n", fontSource);

    return EXIT_SUCCESS;
}

int ContourGuiApp::terminalGuiAction()
{
    {
        auto const timer = ScopedTimer(startupLog, "loadConfig");
        if (!loadConfig("terminal"))
            return EXIT_FAILURE;
    }

#ifdef __APPLE__
    QGuiApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, true);
#endif

    // Software rendering must be requested before the QApplication is constructed. The concrete RHI
    // graphics API (OpenGL/Vulkan/Direct3D/Metal/auto) is selected further below via
    // QQuickWindow::setGraphicsApi, once the QApplication exists.
    QGuiApplication::setAttribute(Qt::AA_UseSoftwareOpenGL,
                                  _config.renderer.value().renderingBackend
                                      == config::RenderingBackend::Software);

    auto const* profile = _config.profile(profileName());
    if (!profile)
    {
        errorLog()("Could not access configuration profile.");
        return EXIT_FAILURE;
    }
    auto appName = QString::fromStdString(profile->wmClass.value());
    QCoreApplication::setApplicationName(appName);
    QCoreApplication::setOrganizationName("contour");
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
    // Expose the profile name to the shell.
    _config.profile(profileName())->shell.value().env["CONTOUR_PROFILE"] = profileName();

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
        if (!_config.platformPlugin.value().empty())
        {
            static constexpr auto PlatformArg = string_view("-platform");
            qtArgsPtr.push_back(PlatformArg.data());
            qtArgsPtr.push_back(_config.platformPlugin.value().c_str());
        }
    }

#ifdef __linux__
    addQtArgIfSet("contour.terminal.display", "-display");
#endif

    auto qtArgsCount = static_cast<int>(qtArgsPtr.size());

    // NB: We use QApplication over QGuiApplication because we want to use SystemTrayIcon.
    // NOTE: Cannot use ScopedTimer here because QApplication must live until end of function.
    auto const qtInitStart = std::chrono::steady_clock::now();
    QApplication const app(qtArgsCount, (char**) qtArgsPtr.data());
    setupQCoreApplication();
    if (startupLog.is_enabled())
    {
        auto const elapsed = std::chrono::steady_clock::now() - qtInitStart;
        auto const ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count())
            / 1000.0;
        startupLog()("QApplication + setup: {:.1f} ms", ms);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Seed the terminal's light/dark preference from the *real* OS color scheme. This is read
    // before applyGuiTheme() may override the application color scheme below, so a force-pinned GUI
    // theme (theme: dark|light) never drags the terminal grid away from the OS preference.
    _colorPreference = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                           ? vtbackend::ColorPreference::Dark
                           : vtbackend::ColorPreference::Light;

    displayLog()("Color theme mode at startup: {}", _colorPreference);

    connect(
        QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, [this](Qt::ColorScheme newScheme) {
            // Only the terminal grid tracks the OS here, and only while the GUI theme defers to the
            // system (theme: system). When the GUI theme is force-pinned (dark/light), colorScheme()
            // reflects our own override, which must not move the terminal palette.
            if (_config.theme.value() != config::GuiTheme::System)
                return;

            auto const newValue = newScheme == Qt::ColorScheme::Dark ? vtbackend::ColorPreference::Dark
                                                                     : vtbackend::ColorPreference::Light;
            if (_colorPreference == newValue)
                return;

            _colorPreference = newValue;
            displayLog()("Color preference changed to {} mode\n", _colorPreference);
            sessionsManager().updateColorPreference(_colorPreference);
        });
#endif

    // Apply the configured GUI chrome theme (dark/light/system). Must run after the OS scheme has
    // been captured above and before the first QQuickWindow is created, so the chrome opens themed.
    applyGuiTheme(_config.theme.value());

    // Pin the Fusion Qt Quick Controls style app-wide so the hand-drawn tab strip, its controls, and the
    // tab context menu render, blend, and stay readable on every OS. Qt otherwise picks the native style
    // per platform (e.g. Windows/FluentWinUI3 on Qt 6.7+, whose opaque button chrome and dark-only menu
    // text clash with our custom palette-driven title bar) or the flat Basic style, neither of which
    // gives the menu an opaque, themed, readable surface on our transparent window. NB: set
    // unconditionally — QQuickStyle::name() already resolves to the (non-empty) native default here, so
    // guarding on it silently skips Fusion.
    QQuickStyle::setStyle("Fusion");

    // Select the Qt RHI graphics API from the configured renderer backend, before the first
    // QQuickWindow is created. RenderingBackend::Auto leaves the choice to Qt, which resolves the
    // platform-native backend (Direct3D 11 on Windows, Metal on macOS, OpenGL on Linux). The renderer
    // is backend-portable (RhiRenderer uses QRhi::clipSpaceCorrMatrix()/isYUpInFramebuffer(), and the
    // shaders are compiled for every backend), so no GL-specific assumptions leak through here.
    auto const graphicsApiFor =
        [](config::RenderingBackend backend) -> std::optional<QSGRendererInterface::GraphicsApi> {
        switch (backend)
        {
            case config::RenderingBackend::Auto: return std::nullopt;
            case config::RenderingBackend::OpenGL: return QSGRendererInterface::OpenGL;
            case config::RenderingBackend::Software: return QSGRendererInterface::OpenGL;
            case config::RenderingBackend::Vulkan: return QSGRendererInterface::Vulkan;
            case config::RenderingBackend::Direct3D11: return QSGRendererInterface::Direct3D11;
            case config::RenderingBackend::Direct3D12: return QSGRendererInterface::Direct3D12;
            case config::RenderingBackend::Metal: return QSGRendererInterface::Metal;
        }
        return std::nullopt;
    };

    // Resolve the configured backend against what this platform can actually composite (see
    // RenderingBackendSelection.h): an unavailable backend — including desktop OpenGL on macOS, which
    // maps a window but never composites the scene graph, leaving the user with an invisible window —
    // self-heals to Auto so a mis-set config never prevents startup or hangs on a dead window.
    auto const configuredBackend = _config.renderer.value().renderingBackend;
    auto const requestedBackend = resolveRenderingBackend(currentRhiPlatform(), configuredBackend);
    if (requestedBackend != configuredBackend)
        errorLog()("Renderer backend {} is not available on this platform; falling back to auto.",
                   configuredBackend);

    if (auto const api = graphicsApiFor(requestedBackend))
        QQuickWindow::setGraphicsApi(*api);

    QGuiApplication::setWindowIcon(QIcon(":/contour/logo-256.png"));

    QSurfaceFormat::setDefaultFormat(display::createSurfaceFormat());

    // Hands out the accessibility interface for the terminal item, so OS magnifiers and screen readers
    // can follow the caret. Installed here rather than from a static initializer: the ordering against
    // the application object matters, and the factory must be in place before the first item is created.
    display::TerminalAccessible::installFactory();

    ensureTermInfoFile();

    // clang-format off
    qmlRegisterType<display::TerminalDisplay>("Contour.Terminal", 1, 0, "ContourTerminal");
    qmlRegisterUncreatableType<TerminalSession>("Contour.Terminal", 1, 0, "TerminalSession", "Use factory.");
    qmlRegisterUncreatableType<TerminalSessionManager>("Contour.Terminal", 1, 0, "TerminalSessionManager", "Do not use me directly.");
    qmlRegisterUncreatableType<PaneProxy>("Contour.Terminal", 1, 0, "PaneProxy", "Created by the session manager.");
    qmlRegisterUncreatableType<WindowController>("Contour.Terminal", 1, 0, "WindowController", "Created by the session manager.");
    qmlRegisterUncreatableType<CommandPaletteModel>("Contour.Terminal", 1, 0, "CommandPaletteModel", "Created by the window controller.");
    qmlRegisterUncreatableType<SettingsController>("Contour.Terminal", 1, 0, "SettingsController", "Created by the window controller.");
    qRegisterMetaType<TerminalSession*>("TerminalSession*");
    qRegisterMetaType<PaneProxy*>("PaneProxy*");
    qRegisterMetaType<WindowController*>("WindowController*");
    qRegisterMetaType<CommandPaletteModel*>("CommandPaletteModel*");
    qRegisterMetaType<SettingsController*>("SettingsController*");
    // clang-format on

    {
        auto const timer = ScopedTimer(startupLog, "QML engine setup");
        _qmlEngine = make_unique<QQmlApplicationEngine>();

        QQmlContext* context = _qmlEngine->rootContext();
        context->setContextProperty("terminalSessions", &_sessionManager);
    }

    // auto const HTS = "\033H";
    // auto const TBC = "\033[g";
    // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

    // Pre-warm the Qt multimedia singleton on a background thread.
    // This triggers the expensive FFmpeg/VDPAU/VA-API/Vulkan driver probing
    // without blocking the main thread. When done, we signal QML via
    // multimediaReady so the bell Loader activates only after the probe completes.
    //
    // Skipped entirely when no profile can ever ring an audible bell -- and that is a CRASH fix, not
    // a speed one. The probe brings up a PipeWire client and its monitor threads, and those race with
    // Qt Multimedia's teardown when the process exits before they have settled. It surfaces as a
    // SIGSEGV inside PipeWire's own protocol thread rather than anywhere in Contour, which is why the
    // event-pumping join below -- which correctly fixes the *deadlock* -- does not fix this.
    //
    // Measured against a five-second session (`contour <config> ucs-detect`), which is short enough
    // to lose the race reliably: 3 crashes in 7 runs with the probe running, 0 in 7 without it.
    //
    // Profiles are checked rather than just the default one, because a profile can be switched to at
    // runtime. A profile that DOES have an audible bell still runs the probe and can still lose the
    // race; that part is an upstream Qt Multimedia/PipeWire teardown issue, and is far less likely in
    // a session that lives longer than its own startup.
    auto const anyProfileHasAudibleBell =
        std::ranges::any_of(_config.profiles.value(),
                            [](auto const& profile) { return profile.second.bell.value().sound != "off"; });

    auto multimediaWarmedUp = std::atomic<bool> { false };
    auto multimediaWarmupThread = std::thread([this, &multimediaWarmedUp, anyProfileHasAudibleBell] {
        if (anyProfileHasAudibleBell)
            QMediaDevices::audioOutputs();
        QMetaObject::invokeMethod(
            &_sessionManager, [this] { _sessionManager.setMultimediaReady(true); }, Qt::QueuedConnection);
        multimediaWarmedUp.store(true, std::memory_order_release);
    });

    // Spawn initial window.
    {
        auto const timer = ScopedTimer(startupLog, "newWindow (QML load)");
        newWindow();
    }

    // Attach mode adopts the remaining remote sessions as tabs now that a
    // window exists to put them in.
    if (_onGuiBooted)
        _onGuiBooted();

    // Run the event loop FIRST (it populates _exitStatus via onExit during the run), THEN map that
    // status to the process exit code. The mapping is the pure exitCodeFor() (ExitCode.h), extracted
    // so it is unit-testable without the event loop. NB: sequenced explicitly — passing exec() as an
    // argument alongside _exitStatus would read _exitStatus before the loop populated it.
    auto const loopResult = QApplication::exec();
    auto const rv = exitCodeFor(_exitStatus, loopResult);

    // Ensure the multimedia warmup thread has finished before destroying Qt objects -- but keep serving
    // this thread's event queue while waiting, rather than blocking straight into join().
    //
    // QMediaDevices::audioOutputs() is not self-contained on a worker thread: underneath, Qt's
    // QPlatformAudioDevices::create() posts a *blocking* QMetaObject::invokeMethod back to the main
    // thread and waits on it. So once exec() has returned, a bare join() deadlocks by construction --
    // the warmup thread waits for an event loop that is gone, while the main thread waits for the warmup
    // thread. That is not hypothetical: it is reachable whenever the app quits before the probe finishes
    // (`contour execute sh -c "exit 0"` is the extreme case), and in practice the process did not even
    // hang to be noticed -- PipeWire's own loop thread crashed on the half-built device monitor first.
    //
    // Pumping until the probe reports done gives the blocking invoke someone to answer it, so the probe
    // completes and the join is immediate. When the probe wins the race (the normal case) the flag is
    // already set and this degenerates to the plain join it was before.
    while (!multimediaWarmedUp.load(std::memory_order_acquire))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    if (multimediaWarmupThread.joinable())
        multimediaWarmupThread.join();

    // Explicitly destroy QML engine here to ensure it's being destructed before QGuiApplication.
    _qmlEngine.reset();

    // printf("\r%s", TBC);
    return rv;
}

void ContourGuiApp::setupQCoreApplication()
{
    auto const* profile = _config.profile(profileName());
    Require(profile);

    auto const defaultAppName = QStringLiteral(CONTOUR_APP_ID);
    auto const defaultOrgDomain = QStringLiteral("contour-terminal.org");
    auto const defaultOrgName = QStringLiteral("contour");

    auto const platformName = QGuiApplication::platformName();
    auto const wmClass = QString::fromStdString(profile->wmClass.value());

    auto const effectiveAppName = [&]() -> QString const& {
        // On X11, we want to set the WM_CLASS property to the configured value.
        if (platformName == "xcb" && !wmClass.isEmpty())
            return wmClass;
        return defaultAppName;
    }();

    // On Wayland, we want to set the Application Id to the configured value via the desktop file name.
    // We use the desktop file name as the application id, because that's what Qt uses to set the
    // app id on Wayland.
    // I know this sounds weird. This is because it is weird. But it's the only way to set the
    // application id on Wayland when using Qt.
    if (platformName == "wayland" && !wmClass.isEmpty())
        QGuiApplication::setDesktopFileName(wmClass);
    else
        QGuiApplication::setDesktopFileName(defaultAppName);

    QCoreApplication::setOrganizationDomain(defaultOrgDomain);
    QCoreApplication::setOrganizationName(defaultOrgName);
    QCoreApplication::setApplicationName(effectiveAppName);
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
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

void ContourGuiApp::newWindow(QScreen* targetScreen)
{
    _pendingSpawnScreen = targetScreen;
    // The engine is always up when one window spawns another (the GUI is running); the guard makes
    // the stage/consume contract exercisable headlessly, where no QML engine exists.
    if (_qmlEngine)
        _qmlEngine->load(resolveResource("ui/main.qml"));
}

QScreen* ContourGuiApp::takePendingSpawnScreen() noexcept
{
    auto* screen = _pendingSpawnScreen.data();
    _pendingSpawnScreen.clear();
    return screen;
}

bool ContourGuiApp::requestRemoteWindow()
{
    return _routingFactory != nullptr && _routingFactory->requestRemoteWindow();
}

void ContourGuiApp::reconcileAttachWindows()
{
    if (!_attachController)
        return;

    // Daemon window ids come ascending, so the primary (lowest-id) window is handled
    // first — it maps to the boot window; the rest each get their own OS window.
    for (auto const daemonWindow: _attachController->windowIds())
    {
        if (auto const mapped = _attachWindowMap.find(daemonWindow); mapped != _attachWindowMap.end())
        {
            // Already shown: bring its tree up to date.
            contour::applyRemoteLayout(_sessionManager, mapped->second, *_attachController, daemonWindow);
            continue;
        }
        if (_pendingAttachWindow == daemonWindow)
            continue; // an OS window is already spawning for this daemon window

        if (_attachWindowMap.empty())
        {
            // The primary daemon window adopts the boot window (already created before
            // the first layout arrived). A missing focused window (mid-boot) just retries
            // on the next layout push.
            if (auto const boot = _sessionManager.focusedWindow())
                bindDaemonWindow(daemonWindow, *boot);
            continue;
        }

        // A new daemon window: spawn an OS window to host it. Its main.qml claims the
        // staged id (consumeAttachWindow → bindPendingAttachWindow), records the mapping
        // and reconciles — so it never creates a stray fresh tab. The QML load is
        // synchronous, so the stage is consumed before this returns.
        _pendingAttachWindow = daemonWindow;
        newWindow();
    }
}

std::optional<std::uint64_t> primaryDaemonWindowToAdopt(bool anyWindowMapped,
                                                        std::vector<std::uint64_t> const& daemonWindowIds)
{
    if (anyWindowMapped || daemonWindowIds.empty())
        return std::nullopt;
    return daemonWindowIds.front(); // windowIds() is ascending, so front() is the primary window
}

bool ContourGuiApp::bindPendingAttachWindow(WindowController* controller)
{
    if (!_attachController || controller == nullptr)
        return false;

    // A reconcile-spawned secondary OS window claims the daemon window staged for it.
    if (_pendingAttachWindow)
    {
        auto const daemonWindow = *_pendingAttachWindow;
        _pendingAttachWindow.reset();
        bindDaemonWindow(daemonWindow, controller->windowId());
        return true;
    }

    // The boot (first) OS window in attach mode ADOPTS the daemon's primary window instead of
    // authoring a fresh tab. Claiming it here makes main.qml skip win.createNewTab(), which in
    // attach mode would author a spurious extra tab on the daemon (and create no local first tab).
    // Bind the primary window now if the daemon already reported one; otherwise still claim the boot
    // window (return true) and let reconcileAttachWindows bind it once the first layout arrives.
    if (_attachWindowMap.empty())
    {
        if (auto const adopt = primaryDaemonWindowToAdopt(false, _attachController->windowIds()))
            bindDaemonWindow(*adopt, controller->windowId());
        return true;
    }

    return false;
}

void ContourGuiApp::bindDaemonWindow(std::uint64_t daemonWindow, vtworkspace::WindowId osWindow)
{
    _attachWindowMap.emplace(daemonWindow, osWindow);
    contour::applyRemoteLayout(_sessionManager, osWindow, *_attachController, daemonWindow);
}

display::ForcedFontDpiProvider* ContourGuiApp::forcedFontDpiProvider()
{
    // Lazy: the provider inspects QGuiApplication::platformName(), so it must not be constructed before
    // the Qt application. First use is a display's setSession(), which is well past that point.
    if (!_forcedFontDpiProvider && QCoreApplication::instance() != nullptr)
        _forcedFontDpiProvider = std::make_unique<display::ForcedFontDpiProvider>();
    return _forcedFontDpiProvider.get();
}

} // namespace contour
