// SPDX-License-Identifier: Apache-2.0
#include <contour/CaptureScreen.h>
#include <contour/CatImageArgs.h>
#include <contour/Config.h>
#include <contour/ContourApp.h>
#include <contour/ShellIntegration.h>

#include <vtbackend/Capabilities.h>
#include <vtbackend/Functions.h>

#include <vtparser/Parser.h>

#include <crispy/App.h>
#include <crispy/CLI.h>
#include <crispy/StackTrace.h>
#include <crispy/base64.h>
#include <crispy/logsink.h>
#include <crispy/utils.h>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <utility>

#include <vthost/Daemon.h>
#include <vthost/ServiceControl.h>
#include <vthost/SocketPath.h>
#include <vthost/Token.h>

#ifndef _WIN32
    #include <sys/ioctl.h>

    #include <unistd.h>
#endif

#ifdef _WIN32
    #include <Windows.h>
#endif

using std::bind;
using std::cerr;
using std::cout;
using std::make_unique;
using std::ofstream;
using std::string;
using std::string_view;
using std::unique_ptr;

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace CLI = crispy::cli;

namespace contour
{

// {{{ helper
namespace
{
#ifdef __linux__
    void crashLogger(std::ostream& out)
    {
        out << "Contour version: " << CONTOUR_VERSION_STRING << "\r\n"
            << "\r\n"
            << "Stack Trace:\r\n"
            << "------------\r\n";

        auto stackTrace = crispy::stack_trace();
        auto symbols = stackTrace.symbols();
        for (auto const& symbol: symbols)
            out << symbol << "\r\n";
    }

    // Have this directory string already pre-created, as in case of a SEGV
    // it may very well be that the memory was corrupted too.
    std::string crashLogDir;

    void segvHandler(int signum)
    {
        signal(signum, SIG_DFL);
        return;

        std::stringstream sstr;
        crashLogger(sstr);
        string const crashLog = sstr.str();

        auto const logFileName = std::format(
            "contour-crash-{:%Y-%m-%d-%H-%M-%S}-pid-{}.log", std::chrono::system_clock::now(), getpid());
        if (chdir(crashLogDir.c_str()) < 0)
            perror("chdir");
        char hostname[80] = { 0 };
        gethostname(hostname, sizeof(hostname));

        cerr << "\r\n"
             << "========================================================================\r\n"
             << "  An internal error caused the terminal to crash ;-( 😭\r\n"
             << "-------------------------------------------------------\r\n"
             << "\r\n"
             << "Please report this to https://github.com/contour-terminal/contour/issues/\r\n"
             << "\r\n"
             << crashLog << "========================================================================\r\n"
             << "\r\n"
             << "Please report the above information and help making this project better.\r\n"
             << "\r\n"
             << "This log will also be written to: \033[1m"
             << "\033]8;;file://" << hostname << "/" << crashLogDir << "/" << logFileName << "\033\\"
             << crashLogDir << "/" << logFileName << "\033]8;;\033\\"
             << "\033[m\r\n"
             << "\r\n";
        cerr.flush();

        ofstream logFile(logFileName);
        logFile << crashLog;

        abort();
    }
#endif
    /// One row per `contour daemon-service` verb: its CLI name, its help text, and what it does.
    ///
    /// The table is the definition — the command list, the handler registration and the dispatch all
    /// read it, so a seventh verb is a row here rather than an edit in three places.
    struct DaemonServiceVerb
    {
        std::string_view name;
        std::string_view helpText;
        contour::DaemonServiceAction action;
    };

    constexpr auto DaemonServiceVerbs = std::array {
        // Only `install` carries the experimental marker: a parent command with children renders
        // THEIR help rather than its own, so marking `daemon-service` itself would say it to
        // nobody -- and install is the verb that opts a machine in, where it is worth saying.
        DaemonServiceVerb { "install",
                            "(experimental) Registers the daemon so it starts on its own (see "
                            "--start).",
                            contour::DaemonServiceAction::Install },
        DaemonServiceVerb { "uninstall",
                            "Stops the daemon and removes its registration.",
                            contour::DaemonServiceAction::Uninstall },
        DaemonServiceVerb { "start",
                            "Starts the registered daemon now, without waiting for its trigger.",
                            contour::DaemonServiceAction::Start },
        DaemonServiceVerb {
            "stop", "Stops the running daemon, leaving it registered.", contour::DaemonServiceAction::Stop },
        DaemonServiceVerb {
            "restart", "Stops it and starts it again.", contour::DaemonServiceAction::Restart },
        DaemonServiceVerb { "status",
                            "Reports whether it is registered and running, and what it runs.",
                            contour::DaemonServiceAction::Status },
    };

    /// The option set each `daemon-service` verb carries.
    ///
    /// Mounted on every verb rather than on their shared parent because crispy's parser resolves
    /// an option against the CURRENT command only — so `daemon-service install --label x` could
    /// not see a `--label` declared one level up, and the natural argument order would be the one
    /// that fails. Built once here so the spellings and help texts still have a single home.
    /// @param full Whether to include the options only `install` needs.
    /// @return The option list.
    [[nodiscard]] crispy::cli::option_list daemonServiceOptions(bool full)
    {
        namespace CLI = crispy::cli;

        auto options = CLI::option_list {
            CLI::option { "label", CLI::value { "default"s }, "Socket label the daemon serves.", "NAME" },
        };
        if (!full)
            return options;

        options.emplace_back(CLI::option { "socket",
                                           CLI::value { ""s },
                                           "Control socket path. Resolved to an absolute path at "
                                           "install time, since a registration outlives this shell.",
                                           "PATH" });
        options.emplace_back(CLI::option { "start",
                                           CLI::value { "logon"s },
                                           "When the daemon starts: `logon` (a Scheduled Task "
                                           "triggered by your logon, running as you, needing no "
                                           "elevation and no password — the default), `boot` or "
                                           "`manual` (an SCM service; not implemented yet, since a "
                                           "service under a named account needs its password).",
                                           "MODE" });
        options.emplace_back(CLI::option { "config",
                                           CLI::value { ""s },
                                           "Configuration file the hosted sessions' terminal "
                                           "settings come from.",
                                           "FILE" });
        options.emplace_back(CLI::option {
            "profile", CLI::value { ""s }, "Config profile those settings come from.", "NAME" });
        options.emplace_back(CLI::option { "size-policy",
                                           CLI::value { "latest"s },
                                           "Which attached client's size the shared grid takes: "
                                           "`latest`, `smallest` or `largest`.",
                                           "POLICY" });
        options.emplace_back(
            CLI::option { "log", CLI::value { ""s }, "Log tag filter for the installed daemon.", "TAGS" });
        options.emplace_back(CLI::option { "log-file",
                                           CLI::value { ""s },
                                           "Where the installed daemon logs. Defaults to SOCKET.log, "
                                           "because a detached daemon has no console to write to.",
                                           "FILE" });
        return options;
    }

    /// Builds the `daemon-service` sub-verbs from @ref DaemonServiceVerbs.
    /// @return One command per verb, each carrying the options it needs.
    [[nodiscard]] crispy::cli::command_list daemonServiceCommands()
    {
        auto commands = crispy::cli::command_list {};
        for (auto const& verb: DaemonServiceVerbs)
            commands.emplace_back(crispy::cli::command {
                .name = verb.name,
                .helpText = verb.helpText,
                .options = daemonServiceOptions(verb.action == contour::DaemonServiceAction::Install) });
        return commands;
    }

    /// Help text for `generate integration`'s `shell` option, naming the shells this binary was
    /// actually built with rather than a list maintained by hand beside them.
    /// @return A view of storage with process lifetime, because crispy::cli::option::helpText
    ///         borrows rather than owns.
    [[nodiscard]] std::string_view integrationShellHelpText()
    {
        static std::string const text = std::format(
            "Shell name to create the integration for. Supported shells: {}", contour::supportedShellsText());
        return text;
    }

} // namespace
// }}}

ContourApp::ContourApp(crispy::environment const& env):
    app(env, "contour", "Contour Terminal Emulator", CONTOUR_VERSION_STRING, "Apache-2.0")
{
    using Project = crispy::cli::about::project;
    crispy::cli::about::registerProjects(
#ifdef CONTOUR_BUILD_WITH_MIMALLOC
        Project { "mimalloc", "", "" },
#endif
        Project { "Qt", "GPL", "https://www.qt.io/" },
        Project { "FreeType", "GPL, FreeType License", "https://freetype.org/" },
        Project { "HarfBuzz", "Old MIT", "https://harfbuzz.github.io/" },
        // Project{"Catch2", "BSL-1.0", "https://github.com/catchorg/Catch2"},
        Project { "libunicode", "Apache-2.0", "https://github.com/contour-terminal/libunicode" },
        Project { "yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp" },
        Project { "termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro" },
        Project { "fmt", "MIT", "https://github.com/fmtlib/fmt" });

#ifdef __linux__
    auto crashLogDirPath = crispy::app::instance()->localStateDir() / "crash";
    crashLogDir = crashLogDirPath.string();
    auto errorCode = std::error_code {};
    std::filesystem::create_directories(crashLogDirPath, errorCode);
    if (errorCode)
        std::cerr << std::format("Warning: Failed to create crash log directory: {} ({})\n",
                                 errorCode.message(),
                                 errorCode.category().name());
    // signal(SIGSEGV, segvHandler);
    signal(SIGABRT, segvHandler);
#endif

#ifdef _WIN32
    // Enable VT output processing on Conhost.
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD savedModes {}; // NOTE: Is it required to restore that upon process exit?
    if (GetConsoleMode(stdoutHandle, &savedModes) != FALSE)
    {
        DWORD modes = savedModes;
        modes |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(stdoutHandle, modes);
    }
#endif

    link("contour.capture", bind(&ContourApp::captureAction, this));
    link("contour.list-debug-tags", bind(&ContourApp::listDebugTagsAction, this));
    link("contour.set.profile", bind(&ContourApp::profileAction, this));
    link("contour.generate.parser-table", bind(&ContourApp::parserTableAction, this));
    link("contour.generate.terminfo", bind(&ContourApp::terminfoAction, this));
    link("contour.generate.config", bind(&ContourApp::configAction, this));
    link("contour.generate.integration", bind(&ContourApp::integrationAction, this));
    link("contour.info.vt", bind(&ContourApp::infoVT, this));
    link("contour.documentation.vt", bind(&ContourApp::documentationVT, this));
    link("contour.documentation.keys", bind(&ContourApp::documentationKeyMapping, this));
    link("contour.documentation.configuration.global", bind(&ContourApp::documentationGlobalConfig, this));
    link("contour.documentation.configuration.profile", bind(&ContourApp::documentationProfileConfig, this));
    link("contour.cat", bind(&ContourApp::catAction, this));
    link("contour.daemon", bind(&ContourApp::daemonAction, this));
    // One handler per verb rather than one that switches: crispy::app dispatches on the first
    // true flag, so the verb IS the dispatch.
    for (auto const& verb: DaemonServiceVerbs)
    {
        // The flag prefix is captured rather than rediscovered: deriving it inside the handler
        // meant indexing the verb table by the enum's value, which silently breaks the day the
        // two fall out of order.
        auto prefix = std::format("contour.daemon-service.{}", verb.name);
        link(prefix, [this, action = verb.action, prefix] { return daemonServiceAction(action, prefix); });
    }
}

template <typename Callback>
static auto withOutput(crispy::cli::flag_store const& flags, std::string const& name, Callback callback)
{
    std::ostream* out = &cout;

    auto const& outputFileName = flags.get<string>(name); // TODO: support string_view
    auto ownedOutput = unique_ptr<std::ostream> {};
    if (outputFileName != "-")
    {
        ownedOutput = make_unique<std::ofstream>(outputFileName);
        out = ownedOutput.get();
    }

    return callback(*out);
}

int ContourApp::documentationVT()
{
    using category = vtbackend::FunctionCategory;
    using namespace std::string_view_literals;

    std::string info;
    auto back = std::back_inserter(info);
    std::format_to(back, "# {}\n", "VT sequences");
    std::format_to(back, "{}\n\n", "List of VT sequences supported by Contour Terminal Emulator.");
    for (auto const& [category, headline]: { std::pair { category::C0, "Control Codes"sv },
                                             std::pair { category::ESC, "Escape Sequences"sv },
                                             std::pair { category::CSI, "Control Sequences"sv },
                                             std::pair { category::OSC, "Operating System Commands"sv },
                                             std::pair { category::DCS, "Device Control Sequences"sv } })
    {

        std::format_to(back, "## {}\n\n", headline);

        std::format_to(back, "| Sequence | Code | Description |\n");
        std::format_to(back, "|----------|------|-------------|\n");
        for (auto const& fn: vtbackend::allFunctions())
        {
            if (fn.category != category)
                continue;

            // This could be much more improved in good looking and informationally.
            // We can also print short/longer description, minimum required VT level,
            // colored output for easier reading, and maybe more.
            std::format_to(back,
                           "| `{:}` | {:} | {:} |\n",
                           crispy::escapeMarkdown(std::format("{}", fn)),
                           fn.documentation.mnemonic,
                           fn.documentation.comment);
        }
        std::format_to(back, "\n");
    }

    std::cout << info;
    return EXIT_SUCCESS;
}

int ContourApp::documentationKeyMapping()
{
    std::string info;
    auto back = std::back_inserter(info);
    std::format_to(back, "{}\n\n", "List of supported actions for key mappings.");

    std::format_to(back, "| Action | Description |\n");
    std::format_to(back, "|--------|-------------|\n");
    for (auto const& entry: contour::actions::actionCatalog())
    {
        // The Action column names the action; the example block below shows the full binding form
        // (which for an argument-carrying action includes its arguments).
        std::format_to(back, "| `{:<20}` | {:} |\n", entry.name, entry.documentation);
    }

    std::format_to(back, "\n");
    std::format_to(back, "Example of entries inside config file\n");
    std::format_to(back, "``` yaml\n");
    for (auto const& entry: contour::actions::actionCatalog())
    {
        std::format_to(back, " - {{ mods: [Control], key: Enter, action: {:} }}\n", entry.prototype);
    }
    std::format_to(back, "```\n");
    std::format_to(back, "\n");

    std::cout << info;
    return EXIT_SUCCESS;
}

int ContourApp::documentationGlobalConfig()
{
    std::string configInfo;
    auto back = std::back_inserter(configInfo);
    std::format_to(back, "{}\n", contour::config::documentationGlobalConfig());

    std::string_view const headerInfo = R"(# Configuring Contour

Contour offers a wide range of configuration options that can be customized, including color scheme, shell, initial working directory, and more.
The configuration options can be categorized into several groups:

- Global options: These settings determine the overall behavior of the terminal and apply to all profiles.<br/>
- Profiles: With profiles, you can configure the terminal more granularly and create multiple profiles that can be easily switched between.<br/>
- Color scheme: Contour allows you to define different color schemes for the terminal and choose which one to use for each of the profiles. <br/>


On Unix systems, the main configuration file is located at  `~/.config/contour/contour.yml` and is both read from and auto-generated there. On Windows systems, the file is located at  `%LocalAppData%\contour\contour.yml`.

!!! note "Please note that on Unix systems, the environment variable `XDG_CONFIG_HOME` (by default set to `~/.config`) is taken into account."

By default, on Unix systems, Contour is executed with the following arguments `contour config ~/.config/contour/contour.yml profile main`. If the configuration file includes a `default_profile` variable, it will be used as the default profile. Otherwise, the first profile listed in the file will be the default one.
## How to

### Load specific configuration file
`contour config /path/to/file/with/configuration.yml`
### Set profile for current session
you can utilize the `profile` parameter with the `contour` command <br/>
`contour profile one_of_profiles`


## Global options

Let's go through the different sections of the global configurations in the file:
)";

    std::string_view const additionalInfo = R"(
### Default global parameters

```yaml
platform_plugin: auto
renderer:
    backend: auto
    tile_hashtable_slots: 4096
    tile_cache_count: 4000
    tile_direct_mapping: true
word_delimiters: " /\\()\"'-.,:;<>~!@#$%^&*+=[]{}~?|│"
read_buffer_size: 16384
pty_buffer_size: 1048576
default_profile: main
spawn_new_process: false
reflow_on_resize: true
bypass_mouse_protocol_modifier: Shift
mouse_block_selection_modifier: Control
on_mouse_select: CopyToSelectionClipboard
live_config: false
images:
    sixel_scrolling: true
    sixel_register_count: 4096

```

The default profile is automatically the top (first) defined profile in the configuration file, but can be explicitly set to an order-independent name using `default_profile` configuration key.


## Profiles
Profiles is the main part of user specific customizations, you can create more than one profile and chose which you want to use during startup or define in configuration file.


By default each profile inherites values from `default_profile`. This means that you can specify only values that you want to change in respect to default profile, for example you can create new profile to use `bash` as a shell preserving other configuration from `main` profile
```
profiles:
    main:
    # default profile here
    bash:
        shell: "/usr/bin/bash"

```

For the full list of options see generated configuration file on your system or [Profiles](profiles.md) section of documentation.


## Color Schemes
In contour you can specify different colors inside terminal, for example text background and foreground, cursor properties, selection colors and plenty others.
You can configure your color profiles, whereas a color can be expressed in standard web format, with a leading # followed by red/green/blue values, 7 characters in total. You may alternatively use 0x as prefix instead of #. For example 0x102030 is equal to '#102030'.

Syntax for color schemes repeat the one of profiles. First color scheme inside configuration file must be named `default`, each other color schemes inherit values from `default` color scheme. Example of configuration for `color_schemes`
```
color_schemes:
    default:
    # values for default color scheme
    different_selection:
      selection:
        background: '#fff0f0'
```

For the full list of options see generated configuration file on your system or [Colors](colors.md) section of documentation.
)";

    std::cout << headerInfo;
    std::cout << configInfo;
    std::cout << additionalInfo;
    return EXIT_SUCCESS;
}

int ContourApp::documentationProfileConfig()
{
    std::string profileInfo;
    auto back = std::back_inserter(profileInfo);
    std::format_to(back, "{}", contour::config::documentationProfileConfig());

    std::cout << profileInfo;
    return EXIT_SUCCESS;
}

int ContourApp::infoVT()
{
    using category = vtbackend::FunctionCategory;
    using std::pair;
    using vtbackend::VTExtension;
    using namespace std::string_view_literals;

    for (auto const& [category, headline]: { pair { category::C0, "Control Codes"sv },
                                             pair { category::ESC, "Escape Sequences"sv },
                                             pair { category::CSI, "Control Sequences"sv },
                                             pair { category::OSC, "Operating System Commands"sv },
                                             pair { category::DCS, "Device Control Sequences"sv } })
    {
        std::cout << std::format("{}\n", headline);
        std::cout << std::format("{}\n\n", string(headline.size(), '='));

        for (auto const& fn: vtbackend::allFunctions())
        {
            if (fn.category != category)
                continue;

            auto const level = fn.extension == VTExtension::None ? std::format("{}", fn.conformanceLevel)
                                                                 : std::format("{}", fn.extension);

            // This could be much more improved in good looking and informationally.
            // We can also print short/longer description, minimum required VT level,
            // colored output for easier reading, and maybe more.
            std::cout << std::format("{:<20} {:<15} {} ({})\n",
                                     fn.documentation.mnemonic,
                                     std::format("{}", fn),
                                     fn.documentation.comment,
                                     level);
        }
        std::cout << std::format("\n");
    }

    return EXIT_SUCCESS;
}

int ContourApp::integrationAction()
{
    return withOutput(parameters(), "contour.generate.integration.to", [&](auto& stream) {
        auto const shell = parameters().get<string>("contour.generate.integration.shell");
        auto const script = shellIntegrationScript(shell);
        if (!script)
        {
            std::cerr << std::format(
                "Cannot generate shell integration for an unsupported shell, {}. Supported shells: {}.\n",
                shell,
                supportedShellsText());
            return EXIT_FAILURE;
        }

        stream.write(script->data(), static_cast<std::streamsize>(script->size()));
        return EXIT_SUCCESS;
    });
}

int ContourApp::configAction()
{
    withOutput(parameters(), "contour.generate.config.to", [](auto& stream) {
        stream << config::defaultConfigString();
    });
    return EXIT_SUCCESS;
}

int ContourApp::terminfoAction()
{
    withOutput(parameters(), "contour.generate.terminfo.to", [](auto& stream) {
        stream << vtbackend::capabilities::StaticDatabase {}.terminfo();
    });
    return EXIT_SUCCESS;
}

int ContourApp::captureAction()
{
    // clang-format off
    auto captureSettings = contour::CaptureSettings {};
    captureSettings.logicalLines = parameters().get<bool>("contour.capture.logical");
    captureSettings.words = parameters().get<bool>("contour.capture.words");
    captureSettings.timeout = parameters().get<double>("contour.capture.timeout");
    captureSettings.lineCount = vtbackend::LineCount::cast_from(parameters().get<unsigned>("contour.capture.lines"));
    captureSettings.outputFile = parameters().get<string>("contour.capture.to");
    // clang-format on

    if (contour::captureScreen(captureSettings))
        return EXIT_SUCCESS;
    else
        return EXIT_FAILURE;
}

namespace
{
    // The pure `contour cat` image-argument parsers live in CatImageArgs.h (unit-tested there); the
    // callers below use parseCatSize/parseCatAlignment/parseCatResize/parseCatLayer directly.

    /// Reads a file in binary mode, returning its raw bytes.
    /// Returns an empty vector on any failure or if the file exceeds @p maxSize bytes.
    std::vector<uint8_t> readFile(std::filesystem::path const& path,
                                  std::uintmax_t maxSize = std::numeric_limits<std::uintmax_t>::max())
    {
        auto ifs = std::ifstream(path.string(), std::ios::binary | std::ios::ate);
        if (!ifs.good())
            return {};

        auto const fileSize = static_cast<std::uintmax_t>(ifs.tellg());
        if (fileSize > maxSize)
            return {};

        ifs.seekg(0, std::ios::beg);
        auto data = std::vector<uint8_t>(static_cast<size_t>(fileSize));
        ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
        if (!ifs || std::cmp_not_equal(ifs.gcount(), fileSize))
            return {};
        return data;
    }

    void displayImage(vtbackend::ImageResize resizePolicy,
                      vtbackend::ImageAlignment alignmentPolicy,
                      crispy::size screenSize,
                      int layer,
                      string_view fileName)
    {
        auto constexpr MaxImageFileSize = static_cast<std::uintmax_t>(16 * 1024 * 1024); // GIP body limit
        auto const data = readFile(std::filesystem::path(string(fileName)), MaxImageFileSize);
        if (data.empty())
        {
            cerr << std::format("Error: Failed to read file '{}' (not found, unreadable, or exceeds 16 MB)\n",
                                fileName);
            return;
        }

        auto constexpr ST = "\033\\"sv;

        cout << std::format("{}o=s,f=1,c={},r={},a={},z={},L={},u,l;!",
                            "\033P!g"sv,
                            screenSize.width,
                            screenSize.height,
                            static_cast<int>(alignmentPolicy) + 1,
                            static_cast<int>(resizePolicy),
                            layer);

        auto encoderState = crispy::base64::encoder_state {};
        std::vector<char> buf;
        auto const writer = [&](char a, char b, char c, char d) {
            buf.push_back(a);
            buf.push_back(b);
            buf.push_back(c);
            buf.push_back(d);
        };
        auto const flush = [&]() {
            cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            buf.clear();
        };

        for (uint8_t const byte: data)
        {
            crispy::base64::encode(byte, encoderState, writer);
            if (buf.size() >= 4096)
                flush();
        }
        crispy::base64::finish(encoderState, writer);
        flush();

        cout << ST;
        cout.flush();
    }

} // namespace

int ContourApp::daemonAction()
{
    // Logging is installed FIRST, so every diagnostic below — socket binds, TLS setup — is
    // captured. The app owns it, so it outlives runDaemon (categories hold a reference into its
    // sink) and every thread runDaemon spawns has been joined by the time it returns.
    if (auto const installed = installLogging("contour.daemon", /*showProcessId=*/true); !installed)
    {
        cerr << std::format("contour daemon: {}\n", installed.error());
        return EXIT_FAILURE;
    }

    auto config = vthost::DaemonConfig {};
    config.socketPath = vthost::muxSocketPath(parameters().get<string>("contour.daemon.label"),
                                              parameters().get<string>("contour.daemon.socket"));

    // The hosted terminals' emulation settings. The profile's `history.limit` is
    // load-bearing here rather than cosmetic -- @see vthost::DefaultSessionHistoryLineCount.
    auto const settings =
        config::resolveEmulationSettings(parameters().get<string>("contour.daemon.config"),
                                         parameters().get<string>("contour.daemon.profile"));
    if (!settings)
    {
        cerr << std::format("contour daemon: {}\n", settings.error());
        return EXIT_FAILURE;
    }
    config.settings = *settings;

    auto const shellCommand = vtpty::Process::loginShell(/*escapeSandbox=*/false);
    config.shell.program = shellCommand.front();
    config.shell.arguments.assign(std::next(shellCommand.begin()), shellCommand.end());
    config.shell.workingDirectory = vtpty::Process::homeDirectory();

    if (auto label = parameters().get<string>("contour.daemon.tmux-compat-socket"); !label.empty())
        config.tmuxCompatLabel = std::move(label);

    if (parameters().get<bool>("contour.daemon.exit-with-last-session"))
        config.lifecycle = vthost::DaemonLifecycle::ExitWhenEmpty;

    // Rejected rather than defaulted: silently serving `latest` to someone who asked for
    // `smallest` would look like the feature does not work.
    auto const sizePolicy = parameters().get<string>("contour.daemon.size-policy");
    if (auto const policy = vthost::clientSizePolicyFrom(sizePolicy))
        config.sizePolicy = *policy;
    else
    {
        cerr << std::format("contour daemon: unknown --size-policy '{}' (expected one of: {})\n",
                            sizePolicy,
                            crispy::joinHumanReadable(vthost::ClientSizePolicyNames | std::views::keys));
        return EXIT_FAILURE;
    }

    // Opt-in TCP listener: absent unless --listen-tcp is given; always TLS-encrypted
    // (self-signed when no cert/key), token-authenticated, loopback-bound unless the
    // host part says otherwise.
    if (auto const listen = parameters().get<string>("contour.daemon.listen-tcp"); !listen.empty())
    {
        auto const hostPort = vthost::parseHostPort(listen);
        if (!hostPort)
        {
            cerr << std::format("contour daemon: invalid --listen-tcp '{}' (expected HOST:PORT)\n", listen);
            return EXIT_FAILURE;
        }

        auto token = vthost::resolveToken(parameters().get<string>("contour.daemon.token"),
                                          parameters().get<string>("contour.daemon.token-file"));
        if (!token)
        {
            cerr << std::format("contour daemon: {}\n", token.error());
            return EXIT_FAILURE;
        }
        // A token is REQUIRED here, not merely supported. On the unix socket the filesystem
        // permissions are the gate; TCP has no such gate, so serving it without a token would
        // hand a full shell to anyone who can reach the port. Refusing to start is the only safe
        // reading of `--listen-tcp` with no credential — a warning would be dismissed once and
        // then run that way forever.
        if (token->empty())
        {
            cerr << "contour daemon: --listen-tcp requires --token or --token-file; the TCP "
                    "transport has no filesystem permissions to fall back on.\n";
            return EXIT_FAILURE;
        }

        config.nativeTcp = vthost::NativeTcpListenerConfig {
            .host = hostPort->first,
            .port = hostPort->second,
            .token = *std::move(token),
            .tlsCertPath = parameters().get<string>("contour.daemon.tls-cert"),
            .tlsKeyPath = parameters().get<string>("contour.daemon.tls-key"),
        };
    }

    // Everything above has validated the configuration, so a typo is reported by THIS process
    // rather than killing a child whose stderr nobody is reading. Only now is it safe to hand
    // the work to a detached copy of ourselves.
    if (parameters().get<bool>("contour.daemon.background"))
        return runDaemonInBackground(config.socketPath);

    return vthost::runDaemon(config);
}

int ContourApp::runDaemonInBackground(std::filesystem::path const& socketPath)
{
    // Our own tokens replayed verbatim — rebuilding the child's command line from parsed flags
    // would silently drop every option someone forgets to add to the rebuilder.
    auto childArgs = commandLine();

    // The flag that brought us here is OVERRIDDEN rather than filtered out of them, because a token
    // filter cannot recognize all of its spellings and gets it wrong in both directions.
    // crispy::cli accepts `--background`, `-background` and the bare `background` (its natural
    // style), each optionally followed by an explicit value in either the `=VALUE` or the
    // separate-token form. A surviving token makes the child spawn ANOTHER detached child, which
    // spawns another — a slow fork bomb, each generation additionally polling for readiness — while
    // dropping only the flag of `--background true` leaves the orphaned `true` for the parser to
    // reject, and the user is told the daemon did not start with no cause given. Options are
    // last-one-wins (@see crispy::cli's setOption, which overwrites), so stating it once more at the
    // end settles it whatever the user wrote.
    childArgs.emplace_back("--background=false"); // the child runs in ITS foreground, or nothing binds

    // A detached process has no console, so its standard error goes nowhere: without a log file
    // the backgrounded daemon becomes the one instance nobody can diagnose. Derived rather than
    // required, so the common case needs no second flag.
    auto logFile = parameters().get<string>("contour.daemon.log-file");
    if (logFile.empty())
    {
        logFile = vthost::daemonLogPath(socketPath).string();
        childArgs.push_back(std::format("--log-file={}", logFile));
    }

    if (vthost::runDaemonDetached(std::move(childArgs), socketPath) != EXIT_SUCCESS)
    {
        cerr << std::format("contour daemon: the background daemon did not start; see {}\n", logFile);
        return EXIT_FAILURE;
    }

    cout << std::format("contour daemon: serving on {} (logging to {})\n", socketPath.string(), logFile);
    return EXIT_SUCCESS;
}

int ContourApp::daemonServiceAction(DaemonServiceAction action, std::string const& prefix)
{
    auto const label = parameters().get<string>(prefix + ".label");

    auto const mode = [&]() -> std::optional<vthost::ServiceStartMode> {
        // Only `install` chooses a backend; every other verb must address whatever is already
        // registered, so it asks each backend in turn rather than guessing.
        if (action != DaemonServiceAction::Install)
            return std::nullopt;
        return vthost::serviceStartModeFrom(parameters().get<string>(prefix + ".start"));
    }();

    if (action == DaemonServiceAction::Install && !mode)
    {
        cerr << std::format("contour daemon-service: unknown --start '{}' (expected one of: {})\n",
                            parameters().get<string>(prefix + ".start"),
                            crispy::joinHumanReadable(vthost::ServiceStartModeNames | std::views::keys));
        return EXIT_FAILURE;
    }

    auto const name = vthost::serviceNameForLabel(label);

    // For a non-install verb the registration decides which backend owns it: `status` on a
    // logon task must not report "not installed" merely because it asked the SCM.
    auto backend = std::unique_ptr<vthost::ServiceBackend> {};
    if (mode)
        backend = vthost::makeServiceBackend(*mode, name);
    else
    {
        for (auto const& [_, candidate]: vthost::ServiceStartModeNames)
        {
            auto probe = vthost::makeServiceBackend(candidate, name);
            if (auto const found = probe->status();
                found && found->state != vthost::ServiceRunState::NotInstalled)
            {
                backend = std::move(probe);
                break;
            }
        }
        if (!backend)
        {
            cerr << std::format("contour daemon-service: no registration named '{}'; run "
                                "'contour daemon-service install' first\n",
                                name);
            return EXIT_FAILURE;
        }
    }

    auto const report = [&](std::expected<void, vthost::ServiceError> const& result, std::string_view verb) {
        if (result)
        {
            cout << std::format("contour daemon-service: {} {}\n", name, verb);
            return EXIT_SUCCESS;
        }
        cerr << std::format(
            "contour daemon-service: could not {} {}: {}\n", verb, name, result.error().toString());
        return EXIT_FAILURE;
    };

    switch (action)
    {
        case DaemonServiceAction::Install: {
            auto commandLineOrError = daemonServiceCommandLine(prefix, label);
            if (!commandLineOrError)
            {
                cerr << std::format("contour daemon-service: {}\n", commandLineOrError.error());
                return EXIT_FAILURE;
            }
            auto request = vthost::ServiceInstallRequest {
                .commandLine = *std::move(commandLineOrError),
                .mode = *mode,
                .displayName = std::format("Contour daemon ({})", label),
                .description = "Hosts Contour terminal sessions so they outlive any window "
                               "showing them.",
            };
            if (auto const installed = backend->install(request); !installed)
            {
                cerr << std::format(
                    "contour daemon-service: could not install {}: {}\n", name, installed.error().toString());
                return EXIT_FAILURE;
            }
            cout << std::format(
                "contour daemon-service: {} installed, starting at {}\n", name, vthost::nameOf(*mode));
            return EXIT_SUCCESS;
        }
        case DaemonServiceAction::Uninstall: return report(backend->uninstall(), "uninstalled");
        case DaemonServiceAction::Start: return report(backend->start(), "started");
        case DaemonServiceAction::Stop: return report(backend->stop(), "stopped");
        case DaemonServiceAction::Restart:
            // A stop that finds nothing running is not a failure here: restart's contract is
            // "be running afterwards", which a stopped registration already satisfies halfway.
            std::ignore = backend->stop();
            return report(backend->start(), "restarted");
        case DaemonServiceAction::Status: {
            auto const status = backend->status();
            if (!status)
            {
                cerr << std::format(
                    "contour daemon-service: could not query {}: {}\n", name, status.error().toString());
                return EXIT_FAILURE;
            }
            cout << std::format(
                "{}: {} (starts at {})\n", name, vthost::nameOf(status->state), vthost::nameOf(status->mode));
            if (!status->commandLine.empty())
                cout << std::format("  runs: {}\n", status->commandLine);
            return EXIT_SUCCESS;
        }
    }
    return EXIT_FAILURE;
}

std::expected<std::vector<std::string>, std::string> ContourApp::daemonServiceCommandLine(
    std::string const& prefix, std::string const& label) const
{
    // Absolute, and resolved NOW: a registration outlives the shell that created it, and a
    // service's %TEMP%/%USERNAME% need not match the interactive user's — so a path left to be
    // derived at start time would resolve somewhere the user's own client never looks.
    auto const socketPath = vthost::muxSocketPath(label, parameters().get<string>(prefix + ".socket"));

    auto const sizePolicyText = parameters().get<string>(prefix + ".size-policy");
    auto const sizePolicy = vthost::clientSizePolicyFrom(sizePolicyText);
    if (!sizePolicy)
    {
        // Caught here rather than by the installed daemon: a rejected value would otherwise kill
        // a process started by the OS, whose stderr nobody is watching.
        return std::unexpected(
            std::format("unknown --size-policy '{}' (expected one of: {})",
                        sizePolicyText,
                        crispy::joinHumanReadable(vthost::ClientSizePolicyNames | std::views::keys)));
    }

    auto logFile = parameters().get<string>(prefix + ".log-file");
    if (logFile.empty())
        logFile = vthost::daemonLogPath(socketPath).string();

    // Through daemonSpawnArgs, which is already the ONE spelling of this flag list — the client's
    // auto-spawn builds its command line the same way, so a daemon option cannot end up forwarded
    // by one path and silently dropped by the other. Persistent, not ExitWhenEmpty: an installed
    // daemon exists precisely to still be there for the next client.
    return vthost::daemonSpawnArgs(std::filesystem::absolute(commandLine().front()).string(),
                                   socketPath,
                                   vthost::DaemonSpawnOptions {
                                       .filter = parameters().get<string>(prefix + ".log"),
                                       .logFile = std::move(logFile),
                                       .configPath = parameters().get<string>(prefix + ".config"),
                                       .profileName = parameters().get<string>(prefix + ".profile"),
                                       .sizePolicy = *sizePolicy,
                                   },
                                   vthost::DaemonLifecycle::Persistent);
}

int ContourApp::catAction()
{
    if (parameters().verbatim.empty())
    {
        cerr << "Error: No image file specified.\n";
        return EXIT_FAILURE;
    }

    auto const resizePolicy = parseCatResize(parameters().get<string>("contour.cat.resize"));
    auto const alignmentPolicy = parseCatAlignment(parameters().get<string>("contour.cat.align"));
    auto const size = parseCatSize(parameters().get<string>("contour.cat.size"));
    auto const layer = parseCatLayer(parameters().get<string>("contour.cat.layer"));
    auto const fileName = parameters().verbatim.front();

    displayImage(resizePolicy, alignmentPolicy, size, layer, fileName);
    return EXIT_SUCCESS;
}

int ContourApp::parserTableAction()
{
    vtparser::parserTableDot(std::cout);
    return EXIT_SUCCESS;
}

int ContourApp::listDebugTagsAction()
{
    listDebugTags();
    return EXIT_SUCCESS;
}

int ContourApp::profileAction()
{
    auto const profileName = parameters().get<string>("contour.set.profile.to");
    // TODO: guard `profileName` value against invalid input.
    cout << std::format("\033P$p{}\033\\", profileName);
    return EXIT_SUCCESS;
}

crispy::cli::command ContourApp::parameterDefinition() const
{
    // NOLINTBEGIN
    return CLI::command {
        "contour",
        "Contour Terminal Emulator " CONTOUR_VERSION_STRING
        " - https://github.com/contour-terminal/contour/ ;-)",
        CLI::option_list {},
        CLI::command_list {
            CLI::command { "help", "Shows this help and exits." },
            CLI::command { "version", "Shows the version and exits." },
            CLI::command { "license",
                           "Shows the license, and project URL of the used projects and Contour." },
            CLI::command { "list-debug-tags", "Lists all available debug tags and exits." },
            CLI::command {
                "info",
                "General informational outputs.",
                CLI::option_list {},
                CLI::command_list {
                    CLI::command { "vt", "Prints general information about supported VT sequences." },
                    CLI::command { "config", "Prints missing entries from user config file." },
                } },
            CLI::command {
                "documentation",
                "Generate documentation for web page",
                CLI::option_list {},
                CLI::command_list {
                    CLI::command { "vt", "VT sequence reference documentation" },
                    CLI::command { "keys", "List of configurable actions for key binding" },
                    CLI::command {
                        "configuration",
                        "Create documentation for configuration file",
                        CLI::option_list {},
                        CLI::command_list {
                            CLI::command { "global",
                                           "Create documentation entry for global part of the config file" },
                            CLI::command { "profile",
                                           "Create documentation entry for profile part of the config file" },
                        } },
                },
            },
            CLI::command {
                "generate",
                "Generation utilities.",
                CLI::option_list {},
                CLI::command_list {
                    CLI::command { "parser-table",
                                   "Dumps VT parser's state machine in dot-file format to stdout." },
                    CLI::command {
                        "terminfo",
                        "Generates the terminfo source file that will reflect the features of "
                        "this version "
                        "of contour. Using - as value will write to stdout instead.",
                        {
                            CLI::option { "to",
                                          CLI::value { ""s },
                                          "Output file name to store the screen capture to. If - (dash) is "
                                          "given, the output will be written to standard output.",
                                          "FILE",
                                          CLI::presence::Required },
                        } },
                    CLI::command {
                        "config",
                        "Generates configuration file with the default configuration.",
                        CLI::option_list {
                            CLI::option { "to",
                                          CLI::value { ""s },
                                          "Output file name to store the config file to. If - (dash) is "
                                          "given, the output will be written to standard output.",
                                          "FILE",
                                          CLI::presence::Required },
                        } },
                    CLI::command {
                        "integration",
                        "Generates shell integration script.",
                        CLI::option_list {
                            CLI::option { "shell",
                                          CLI::value { ""s },
                                          integrationShellHelpText(),
                                          "SHELL",
                                          CLI::presence::Required },
                            CLI::option { "to",
                                          CLI::value { ""s },
                                          "Output file name to store the shell integration file to. If - "
                                          "(dash) is given, the output will be written to standard output.",
                                          "FILE",
                                          CLI::presence::Required },
                        } } } },
            CLI::command {
                "cat",
                "Displays an image in the terminal using the Good Image Protocol.",
                CLI::option_list {
                    CLI::option { "resize",
                                  CLI::value { "fit"s },
                                  "Sets the image resize policy.\n"
                                  "Policies available are:\n"
                                  " - no/none (no resize),\n"
                                  " - fit (resize to fit),\n"
                                  " - fill (resize to fill),\n"
                                  " - stretch (stretch to fill)." },
                    CLI::option { "align",
                                  CLI::value { "center"s },
                                  "Sets the image alignment policy.\n"
                                  "Possible values: top-start, top-center, top-end, "
                                  "middle-start, middle-center/center, middle-end, "
                                  "bottom-start, bottom-center, bottom-end." },
                    CLI::option { "size",
                                  CLI::value { "0x0"s },
                                  "Sets the amount of columns and rows to place the image onto "
                                  "(format: COLSxROWS, e.g. 80x24). "
                                  "The top-left of the area is the current cursor position." },
                    CLI::option { "layer",
                                  CLI::value { "1"s },
                                  "Sets the image layer relative to text.\n"
                                  "Values: 0/below (below text), 1/replace (replace text, default), "
                                  "2/above (above text)." } },
                CLI::command_list {},
                CLI::command_select::Explicit,
                CLI::verbatim {
                    "IMAGE_FILE",
                    "Path to image to be displayed. Image formats supported are at least PNG, JPG." } },
            CLI::command { "daemon-service",
                           "Installs, removes and controls an OS-managed Contour daemon that starts on its "
                           "own. A sibling verb rather than `daemon service` because options bind to the "
                           "command they follow, and the daemon verb already owns its own set.",
                           CLI::option_list {},
                           daemonServiceCommands() },
            CLI::command {
                "daemon",
                "(experimental) Runs the headless terminal multiplexer daemon, serving sessions "
                "to attaching clients over a control socket. Its options and wire protocol may "
                "still change between releases.",
                CLI::option_list {
                    CLI::option { "socket",
                                  CLI::value { ""s },
                                  "Path of the control socket file. Defaults to "
                                  "$XDG_RUNTIME_DIR/contour/LABEL (respecting $CONTOUR_MUX).",
                                  "PATH" },
                    CLI::option { "label",
                                  CLI::value { "default"s },
                                  "Socket label distinguishing daemon instances.",
                                  "NAME" },
                    CLI::option { "config",
                                  CLI::value { ""s },
                                  "Path to the configuration file whose profile supplies the "
                                  "hosted sessions' terminal settings (history depth, terminal "
                                  "id, reflow, image limits).",
                                  "FILE" },
                    CLI::option { "profile",
                                  CLI::value { ""s },
                                  "Config profile the hosted sessions' terminal settings come "
                                  "from. Defaults to the configuration's default profile.",
                                  "NAME" },
                    CLI::option { "background",
                                  CLI::value { false },
                                  "Relaunches this command detached and returns once the daemon "
                                  "is accepting connections, instead of holding the terminal. "
                                  "Without --log-file the detached daemon logs beside its socket "
                                  "(SOCKET.log), since a detached process has no console to write "
                                  "its diagnostics to." },
                    CLI::option { "exit-with-last-session",
                                  CLI::value { false },
                                  "Terminates the daemon once its last hosted session is gone "
                                  "instead of waiting for further clients (tmux spells this "
                                  "`exit-empty`). Off by default, so a daemon started by hand "
                                  "keeps serving with no sessions; `contour client` passes this "
                                  "to a daemon it auto-spawns, which belongs to that client." },
                    CLI::option { "size-policy",
                                  CLI::value { "latest"s },
                                  "Which attached client's size the shared grid takes when several "
                                  "clients of different sizes are attached: `latest` (the client "
                                  "that resized most recently, the default), `smallest` (the "
                                  "largest grid every client can fully display) or `largest` (the "
                                  "union, which smaller clients pan). Mirrors tmux's `window-size`.",
                                  "POLICY" },
                    CLI::option { "tmux-compat-socket",
                                  CLI::value { ""s },
                                  "Additionally binds tmux's own discovery path "
                                  "/tmp/tmux-<uid>/<LABEL> so a plain `tmux -L LABEL -C "
                                  "attach-session` finds this daemon.",
                                  "LABEL" },
                    CLI::option { "listen-tcp",
                                  CLI::value { ""s },
                                  "Also serve the native protocol over TCP at HOST:PORT "
                                  "(opt-in; loopback e.g. 127.0.0.1:9090 by default). Always "
                                  "TLS-encrypted and token-authenticated.",
                                  "HOST:PORT" },
                    CLI::option { "token",
                                  CLI::value { ""s },
                                  "Preshared token every TCP client must present; required with "
                                  "--listen-tcp, which has no filesystem gate to fall back on. "
                                  "Note that a token given here is visible to other local users "
                                  "via the process list; prefer --token-file.",
                                  "TOKEN" },
                    CLI::option { "token-file",
                                  CLI::value { ""s },
                                  "Reads the preshared token from FILE instead of the command "
                                  "line, so the secret is protected by that file's permissions "
                                  "rather than exposed in the process list. Trailing newlines "
                                  "are ignored.",
                                  "FILE" },
                    CLI::option { "tls-cert",
                                  CLI::value { ""s },
                                  "PEM certificate for the TCP listener. When omitted (with "
                                  "--tls-key) the daemon generates an ephemeral self-signed "
                                  "certificate (TOFU).",
                                  "FILE" },
                    CLI::option {
                        "tls-key", CLI::value { ""s }, "PEM private key matching --tls-cert.", "FILE" },
                    CLI::option { "log",
                                  CLI::value { ""s },
                                  "Enables logging for a comma (,) separated list of tags, or "
                                  "`all` (see `contour list-debug-tags`). Note the trailing `*` "
                                  "is a prefix match, so `vthost.*` includes the verbose "
                                  "`vthost.trace.*` tiers. Overrides $LOG.",
                                  "TAGS" },
                    CLI::option { "log-file",
                                  CLI::value { ""s },
                                  "Appends log output to FILE instead of standard error. If - "
                                  "(dash) is given, standard error is used explicitly.",
                                  "FILE" },
                } },
            CLI::command {
                "capture",
                "Captures the screen buffer of the currently running terminal.",
                {
                    CLI::option { "logical",
                                  CLI::value { false },
                                  "Tells the terminal to use logical lines for counting and capturing." },
                    CLI::option { "words",
                                  CLI::value { false },
                                  "Splits each line into words and outputs only one word per line." },
                    CLI::option { "timeout",
                                  CLI::value { 1.0 },
                                  "Sets timeout seconds to wait for terminal to respond.",
                                  "SECONDS" },
                    CLI::option { "lines", CLI::value { 0u }, "The number of lines to capture", "COUNT" },
                    CLI::option { "to",
                                  CLI::value { ""s },
                                  "Output file name to store the screen capture to. If - (dash) is given, "
                                  "the capture will be written to standard output.",
                                  "FILE",
                                  CLI::presence::Required },
                } },
            CLI::command {
                "set",
                "Sets various aspects of the connected terminal.",
                CLI::option_list {},
                CLI::command_list {
                    CLI::command { "profile",
                                   "Changes the terminal profile of the currently attached terminal to the "
                                   "given value.",
                                   CLI::option_list { CLI::option {
                                       "to",
                                       CLI::value { ""s },
                                       "Profile name to activate in the currently connected terminal.",
                                       "NAME" } } } } } }
    };
    // NOLINTEND
}

} // namespace contour
