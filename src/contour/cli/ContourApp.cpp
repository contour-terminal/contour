// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentSkill.hpp>
#include <contour/cli/CaptureScreen.hpp>
#include <contour/cli/CatImageArgs.hpp>
#include <contour/cli/ConfigProfileCatalog.hpp>
#include <contour/cli/ConfigReloadPoller.hpp>
#include <contour/cli/ContourApp.hpp>
#include <contour/cli/ControlVerbs.hpp>
#include <contour/cli/DaemonVerbs.hpp>
#include <contour/cli/Hosting.hpp>
#include <contour/cli/ShellIntegration.hpp>
#include <contour/config/Config.hpp>

#include <vtbackend/screen/LocalTime.hpp>
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/vt/Capabilities.hpp>
#include <vtbackend/vt/Functions.hpp>

#include <vtparser/Parser.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/ProgramLookup.hpp>

#include <crispy/StackTrace.hpp>

#include <core/Base64.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/async/StopToken.hpp>
#include <core/cli/App.hpp>
#include <core/cli/CLI.hpp>
#include <core/log/LogSink.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef CONTOUR_EXTENSIONS
    #include <contour/config/ExtensionDeclarations.hpp>

    #include <vtpty/CommandCapture.hpp>

    #include <vtextension/BundledExtensions.hpp>
    #include <vtextension/ExtensionGrantStore.hpp>
    #include <vtextension/ExtensionWorkerPool.hpp>
    #include <vtextension/wasm/DefaultBackend.hpp>
    #include <vtextension/wasm/WasmRuntime.hpp>
    #include <vthost/DaemonExtensions.hpp>
#endif
#include <vthost/AccessFile.hpp>
#include <vthost/CommandRunner.hpp>
#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonImage.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/FilePrivacy.hpp>
#include <vthost/PlatformStreams.hpp>
#include <vthost/ServiceControl.hpp>
#include <vthost/SessionSnapshotStore.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/StatePath.hpp>
#include <vthost/StdioAttach.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/Token.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>

#ifndef _WIN32
    #include <sys/ioctl.h>

    #include <unistd.h>
#endif

#ifdef _WIN32
    #include <Windows.h>
    #include <io.h>
#endif

using std::bind;
using std::cerr;
using std::cout;
using std::ofstream;
using std::string;
using std::string_view;

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace CLI = core::cli;

namespace contour::cli
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

        auto stackTrace = crispy::StackTrace();
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
        contour::cli::DaemonServiceAction action;
    };

    constexpr auto DaemonServiceVerbs = std::array {
        // Only `install` carries the experimental marker: a parent command with children renders
        // THEIR help rather than its own, so marking `daemon-service` itself would say it to
        // nobody -- and install is the verb that opts a machine in, where it is worth saying.
        DaemonServiceVerb { "install",
                            "(experimental) Registers the daemon so it starts on its own (see "
                            "--start).",
                            contour::cli::DaemonServiceAction::Install },
        DaemonServiceVerb { "uninstall",
                            "Stops the daemon and removes its registration.",
                            contour::cli::DaemonServiceAction::Uninstall },
        DaemonServiceVerb { "start",
                            "Starts the registered daemon now, without waiting for its trigger.",
                            contour::cli::DaemonServiceAction::Start },
        DaemonServiceVerb { "stop",
                            "Stops the running daemon, leaving it registered.",
                            contour::cli::DaemonServiceAction::Stop },
        DaemonServiceVerb {
            "restart", "Stops it and starts it again.", contour::cli::DaemonServiceAction::Restart },
        DaemonServiceVerb { "status",
                            "Reports whether it is registered and running, and what it runs.",
                            contour::cli::DaemonServiceAction::Status },
    };

    /// The option set each `daemon-service` verb carries.
    ///
    /// Mounted on every verb rather than on their shared parent because crispy's parser resolves
    /// an option against the CURRENT command only — so `daemon-service install --label x` could
    /// not see a `--label` declared one level up, and the natural argument order would be the one
    /// that fails. Built once here so the spellings and help texts still have a single home.
    /// @param full Whether to include the options only `install` needs.
    /// @return The option list.
    [[nodiscard]] core::cli::OptionList daemonServiceOptions(bool full)
    {
        namespace CLI = core::cli;

        auto options = CLI::OptionList {
            CLI::Option { "label",
                          CLI::Value { std::string { vthost::DefaultSocketLabel } },
                          "Socket label the daemon serves.",
                          "NAME" },
        };
        if (!full)
            return options;

        options.emplace_back(CLI::Option { "socket",
                                           CLI::Value { ""s },
                                           "Control socket path. Resolved to an absolute path at "
                                           "install time, since a registration outlives this shell.",
                                           "PATH" });
        options.emplace_back(CLI::Option { "start",
                                           CLI::Value { "logon"s },
                                           "When the daemon starts: `logon` (with your session, "
                                           "running as you and needing no elevation: a systemd user "
                                           "unit, a launchd LaunchAgent, or a Windows Scheduled Task "
                                           "— the default), `boot` or `manual` (Windows only, an SCM "
                                           "service; not implemented yet, since a service under a "
                                           "named account needs its password).",
                                           "MODE" });
        options.emplace_back(CLI::Option { "config",
                                           CLI::Value { ""s },
                                           "Configuration file the hosted sessions' terminal "
                                           "settings come from.",
                                           "FILE" });
        options.emplace_back(CLI::Option {
            "profile", CLI::Value { ""s }, "Config profile those settings come from.", "NAME" });
        options.emplace_back(CLI::Option { "size-policy",
                                           CLI::Value { ""s },
                                           "Which attached client's size the shared grid takes: "
                                           "`latest`, `smallest` or `largest`. Defaults to the "
                                           "configuration's `daemon.size_policy`.",
                                           "POLICY" });
        options.emplace_back(
            CLI::Option { "log", CLI::Value { ""s }, "Log tag filter for the installed daemon.", "TAGS" });
        options.emplace_back(CLI::Option { "log-file",
                                           CLI::Value { ""s },
                                           "Where the installed daemon logs. Defaults to SOCKET.log, "
                                           "because a detached daemon has no console to write to.",
                                           "FILE" });
        return options;
    }

    /// Builds the `daemon-service` sub-verbs from @ref DaemonServiceVerbs.
    /// @return One command per verb, each carrying the options it needs.
    [[nodiscard]] core::cli::CommandList daemonServiceCommands()
    {
        auto commands = core::cli::CommandList {};
        for (auto const& verb: DaemonServiceVerbs)
            commands.emplace_back(core::cli::Command {
                .name = verb.name,
                .helpText = verb.helpText,
                .options = daemonServiceOptions(verb.action == contour::cli::DaemonServiceAction::Install) });
        return commands;
    }

    /// The option set each `daemon` sub-verb carries, mounted per verb for the reason
    /// @ref daemonServiceOptions gives.
    /// @param verb Which sub-verb.
    /// @return The option list.
    [[nodiscard]] core::cli::OptionList daemonVerbOptions(contour::cli::DaemonVerb verb)
    {
        namespace CLI = core::cli;
        using contour::cli::DaemonVerb;

        auto options = CLI::OptionList {
            CLI::Option { "label",
                          CLI::Value { std::string { vthost::DefaultSocketLabel } },
                          "Socket label distinguishing daemon instances.",
                          "NAME" },
            CLI::Option { "socket",
                          CLI::Value { ""s },
                          "Path of the control socket file. Defaults to "
                          "$XDG_RUNTIME_DIR/contour/LABEL (respecting $CONTOUR_MUX).",
                          "PATH" },
        };
        switch (verb)
        {
            case DaemonVerb::Stop:
                options.emplace_back(CLI::Option {
                    "yes", CLI::Value { false }, "Ends the daemon's sessions without asking first." });
                options.emplace_back(CLI::Option { "keep-snapshot",
                                                   CLI::Value { false },
                                                   "Keeps the daemon's workspace, so that the next daemon "
                                                   "restores its windows, tabs and panes." });
                break;
            case DaemonVerb::Logs:
                options.emplace_back(CLI::Option {
                    "follow", CLI::Value { false }, "Keeps printing as the log grows (end with Ctrl+C)." });
                options.emplace_back(CLI::Option { "lines",
                                                   CLI::Value { 100u },
                                                   "How many trailing lines to print first; 0 prints all "
                                                   "of the last 256 KiB.",
                                                   "N" });
                break;
            case DaemonVerb::Run:
            case DaemonVerb::Status: break;
        }
        return options;
    }

    /// Builds the `daemon` sub-verbs from @ref contour::cli::DaemonSubVerbs.
    /// @return One command per verb, each carrying the options it needs.
    [[nodiscard]] core::cli::CommandList daemonVerbCommands()
    {
        auto commands = core::cli::CommandList {};
        for (auto const& row: contour::cli::DaemonSubVerbs)
            commands.emplace_back(core::cli::Command {
                .name = row.name, .helpText = row.helpText, .options = daemonVerbOptions(row.verb) });
        return commands;
    }

    /// Help text for `generate integration`'s `shell` option, naming the shells this binary was
    /// actually built with rather than a list maintained by hand beside them.
    /// @return A view of storage with process lifetime, because core::cli::Option::helpText
    ///         borrows rather than owns.
    [[nodiscard]] std::string_view integrationShellHelpText()
    {
        static std::string const text =
            std::format("Shell name to create the integration for. Supported shells: {}. Integrating "
                        "natively, needing none: {}.",
                        contour::cli::supportedShellsText(),
                        contour::cli::nativeShellsText());
        return text;
    }

    /// Whether SIGINT arrived since the last InterruptStop was made; set from the signal handler.
    std::atomic<bool> interruptPending { false };

    void onInterrupt(int signal)
    {
        interruptPending.store(true);
        // The first Ctrl+C asks the stream to end; should that stall, the second ends the process, as it
        // would any other. Re-registering the signal being handled is allowed in a handler.
        std::signal(signal, SIG_DFL);
    }

    /// Turns the first Ctrl+C into a stop request for as long as it lives, so a verb that follows a stream
    /// can end it cleanly instead of being killed mid-line; a second one kills it.
    ///
    /// std::signal rather than core::platform::SignalHandler, which covers SIGCHLD, SIGTSTP and SIGCONT
    /// only.
    class InterruptStop
    {
      public:
        InterruptStop():
            _previous { arm() }, _watcher { [this](std::stop_token const& done) {
                while (!done.stop_requested() && !interruptPending.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds { 25 });
                if (interruptPending.load())
                    _source.request_stop();
            } }
        {
        }

        InterruptStop(InterruptStop const&) = delete;
        InterruptStop(InterruptStop&&) = delete;
        InterruptStop& operator=(InterruptStop const&) = delete;
        InterruptStop& operator=(InterruptStop&&) = delete;

        ~InterruptStop()
        {
            _watcher.request_stop();
            _watcher.join();
            std::signal(SIGINT, _previous);
        }

        /// @return A token that is stopped once Ctrl+C was pressed.
        [[nodiscard]] core::async::StopToken token() const { return _source.get_token(); }

      private:
        /// @return The handler SIGINT had before.
        [[nodiscard]] static void (*arm())(int)
        {
            interruptPending.store(false);
            return std::signal(SIGINT, &onInterrupt);
        }

        core::async::StopSource _source {};
        void (*_previous)(int);
        std::jthread _watcher;
    };

    /// @return The `agent` command, whose `setup` writes the skill that teaches coding agents Contour's
    /// verbs.
    [[nodiscard]] core::cli::Command agentCommand()
    {
        namespace CLI = core::cli;
        return CLI::Command {
            .name = "agent",
            .helpText = "Sets up coding agents to operate Contour.",
            .options = {},
            .children = CLI::CommandList { CLI::Command {
                .name = "setup",
                .helpText = "Writes the Contour skill into the agents' skill directories "
                            "(~/.claude/skills/contour, ~/.agents/skills/contour); rerun it after upgrading.",
                .options =
                    CLI::OptionList {
                        CLI::Option {
                            "agent",
                            CLI::Value { ""s },
                            "Sets up only these agents, a comma-separated list of claude and agents; "
                            "all by default.",
                            "NAME" },
                        CLI::Option { "dry-run", CLI::Value { false }, "Reports what would be written." },
                        CLI::Option { "force",
                                      CLI::Value { false },
                                      "Replaces a skill file that Contour did not generate." },
                        CLI::Option { "json", CLI::Value { false }, "Prints the outcomes as JSON." },
                    } } },
        };
    }

    /// Builds the commands of the control verbs from @ref contour::cli::controlVerbs, nesting the dotted
    /// paths (`block.list`) under their groups.
    /// @return The commands, to be added at the top level.
    [[nodiscard]] core::cli::CommandList controlVerbCommands()
    {
        auto commands = core::cli::CommandList {};
        for (auto const& verb: contour::cli::controlVerbs())
        {
            auto const dot = verb.path.find('.');
            auto command =
                core::cli::Command { .name = verb.path.substr(dot == string_view::npos ? 0 : dot + 1),
                                     .helpText = verb.summary,
                                     .options = verb.options(),
                                     .children = {},
                                     .select = core::cli::CommandSelect::Explicit,
                                     .verbatim = verb.verbatim };
            if (dot == string_view::npos)
            {
                commands.emplace_back(std::move(command));
                continue;
            }
            auto const groupName = verb.path.substr(0, dot);
            auto group = std::ranges::find(commands, groupName, &core::cli::Command::name);
            if (group == commands.end())
            {
                auto const groups = contour::cli::controlVerbGroups();
                auto const row = std::ranges::find(groups, groupName, &contour::cli::ControlVerbGroup::name);
                commands.emplace_back(core::cli::Command {
                    .name = groupName, .helpText = row != groups.end() ? row->helpText : string_view {} });
                group = std::prev(commands.end());
            }
            group->children.emplace_back(std::move(command));
        }
        return commands;
    }

} // namespace
// }}}

ContourApp::ContourApp(core::Environment const& env):
    App(env, "contour", "Contour Terminal Emulator", CONTOUR_VERSION_STRING, "Apache-2.0")
{
    using Project = core::cli::about::Project;
    core::cli::about::registerProjects(
#ifdef CONTOUR_BUILD_WITH_MIMALLOC
        Project { "mimalloc", "", "" },
#endif
        Project { "Qt", "GPL", "https://www.qt.io/" },
        Project { "FreeType", "GPL, FreeType License", "https://freetype.org/" },
        Project { "HarfBuzz", "Old MIT", "https://harfbuzz.github.io/" },
        // Project{"Catch2", "BSL-1.0", "https://github.com/catchorg/Catch2"},
        Project { "libunicode", "Apache-2.0", "https://github.com/contour-terminal/libunicode" },
        Project { "yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp" },
        Project { "termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro" });

#ifdef __linux__
    auto crashLogDirPath = core::cli::App::instance()->localStateDir() / "crash";
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
    link("contour.agent.setup", bind(&ContourApp::agentSetupAction, this));
    for (auto const& verb: contour::cli::controlVerbs())
        link(std::format("contour.{}", verb.path), [this, &verb] { return controlVerbAction(verb); });
    // One handler per verb rather than one that switches: core::cli::App dispatches on the first
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

namespace
{
    /// Why a `generate ... to FILE` verb could not write its output.
    enum class OutputError : uint8_t
    {
        CannotOpen,  ///< The file could not be created: no such directory, no permission, a bad name.
        CannotWrite, ///< It was opened, and then not all of the output reached it.
    };

    /// @return What a user is told when @p error kept the output from @p fileName.
    [[nodiscard]] std::string describe(OutputError error, std::string_view fileName)
    {
        switch (error)
        {
            case OutputError::CannotOpen: return std::format("Cannot open {} for writing.\n", fileName);
            case OutputError::CannotWrite: return std::format("Cannot write all of {}.\n", fileName);
        }
        return {};
    }

    /// Writes @p text to @p fileName -- standard output for "-" -- in binary, so that what lands in a file
    /// is @p text byte for byte, without a CR before each LF on Windows.
    /// @param fileName The file, in UTF-8, or "-".
    /// @param text     What to write.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] std::expected<void, OutputError> writeOutput(std::string const& fileName,
                                                               std::string_view text)
    {
        if (fileName == "-")
        {
            cout.write(text.data(), static_cast<std::streamsize>(text.size()));
            cout.flush();
            return cout ? std::expected<void, OutputError> {} : std::unexpected(OutputError::CannotWrite);
        }

        auto out = std::ofstream { core::pathFromUtf8(fileName).value_or(std::filesystem::path {}),
                                   std::ios::binary };
        if (!out)
            return std::unexpected(OutputError::CannotOpen);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out)
            return std::unexpected(OutputError::CannotWrite);
        return {};
    }

    /// Writes @p text where the flag @p name of @p flags says, and says on standard error when it could not.
    /// @return The verb's exit code.
    [[nodiscard]] int writeOutput(core::cli::FlagStore const& flags,
                                  std::string const& name,
                                  std::string_view text)
    {
        auto const& fileName = flags.get<string>(name);
        auto const written = writeOutput(fileName, text);
        if (written)
            return EXIT_SUCCESS;
        cerr << describe(written.error(), fileName);
        return EXIT_FAILURE;
    }
} // namespace

int ContourApp::documentationVT()
{
    using Category = vtbackend::FunctionCategory;
    using namespace std::string_view_literals;

    std::string info;
    auto back = std::back_inserter(info);
    std::format_to(back, "# {}\n", "VT sequences");
    std::format_to(back, "{}\n\n", "List of VT sequences supported by Contour Terminal Emulator.");
    for (auto const& [category, headline]: { std::pair { Category::C0, "Control Codes"sv },
                                             std::pair { Category::ESC, "Escape Sequences"sv },
                                             std::pair { Category::CSI, "Control Sequences"sv },
                                             std::pair { Category::OSC, "Operating System Commands"sv },
                                             std::pair { Category::DCS, "Device Control Sequences"sv } })
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
                           core::escapeMarkdown(std::format("{}", fn)),
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

    // A format string, for the defaults that have a name to take them from; its braces are doubled.
    constexpr std::string_view AdditionalInfo = R"(
### Default global parameters

```yaml
platform_plugin: auto
renderer:
    backend: auto
    tile_hashtable_slots: 4096
    tile_cache_count: 4000
    tile_direct_mapping: true
word_delimiters: " /\\()\"'-.,:;<>~!@#$%^&*+=[]{{}}~?|│"
read_buffer_size: {}
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
    std::cout << std::format(AdditionalInfo, vtbackend::DefaultPtyReadBufferSize);
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
    using Category = vtbackend::FunctionCategory;
    using std::pair;
    using vtbackend::VTExtension;
    using namespace std::string_view_literals;

    for (auto const& [category, headline]: { pair { Category::C0, "Control Codes"sv },
                                             pair { Category::ESC, "Escape Sequences"sv },
                                             pair { Category::CSI, "Control Sequences"sv },
                                             pair { Category::OSC, "Operating System Commands"sv },
                                             pair { Category::DCS, "Device Control Sequences"sv } })
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
    // Looked up first: a shell with no script gets no file, rather than an empty one to source.
    auto const shell = parameters().get<string>("contour.generate.integration.shell");
    auto const script = shellIntegrationScript(shell);
    if (!script)
    {
        std::cerr << unsupportedShellMessage(shell);
        return EXIT_FAILURE;
    }
    return writeOutput(parameters(), "contour.generate.integration.to", *script);
}

int ContourApp::configAction()
{
    return writeOutput(parameters(), "contour.generate.config.to", config::defaultConfigString());
}

int ContourApp::terminfoAction()
{
    return writeOutput(
        parameters(), "contour.generate.terminfo.to", vtbackend::capabilities::StaticDatabase {}.terminfo());
}

int ContourApp::captureAction()
{
    // clang-format off
    auto captureSettings = contour::cli::CaptureSettings {};
    captureSettings.lineMode = parameters().get<bool>("contour.capture.logical") ? vtbackend::CaptureLineMode::Logical
                                                                                : vtbackend::CaptureLineMode::Physical;
    captureSettings.words = parameters().get<bool>("contour.capture.words");
    captureSettings.timeout = parameters().get<double>("contour.capture.timeout");
    captureSettings.lineCount = vtbackend::LineCount::cast_from(parameters().get<unsigned>("contour.capture.lines"));
    captureSettings.outputFile = parameters().get<string>("contour.capture.to");
    // clang-format on

    if (contour::cli::captureScreen(captureSettings))
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
        auto ifs = std::ifstream(path, std::ios::binary | std::ios::ate);
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
                      crispy::Size screenSize,
                      int layer,
                      string_view fileName)
    {
        auto constexpr MaxImageFileSize = static_cast<std::uintmax_t>(16 * 1024 * 1024); // GIP body limit
        auto const data =
            readFile(core::pathFromUtf8(fileName).value_or(std::filesystem::path {}), MaxImageFileSize);
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

        auto encoderState = core::base64::EncoderState {};
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
            core::base64::encode(byte, encoderState, writer);
            if (buf.size() >= 4096)
                flush();
        }
        core::base64::finish(encoderState, writer);
        flush();

        cout << ST;
        cout.flush();
    }

#ifdef CONTOUR_EXTENSIONS
    /// What a daemon's status-line extensions borrow. Made before the daemon runs, so that it outlives the
    /// extensions the daemon's session host owns.
    struct DaemonExtensionResources
    {
        /// What the extensions' engines are created from. First: it outlives everything made from it, and
        /// only a reference to it travels with the services.
        std::unique_ptr<vtextension::wasm::IWasmBackend> backend = vtextension::wasm::makeDefaultBackend();
        core::async::StopSource commandShutdown;
        vtpty::SystemCommandCapture capture {
            vtpty::SystemCommandCapture::defaultShell(core::defaultEnvironment()), commandShutdown.get_token()
        };
        core::platform::SystemWallClock wallClock;
        vtbackend::LocalTimeConverter localTime { &vtbackend::systemLocalTime };
    };

    /// @param resolved The daemon's profile, resolved.
    /// @param daemon The socket the daemon serves, which names its grants file.
    /// @param executableDirectory Where the running executable is, beside which bundled extensions may be.
    /// @param services What the extensions borrow.
    /// @return What makes the profile's status-line extensions; unset when it declares none.
    [[nodiscard]] std::function<std::shared_ptr<vthost::IExtensionHosting>()> daemonExtensionsOf(
        config::ResolvedSessionConfig const& resolved,
        vtextension::DaemonIdentity const& daemon,
        std::filesystem::path const& executableDirectory,
        DaemonExtensionResources& services)
    {
        auto declared = config::declaredExtensionsOf(resolved.extensions, resolved.statusLineExtension);
        if (auto const diagnostic = vtextension::undeclaredTakeOver(declared))
            errorLog()("{}", *diagnostic);
        if (declared.extensions.empty())
            return {};
        auto runtime = vtextension::ExtensionRuntimeConfig {
            // The daemon's own side file, beside the configuration it was started with, never the GUI's.
            .grantsFile =
                resolved.configFile.parent_path() / vtextension::daemonExtensionGrantsFileName(daemon),
            .bundledDirectories =
                vtextension::bundledExtensionDirectories(vtextension::nativeBundledExtensionPlatform(),
                                                         core::defaultEnvironment(),
                                                         executableDirectory),
            .workerCount = vtextension::extensionWorkerCount(std::thread::hardware_concurrency()),
            .limits = {},
        };
        return [&services, declared = std::move(declared), runtime = std::move(runtime)] {
            return std::shared_ptr<vthost::IExtensionHosting> { std::make_shared<vthost::DaemonExtensions>(
                vthost::DaemonExtensionsConfig { .declared = declared, .runtime = runtime },
                vthost::DaemonExtensionServices {
                    .runtime =
                        vtextension::ExtensionRuntimeServices {
                            .backend = *services.backend,
                            .capture = services.capture,
                            .wallClock = services.wallClock,
                            .localTime = services.localTime,
                            .environment = core::defaultEnvironment(),
                            .fileSystem = core::platform::NativeFileSystem::instance() },
                    .commandShutdown = services.commandShutdown }) };
        };
    }
#endif

    /// Moves a daemon about to serve onto its staged image, or leases the staged image it already runs
    /// from.
    ///
    /// A daemon that moves does not come back from here: its process image is replaced by the staged
    /// copy, which keeps the process id (what systemd and launchd supervise) and enters here again, now
    /// Staged. Any failure is logged and the daemon runs in place.
    /// @param images This process's image store.
    /// @param request Whether `--in-place` refused the move.
    /// @param commandLine This process's argv, which the staged copy is run with.
    /// @return The lease to hold until the daemon is done serving; null when it runs in place.
    [[nodiscard]] std::unique_ptr<vthost::ImageLease> settleDaemonImage(vthost::NativeDaemonImages& images,
                                                                        vthost::RelocationRequest request,
                                                                        std::vector<std::string> commandLine)
    {
        auto const placement = vthost::imagePlacementOf(images.store(), images.inputs());
        if (placement == vthost::ImagePlacement::Staged)
        {
            // Taken before the listener binds, so that the launcher's lease may end once this serves; it
            // also prunes the images nobody leases, which a service-started daemon has no launcher to do.
            auto lease = vthost::leaseRunningImage(images.store(), images.leases(), images.inputs());
            if (!lease)
            {
                errorLog()("cannot lease the image this daemon runs from ({}); a prune may remove it",
                           lease.error().toString());
                return nullptr;
            }
            return std::move(*lease);
        }
        if (vthost::relocationFor(images.store().layout(), vthost::NativeHostPlatform, placement, request)
            == vthost::Relocation::ExecStaged)
        {
            auto const failure = vthost::execFromStagedImage(images.store(),
                                                             images.leases(),
                                                             *vthost::makeNativeProcessImageReplacer(),
                                                             std::move(commandLine));
            errorLog()("cannot run from a per-version copy ({}); running in place", failure);
        }
        return nullptr;
    }

    /// @param env The environment the state directory and the label's own socket are derived from.
    /// @param label The daemon's label.
    /// @param socket The socket the daemon listens on.
    /// @return The file the daemon labelled @p label at @p socket keeps its workspace in; nullopt for a label
    ///         no file can be named for.
    [[nodiscard]] std::optional<std::filesystem::path> snapshotPathOf(core::Environment const& env,
                                                                      std::string_view label,
                                                                      std::filesystem::path const& socket)
    {
        auto const& fileSystem = core::platform::NativeFileSystem::instance();
        return vthost::daemonSnapshotPath(vthost::daemonStateDirectory(env),
                                          label,
                                          fileSystem.weaklyCanonical(socket),
                                          fileSystem.weaklyCanonical(vthost::labelSocketPath(label, env)));
    }

    /// @return Whether `--in-place` refuses a move onto a staged image.
    [[nodiscard]] vthost::RelocationRequest relocationRequestOf(bool inPlace) noexcept
    {
        return inPlace ? vthost::RelocationRequest::Refused : vthost::RelocationRequest::Allowed;
    }
} // namespace

namespace
{
    /// How often a followed daemon log is polled for new output.
    constexpr auto LogFollowInterval = std::chrono::milliseconds { 250 };
} // namespace

int ContourApp::daemonAction()
{
    // core::cli::App dispatches the first true flag in name order, and `contour.daemon` sorts before
    // `contour.daemon.status`, so the daemon's own handler is the one that runs for a sub-verb too.
    for (auto const& row: DaemonSubVerbs)
    {
        auto const prefix = std::format("contour.daemon.{}", row.name);
        if (parameters().get<bool>(prefix))
            return daemonVerbAction(row.verb, prefix);
    }

    // On --stdio every byte of stdout belongs to the protocol, and from before the configuration
    // loads. The log sinks write to stderr, but a stray std::cout or printf write, or a daemon this
    // relay auto-starts and that inherits descriptor 1, would still corrupt a frame, so
    // makeProcessStdioStream() points stdout at stderr. A refused combination is reported first, with
    // stdout still untouched.
    auto stdioClient = std::shared_ptr<vthost::IByteStream> {};
    if (parameters().get<bool>("contour.daemon.stdio"))
    {
        if (auto const combined = checkStdioCombinations(); !combined)
        {
            cerr << std::format("contour daemon: {}\n", combined.error());
            return EXIT_FAILURE;
        }
        stdioClient = vthost::makeProcessStdioStream();
    }
    // Reports a failure on stderr, and on --stdio to the attaching client as well, as an error preamble.
    auto const refuse = [&stdioClient](std::string const& reason) {
        cerr << std::format("contour daemon: {}\n", reason);
        if (!stdioClient)
            return EXIT_FAILURE;
        return vthost::runStdioAttach(
            std::move(stdioClient),
            [&reason]() -> std::expected<std::shared_ptr<vthost::IByteStream>, std::string> {
                return std::unexpected { reason };
            });
    };

    // The hosted terminals' full session configuration: emulation settings, presentation/color
    // fields, the shell to run and its sandbox policy. The profile's `history.limit` is
    // load-bearing here rather than cosmetic -- @see vthost::DefaultSessionHistoryLineCount. This
    // is also, deliberately, the ONE place a daemon's hosted sessions get their shell and
    // presentation from: a profile the user named must not be silently overridden by the bare OS
    // login shell -- @see the contour daemon / contour client profile-resolution issue.
    //
    // Loaded once, and first: its `daemon:` section names the socket when --socket does not, and the
    // same configuration resolves the daemon's own profile below and, as the host's profile catalog,
    // every profile a client names later.
    auto hostConfig = config::tryLoadConfigNamed(parameters().get<string>("contour.daemon.config"));
    if (!hostConfig)
        return refuse(hostConfig.error());
    auto const daemonSettings = hostConfig->daemon.value();

    auto config = vthost::DaemonConfig {};
    // A new run of the daemon: a client that reconnects to it can tell it from the one it lost.
    config.instance = vthost::drawDaemonInstance();
    config.socketPath = vthost::muxSocketPath(
        parameters().get<string>("contour.daemon.label"),
        resolveSocketOption(parameters().get<string>("contour.daemon.socket"), daemonSettings.socket));

    // Before logging, whose default file lies beside the socket: a log sink creating the socket's
    // directory would give it the umask's 0755, which the listener refuses as world-accessible.
    if (auto const prepared = vthost::prepareSocketDirectory(config.socketPath); !prepared)
        return refuse(prepared.error());

    // Logging is installed next, so every diagnostic below — socket binds, TLS setup — is
    // captured. The app owns it, so it outlives runDaemon (categories hold a reference into its
    // sink) and every thread runDaemon spawns has been joined by the time it returns.
    if (auto const installed = installLogging("contour.daemon", /*showProcessId=*/true); !installed)
        return refuse(installed.error());
    // Loaded again, now that there is a log: what the load has to say -- a key it does not know, a value it
    // corrects -- was said to the default sink the first time, which for a --background daemon is nowhere a
    // user looks. The socket stays the one derived above; a file that no longer loads keeps the first load.
    if (auto reloaded = config::tryLoadConfigNamed(parameters().get<string>("contour.daemon.config"));
        reloaded)
        hostConfig = std::move(reloaded);

    // Logging's default sink is stderr, never stdout, and --log-file names a file or stderr; stdout
    // has pointed at stderr since makeProcessStdioStream() above besides.
    if (stdioClient)
    {
        // The option, else the configuration's `daemon.size_policy`, as for a daemon this process becomes.
        auto const sizePolicy = resolveSizePolicy(parameters().get<string>("contour.daemon.size-policy"),
                                                  daemonSettings.sizePolicy);
        if (!sizePolicy)
            return refuse(sizePolicy.error());
        return stdioRelayAction(std::move(stdioClient), config.socketPath, *sizePolicy);
    }

    auto const daemonProfile = parameters().get<string>("contour.daemon.profile");
    auto const resolved = config::resolvedSessionConfigFor(*hostConfig, daemonProfile);
    if (!resolved)
    {
        cerr << std::format("contour daemon: {}\n", resolved.error());
        return EXIT_FAILURE;
    }
    // Read before the configuration moves into the catalog. Not reloaded: a running daemon keeps the file
    // and the schedule it started with.
    auto const daemonLabel = parameters().get<string>("contour.daemon.label");
    if (auto snapshotPath = snapshotPathOf(processEnvironment(), daemonLabel, config.socketPath))
        config.persistence =
            workspacePersistenceOf(hostConfig->daemon.value().restore, *std::move(snapshotPath));
    else
        errorLog()("the label '{}' names no snapshot file; this daemon keeps no workspace", daemonLabel);
    auto profiles = std::make_shared<ConfigProfileCatalog const>(
        *std::move(hostConfig), daemonProfile, vthost::HostedColorPreference);
    // Edits of the configuration reach the sessions spawned afterwards, and the scrollback of those
    // running, without a restart; a load that fails keeps the running configuration.
    auto reloads = std::make_shared<ConfigReloadPoller>(
        core::platform::NativeFileSystem::instance(),
        resolved->configFile,
        [configPath = parameters().get<string>("contour.daemon.config"),
         daemonProfile]() -> std::expected<std::shared_ptr<vthost::ProfileCatalog const>, std::string> {
            return config::tryLoadConfigNamed(configPath)
                .and_then([&](config::Config loaded) {
                    // The daemon's own profile must survive the edit, as it had to for the daemon to start.
                    return config::resolvedSessionConfigFor(loaded, daemonProfile)
                        .transform([&](auto const&) {
                            return std::make_shared<ConfigProfileCatalog const>(
                                std::move(loaded), daemonProfile, vthost::HostedColorPreference);
                        });
                })
                .transform(
                    [](auto catalog) -> std::shared_ptr<vthost::ProfileCatalog const> { return catalog; });
        });
    config.configReload = vthost::ConfigReloadHook { .poll = [reloads] { return reloads->poll().second; } };
    // Through the catalog, so a session that names no profile gets `daemon.history_limit` too.
    auto defaultProfile = profiles->resolve({});
    config.settings = std::move(defaultProfile.settings);
    config.shell = std::move(defaultProfile.shell);
    config.profiles = std::move(profiles);
    config.escapeSandbox = resolved->escapeSandbox;
    config.placement = vtpty::makeDefaultProcessPlacement();
    config.startupLayout = resolved->startupLayout;
#ifdef CONTOUR_EXTENSIONS
    // The extensions' modules and configuration are resolved here, on the daemon's side, as its sessions'
    // shells are; they run in the daemon, where each session's terminal is.
    auto extensionServices = DaemonExtensionResources {};
    auto const label = parameters().get<string>("contour.daemon.label");
    auto const& fileSystem = core::platform::NativeFileSystem::instance();
    config.extensions = daemonExtensionsOf(
        *resolved,
        vtextension::DaemonIdentity { .label = label,
                                      .defaultLabel = vthost::DefaultSocketLabel,
                                      .socketPath = fileSystem.weaklyCanonical(config.socketPath),
                                      .labelSocketPath =
                                          fileSystem.weaklyCanonical(vthost::labelSocketPath(label)) },
        std::filesystem::absolute(
            core::pathFromUtf8(commandLine().front()).value_or(std::filesystem::path {}))
            .parent_path(),
        extensionServices);
#endif

    if (auto label = parameters().get<string>("contour.daemon.tmux-compat-socket"); !label.empty())
        config.tmuxCompatLabel = std::move(label);

    // Rejected rather than defaulted, as --size-policy is below.
    auto const lifecycleText = parameters().get<string>("contour.daemon.lifecycle");
    if (auto const lifecycle = vthost::daemonLifecycleFrom(lifecycleText))
        config.lifecycle = *lifecycle;
    else
    {
        cerr << std::format("contour daemon: unknown --lifecycle '{}' (expected one of: {})\n",
                            lifecycleText,
                            core::joinHumanReadable(vthost::DaemonLifecycleNames | std::views::keys));
        return EXIT_FAILURE;
    }

    // The alias for --lifecycle=exit-with-last-session; stating both only conflicts when they differ.
    if (parameters().get<bool>("contour.daemon.exit-with-last-session"))
    {
        if (config.lifecycle != vthost::DaemonLifecycle::Persistent
            && config.lifecycle != vthost::DaemonLifecycle::ExitWhenEmpty)
        {
            cerr << "contour daemon: --exit-with-last-session conflicts with --lifecycle=" << lifecycleText
                 << "\n";
            return EXIT_FAILURE;
        }
        config.lifecycle = vthost::DaemonLifecycle::ExitWhenEmpty;
    }

    // The option, else the configuration's `daemon.size_policy`.
    if (auto const policy = resolveSizePolicy(parameters().get<string>("contour.daemon.size-policy"),
                                              daemonSettings.sizePolicy))
        config.sizePolicy = *policy;
    else
    {
        cerr << std::format("contour daemon: {}\n", policy.error());
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
        // Narrower tokens for the control API, beside the one that grants everything, never repeating it.
        auto accessTokens = std::vector<vthost::AcceptedToken> {};
        if (auto const accessFile = parameters().get<string>("contour.daemon.access-file");
            !accessFile.empty())
        {
            auto const path = core::pathFromUtf8(accessFile);
            if (!path)
            {
                cerr << std::format("contour daemon: --access-file '{}' is not a valid path: {}\n",
                                    accessFile,
                                    path.error().message());
                return EXIT_FAILURE;
            }
            auto read = vthost::readAccessFile(core::platform::NativeFileSystem::instance(), *path, *token);
            if (!read)
            {
                cerr << std::format("contour daemon: {}\n", read.error());
                return EXIT_FAILURE;
            }
            accessTokens = *std::move(read);
        }
        // A token is REQUIRED here, not merely supported. On the unix socket the filesystem
        // permissions are the gate; TCP has no such gate, so serving it without a token would
        // hand a full shell to anyone who can reach the port. Refusing to start is the only safe
        // reading of `--listen-tcp` with no credential — a warning would be dismissed once and
        // then run that way forever.
        if (token->empty() && accessTokens.empty())
        {
            cerr << "contour daemon: --listen-tcp requires --token, --token-file or a non-empty "
                    "--access-file; the TCP transport has no filesystem permissions to fall back on.\n";
            return EXIT_FAILURE;
        }

        config.nativeTcp = vthost::NativeTcpListenerConfig {
            .host = hostPort->first,
            .port = hostPort->second,
            .token = *std::move(token),
            .accessTokens = std::move(accessTokens),
            .tlsCertPath = parameters().get<string>("contour.daemon.tls-cert"),
            .tlsKeyPath = parameters().get<string>("contour.daemon.tls-key"),
        };
    }
    else if (!parameters().get<string>("contour.daemon.access-file").empty())
    {
        // Its tokens are only ever asked for over TCP: accepting the option alone would read as
        // narrowing a listener that does not exist.
        cerr << "contour daemon: --access-file needs --listen-tcp\n";
        return EXIT_FAILURE;
    }

    // Everything above has validated the configuration, so a typo is reported by THIS process
    // rather than killing a child whose stderr nobody is reading. Only now is it safe to hand
    // the work to a detached copy of ourselves.
    if (parameters().get<bool>("contour.daemon.background"))
        return runDaemonInBackground(config.socketPath);

    // What this daemon states about itself, in every hello and status reply.
    config.releaseVersion = CONTOUR_VERSION_STRING;
    if (auto logFile = parameters().get<string>("contour.daemon.log-file"); logFile != "-")
        config.logFile = core::pathFromUtf8(logFile).value_or(std::filesystem::path {});

    // Where it runs from: a per-version copy wherever an upgrade would disturb the installed binary.
    // Held until runDaemon returns.
    auto runningLease = std::unique_ptr<vthost::ImageLease> {};
    auto images = vthost::NativeDaemonImages::ofThisProcess(
        processEnvironment(), core::platform::NativeFileSystem::instance(), CONTOUR_VERSION_STRING);
    if (images)
    {
        runningLease = settleDaemonImage(
            **images, relocationRequestOf(parameters().get<bool>("contour.daemon.in-place")), commandLine());
        config.executablePath = vthost::stagedImageOf((*images)->store(), (*images)->inputs())
                                    .value_or(vthost::launchedImage((*images)->inputs()));
    }
    else
    {
        errorLog()("cannot locate the running executable ({}); running in place", images.error().message());
        config.executablePath = std::filesystem::absolute(
            core::pathFromUtf8(commandLine().front()).value_or(std::filesystem::path {}));
    }

    return vthost::runDaemon(config);
}

namespace
{
    /// Each names something only a daemon this process would become can do; the relay becomes none.
    constexpr auto StdioRefusals = std::array {
        OptionPresence { .name = "listen-tcp", .givenAs = GivenAs::Text },
        OptionPresence { .name = "background", .givenAs = GivenAs::Flag },
        OptionPresence { .name = "tmux-compat-socket", .givenAs = GivenAs::Text },
    };
} // namespace

std::optional<std::string_view> ContourApp::firstGivenOption(std::string_view verbPrefix,
                                                             std::span<OptionPresence const> options)
{
    for (auto const& option: options)
    {
        auto const key = std::format("{}.{}", verbPrefix, option.name);
        if (option.givenAs == GivenAs::Flag ? parameters().get<bool>(key)
                                            : !parameters().get<string>(key).empty())
            return option.name;
    }
    return std::nullopt;
}

std::expected<void, std::string> ContourApp::checkStdioCombinations()
{
    if (auto const given = firstGivenOption("contour.daemon", StdioRefusals))
        return std::unexpected { std::format("--stdio relays to a daemon; it cannot be combined with --{}",
                                             *given) };
    return {};
}

int ContourApp::stdioRelayAction(std::shared_ptr<vthost::IByteStream> client,
                                 std::filesystem::path const& socketPath,
                                 vthost::ClientSizePolicy sizePolicy)
{
#ifndef _WIN32
    // A client that went away is a write error that ends the relay with RelayEnd::Failed, not a SIGPIPE
    // that ends it as signal 13 in the ssh session's exit status.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    // An auto-started daemon is told what `contour client` would tell it.
    auto const options = vthost::DaemonSpawnOptions {
        .filter = parameters().get<string>("contour.daemon.log"),
        .logFile = parameters().get<string>("contour.daemon.log-file"),
        .configPath = parameters().get<string>("contour.daemon.config"),
        .profileName = parameters().get<string>("contour.daemon.profile"),
        .sizePolicy = sizePolicy,
    };
    auto const binary = vthost::makeOwnDaemonBinary(
        processEnvironment(),
        core::platform::NativeFileSystem::instance(),
        CONTOUR_VERSION_STRING,
        core::pathFromUtf8(commandLine().front()).value_or(std::filesystem::path {}));
    auto const spawner = vthost::makeNativeDaemonSpawner();
    auto const commands = vthost::makeNativeCommandRunner();
    auto const services =
        vthost::ServiceBackendServices { .fileSystem = core::platform::NativeFileSystem::instance(),
                                         .commands = *commands,
                                         .environment = processEnvironment() };
    auto const placement = vtpty::makeDefaultProcessPlacement();
    // The registered user service when there is one, so a stopped service is started rather than an
    // on-demand daemon beside it.
    auto const label = parameters().get<string>("contour.daemon.label");
    auto starter = vthost::chooseDaemonStarter(
        vthost::findRegisteredService(label, vthost::nativeServiceBackendFactory(services)),
        vthost::labelSocketPath(label, services.environment),
        vthost::OnDemandSpawn { .endpoint = vthost::UnixEndpoint { .socketPath = socketPath },
                                .spawner = *spawner,
                                .binary = *binary,
                                .placement = *placement,
                                .options = options,
                                .timeout = std::chrono::seconds(5) });
    return vthost::runStdioAttach(std::move(client),
                                  vthost::makeLocalDaemonConnector(socketPath, *spawner, std::move(starter)));
}

int ContourApp::agentSetupAction()
{
    constexpr auto Prefix = "contour.agent.setup"sv;
    auto const key = [&](std::string_view option) {
        return std::format("{}.{}", Prefix, option);
    };
    auto const& env = processEnvironment();
    auto const homeText = env.get("HOME").or_else([&] { return env.get("USERPROFILE"); });
    auto const home = homeText ? core::pathFromUtf8(*homeText).value_or(std::filesystem::path {})
                               : std::filesystem::path {};
    if (home.empty())
    {
        cerr << "Error: cannot tell your home directory ($HOME is not set)\n";
        return exitCode(ExitStatus::Failed);
    }
    auto const mode = parameters().get<bool>(key("dry-run")) ? SetupMode::DryRun : SetupMode::Write;
    auto const foreign = parameters().get<bool>(key("force")) ? ForeignSkill::Replace : ForeignSkill::Keep;
    auto const agents = parseAgentList(parameters().get<string>(key("agent")));
    auto const outcomes = setupAgentSkills(core::platform::NativeFileSystem::instance(),
                                           env,
                                           home,
                                           agents,
                                           mode,
                                           foreign,
                                           renderAgentSkill(CONTOUR_VERSION_STRING));
    if (!outcomes)
    {
        cerr << "Error: " << outcomes.error() << '\n';
        return exitCode(ExitStatus::Failed);
    }
    cout << formatSkillOutcomes(
        *outcomes, mode, parameters().get<bool>(key("json")) ? OutcomeFormat::Json : OutcomeFormat::Text);
    return exitCode(ExitStatus::Ok);
}

int ContourApp::controlVerbAction(ControlVerb const& verb)
{
#ifdef _WIN32
    auto const target = _isatty(_fileno(stdout)) != 0 ? OutputTarget::Terminal : OutputTarget::Pipe;
#else
    auto const target = isatty(STDOUT_FILENO) != 0 ? OutputTarget::Terminal : OutputTarget::Pipe;
#endif
    auto const wallClock = core::platform::SystemWallClock {};
    // Only a stream is ended by Ctrl+C; every other verb is short enough to be killed.
    auto interrupt = std::optional<InterruptStop> {};
    if (verb.mode == VerbMode::Stream)
        interrupt.emplace();
    // Viewed by the spawner, so it lives as long as the call.
    auto const sshSearchPath = vtpty::execSearchPath(processEnvironment().get("PATH"));
    auto& fileSystem = core::platform::NativeFileSystem::instance();
    auto resolveSavedHost = makeSavedHostResolver(
        [](std::string_view nameOrUrl) { return config::resolveHost(std::string {}, nameOrUrl); },
        fileSystem,
        vthost::makeSystemStdioSpawner(vtpty::ProgramSearch {
            .path = sshSearchPath, .workingDirectory = {}, .fileSystem = fileSystem }));
    return runControlVerb(verb,
                          parameters(),
                          ControlIo { .env = processEnvironment(),
                                      .fs = core::platform::NativeFileSystem::instance(),
                                      .in = std::cin,
                                      .out = cout,
                                      .err = cerr,
                                      .clock = wallClock,
                                      .target = target,
                                      .stop = interrupt ? interrupt->token() : core::async::StopToken {},
                                      .resolveSavedHost = std::move(resolveSavedHost) });
}

int ContourApp::daemonVerbAction(DaemonVerb verb, std::string const& prefix)
{
    // `daemon --socket X status` and `daemon status --socket X` both mean X.
    auto const label = mergeSharedOption("label",
                                         parameters().get<string>("contour.daemon.label"),
                                         parameters().get<string>(prefix + ".label"),
                                         string { vthost::DefaultSocketLabel });
    auto const socketOption = mergeSharedOption("socket",
                                                parameters().get<string>("contour.daemon.socket"),
                                                parameters().get<string>(prefix + ".socket"),
                                                string {});
    if (!label || !socketOption)
    {
        cerr << std::format("contour daemon: {}\n", !label ? label.error() : socketOption.error());
        return exitCode(ExitStatus::BadUsage);
    }
    auto const socket = vthost::muxSocketPath(*label, *socketOption);
    // Never starts a daemon: asking about one, or stopping it, must not bring one up. A stop waits for the
    // daemon's last snapshot.
    auto control =
        vthost::client::makeDaemonControl(std::make_shared<vthost::UnixTransportFactory>(socket, nullptr),
                                          verb == DaemonVerb::Stop ? vthost::client::DefaultStopTimeout
                                                                   : vthost::client::DefaultControlTimeout);

    switch (verb)
    {
        case DaemonVerb::Status: return exitCode(daemonStatusVerb(*control, socket, cout, cerr));
        case DaemonVerb::Stop: {
            auto prompt = makeTerminalPrompt(std::cin, cout);
            auto const snapshotPath = snapshotPathOf(processEnvironment(), *label, socket);
            auto const savedWorkspace = snapshotPath.transform([](std::filesystem::path const& path) {
                return vthost::SessionSnapshotStore { core::platform::NativeFileSystem::instance(),
                                                      vthost::NativeFilePrivacy::instance(),
                                                      path };
            });
            auto const options = StopOptions {
                .consent = parameters().get<bool>(prefix + ".yes") ? StopConsent::Given : StopConsent::Ask,
                .disposition = parameters().get<bool>(prefix + ".keep-snapshot")
                                   ? StopDisposition::KeepSnapshot
                                   : StopDisposition::Discard,
                .savedWorkspace = savedWorkspace ? &*savedWorkspace : nullptr,
            };
            return exitCode(daemonStopVerb(*control, socket, options, *prompt, cout, cerr));
        }
        case DaemonVerb::Logs: {
            auto const lines = parameters().get<unsigned>(prefix + ".lines");
            auto const options = LogsOptions {
                .lines = static_cast<std::size_t>(lines),
                .follow = parameters().get<bool>(prefix + ".follow") ? FollowMode::Follow : FollowMode::Once,
            };
            auto const waitTick = std::function<FollowStep()> { [] {
                std::this_thread::sleep_for(LogFollowInterval);
                return FollowStep::Continue; // Ctrl+C ends the process.
            } };
            return exitCode(daemonLogsVerb(*control,
                                           vthost::daemonLogPath(socket),
                                           options,
                                           core::platform::NativeFileSystem::instance(),
                                           waitTick,
                                           cout,
                                           cerr));
        }
        case DaemonVerb::Run: break;
    }
    return exitCode(ExitStatus::BadUsage);
}

int ContourApp::runDaemonInBackground(std::filesystem::path const& socketPath)
{
    // Our own tokens replayed verbatim — rebuilding the child's command line from parsed flags
    // would silently drop every option someone forgets to add to the rebuilder.
    auto childArgs = commandLine();

    // The flag that brought us here is OVERRIDDEN rather than filtered out of them, because a token
    // filter cannot recognize all of its spellings and gets it wrong in both directions.
    // core::cli accepts `--background`, `-background` and the bare `background` (its natural
    // style), each optionally followed by an explicit value in either the `=VALUE` or the
    // separate-token form. A surviving token makes the child spawn ANOTHER detached child, which
    // spawns another — a slow fork bomb, each generation additionally polling for readiness — while
    // dropping only the flag of `--background true` leaves the orphaned `true` for the parser to
    // reject, and the user is told the daemon did not start with no cause given. Options are
    // last-one-wins (@see core::cli's setOption, which overwrites), so stating it once more at the
    // end settles it whatever the user wrote.
    childArgs.emplace_back("--background=false"); // the child runs in ITS foreground, or nothing binds

    // A detached process has no console, so its standard error goes nowhere: without a log file
    // the backgrounded daemon becomes the one instance nobody can diagnose. Derived rather than
    // required, so the common case needs no second flag.
    auto logFile = parameters().get<string>("contour.daemon.log-file");
    if (logFile.empty())
    {
        logFile = core::pathToUtf8(vthost::daemonLogPath(socketPath));
        childArgs.push_back(std::format("--log-file={}", logFile));
    }

    // Started from this version's own copy, unless --in-place asks otherwise, and that copy held leased
    // until the child serves: by then it has leased the copy itself.
    auto const argv0 = core::pathFromUtf8(childArgs.front()).value_or(std::filesystem::path {});
    auto const binary = relocationRequestOf(parameters().get<bool>("contour.daemon.in-place"))
                                == vthost::RelocationRequest::Allowed
                            ? vthost::makeOwnDaemonBinary(processEnvironment(),
                                                          core::platform::NativeFileSystem::instance(),
                                                          CONTOUR_VERSION_STRING,
                                                          argv0)
                            : std::make_unique<vthost::InstalledDaemonBinary>(argv0);
    auto const image = binary->imageForSpawn();
    childArgs.front() = core::pathToUtf8(image.executable);

    // Placed, as a session's shell is: typed into a tab, it would otherwise live in that tab's scope.
    auto const placement = vtpty::makeDefaultProcessPlacement();
    if (vthost::runDaemonDetached(std::move(childArgs), socketPath, *placement) != EXIT_SUCCESS)
    {
        cerr << std::format("contour daemon: the background daemon did not start; see {}\n", logFile);
        return EXIT_FAILURE;
    }

    cout << std::format(
        "contour daemon: serving on {} (logging to {})\n", core::pathToUtf8(socketPath), logFile);
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
                            core::joinHumanReadable(vthost::ServiceStartModeNames | std::views::keys));
        return EXIT_FAILURE;
    }

    auto const commands = vthost::makeNativeCommandRunner();
    auto const services =
        vthost::ServiceBackendServices { .fileSystem = core::platform::NativeFileSystem::instance(),
                                         .commands = *commands,
                                         .environment = processEnvironment() };

    // For a non-install verb the registration decides which backend owns it, and under which name: a
    // task registered by an older release carries the older name, so `status` must not report "not
    // installed" merely because it asked the newer name or the SCM.
    auto name = vthost::serviceNameForLabel(label);
    auto backend = std::unique_ptr<vthost::ServiceBackend> {};
    if (mode)
        backend = vthost::makeServiceBackend(*mode, name, services);
    else
    {
        if (auto found = vthost::findRegisteredService(label, vthost::nativeServiceBackendFactory(services)))
        {
            backend = std::move(found->backend);
            name = std::move(found->name);
        }
        if (!backend)
        {
            cerr << std::format("contour daemon-service: no registration named '{}'; run "
                                "'contour daemon-service install' first\n",
                                name);
            return EXIT_FAILURE;
        }
    }

    // The socket the registration serves on: the one its command line names, which install pinned, else
    // the label's default.
    auto const socketOf = [&](vthost::ServiceStatus const& status) {
        return vthost::registeredSocketPath(status.commandLine).value_or(vthost::muxSocketPath(label, {}));
    };

    // A daemon the OS did not start itself (a Task Scheduler task only runs the launcher that
    // backgrounds it) is stopped over its socket: stopping the service would stop nothing.
    auto const stopDaemon = [&]() -> std::expected<void, vthost::ServiceError> {
        if (vthost::stopVia(backend->launchStyle()) == vthost::StopRoute::ServiceManager)
            return backend->stop();
        auto const status = backend->status();
        if (!status)
            return std::unexpected(status.error());
        auto const control = vthost::client::makeDaemonControl(
            std::make_shared<vthost::UnixTransportFactory>(socketOf(*status), nullptr),
            vthost::client::DefaultStopTimeout);
        if (auto const stopped = control->stop(vthost::proto::StopReason::User);
            !stopped && stopped.error().code != vthost::client::ControlErrorCode::NotRunning)
            return std::unexpected(vthost::ServiceError { .code = vthost::ServiceErrorCode::Backend,
                                                          .context = stopped.error().toString() });
        return {};
    };

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
            auto commandLineOrError = daemonServiceCommandLine(prefix, label, backend->launchStyle());
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
        case DaemonServiceAction::Uninstall:
            std::ignore = stopDaemon();
            return report(backend->uninstall(), "uninstalled");
        case DaemonServiceAction::Start: return report(backend->start(), "started");
        case DaemonServiceAction::Stop: return report(stopDaemon(), "stopped");
        case DaemonServiceAction::Restart:
            // A stop that finds nothing running is not a failure here: restart's contract is
            // "be running afterwards", which a stopped registration already satisfies halfway.
            std::ignore = stopDaemon();
            return report(backend->start(), "restarted");
        case DaemonServiceAction::Status: {
            auto const status = backend->status();
            if (!status)
            {
                cerr << std::format(
                    "contour daemon-service: could not query {}: {}\n", name, status.error().toString());
                return EXIT_FAILURE;
            }
            // The socket decides whether it runs: a task that backgrounded its daemon reads "Ready".
            auto const socket = socketOf(*status);
            auto const live = vthost::makeNativeDaemonSpawner()->probe(socket);
            cout << std::format("{}: {} (starts at {})\n",
                                name,
                                vthost::nameOf(vthost::effectiveRunState(status->state, live)),
                                vthost::nameOf(status->mode));
            if (live == vthost::DaemonPresence::Serving)
                if (auto const daemon = vthost::client::makeDaemonControl(
                                            std::make_shared<vthost::UnixTransportFactory>(socket, nullptr))
                                            ->status())
                    cout << std::format("  version: {}\n", daemon->releaseVersion);
            if (!status->commandLine.empty())
                cout << std::format("  runs: {}\n", status->commandLine);
            return EXIT_SUCCESS;
        }
    }
    return EXIT_FAILURE;
}

std::expected<std::vector<std::string>, std::string> ContourApp::daemonServiceCommandLine(
    std::string const& prefix, std::string const& label, vthost::DaemonLaunchStyle launchStyle) const
{
    // Absolute, and resolved NOW: a registration outlives the shell that created it, and a
    // service's %TEMP%/%USERNAME% need not match the interactive user's — so a path left to be
    // derived at start time would resolve somewhere the user's own client never looks.
    auto const socketPath = vthost::muxSocketPath(label, parameters().get<string>(prefix + ".socket"));

    // The option, else `daemon.size_policy` of the configuration the installed daemon is given. Caught here
    // rather than by the installed daemon: a rejected value would otherwise kill a process started by the OS,
    // whose stderr nobody is watching.
    auto const sizePolicyText = parameters().get<string>(prefix + ".size-policy");
    auto const sizePolicy =
        sizePolicyText.empty()
            ? config::tryLoadConfigNamed(parameters().get<string>(prefix + ".config"))
                  .transform([](config::Config const& loaded) { return loaded.daemon.value().sizePolicy; })
            : resolveSizePolicy(sizePolicyText, vthost::ClientSizePolicy::Latest);
    if (!sizePolicy)
        return std::unexpected(sizePolicy.error());

    auto logFile = parameters().get<string>(prefix + ".log-file");
    if (logFile.empty())
        logFile = core::pathToUtf8(vthost::daemonLogPath(socketPath));

    // Through daemonSpawnArgs, which is already the ONE spelling of this flag list — the client's
    // auto-spawn builds its command line the same way, so a daemon option cannot end up forwarded
    // by one path and silently dropped by the other. Persistent, not ExitWhenEmpty: an installed
    // daemon exists precisely to still be there for the next client.
    auto const program = core::pathFromUtf8(commandLine().front()).value_or(std::filesystem::path {});
    auto arguments = vthost::daemonSpawnArgs(core::pathToUtf8(std::filesystem::absolute(program)),
                                             socketPath,
                                             vthost::DaemonSpawnOptions {
                                                 .filter = parameters().get<string>(prefix + ".log"),
                                                 .logFile = std::move(logFile),
                                                 .configPath = parameters().get<string>(prefix + ".config"),
                                                 .profileName = parameters().get<string>(prefix + ".profile"),
                                                 .sizePolicy = *sizePolicy,
                                             },
                                             vthost::DaemonLifecycle::Persistent);
    // A supervisor that tracks the process it started (systemd, launchd) must see the daemon itself; one
    // that only starts a launcher (the Task Scheduler) is given a daemon that detaches.
    if (launchStyle == vthost::DaemonLaunchStyle::SelfDetaching)
        arguments.emplace_back("--background");
    return arguments;
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

core::cli::Command ContourApp::parameterDefinition() const
{
    // NOLINTBEGIN
    auto command = CLI::Command {
        "contour",
        "Contour Terminal Emulator " CONTOUR_VERSION_STRING
        " - https://github.com/contour-terminal/contour/ ;-)",
        CLI::OptionList {},
        CLI::CommandList {
            CLI::Command { "help", "Shows this help and exits." },
            CLI::Command { "version", "Shows the version and exits." },
            CLI::Command { "license",
                           "Shows the license, and project URL of the used projects and Contour." },
            CLI::Command { "list-debug-tags", "Lists all available debug tags and exits." },
            CLI::Command {
                "info",
                "General informational outputs.",
                CLI::OptionList {},
                CLI::CommandList {
                    CLI::Command { "vt", "Prints general information about supported VT sequences." },
                    CLI::Command { "config", "Prints missing entries from user config file." },
                } },
            CLI::Command {
                "documentation",
                "Generate documentation for web page",
                CLI::OptionList {},
                CLI::CommandList {
                    CLI::Command { "vt", "VT sequence reference documentation" },
                    CLI::Command { "keys", "List of configurable actions for key binding" },
                    CLI::Command {
                        "configuration",
                        "Create documentation for configuration file",
                        CLI::OptionList {},
                        CLI::CommandList {
                            CLI::Command { "global",
                                           "Create documentation entry for global part of the config file" },
                            CLI::Command { "profile",
                                           "Create documentation entry for profile part of the config file" },
                        } },
                },
            },
            CLI::Command {
                "generate",
                "Generation utilities.",
                CLI::OptionList {},
                CLI::CommandList {
                    CLI::Command { "parser-table",
                                   "Dumps VT parser's state machine in dot-file format to stdout." },
                    CLI::Command {
                        "terminfo",
                        "Generates the terminfo source file that will reflect the features of "
                        "this version "
                        "of contour. Using - as value will write to stdout instead.",
                        {
                            CLI::Option { "to",
                                          CLI::Value { ""s },
                                          "Output file name to store the screen capture to. If - (dash) is "
                                          "given, the output will be written to standard output.",
                                          "FILE",
                                          CLI::Presence::Required },
                        } },
                    CLI::Command {
                        "config",
                        "Generates configuration file with the default configuration.",
                        CLI::OptionList {
                            CLI::Option { "to",
                                          CLI::Value { ""s },
                                          "Output file name to store the config file to. If - (dash) is "
                                          "given, the output will be written to standard output.",
                                          "FILE",
                                          CLI::Presence::Required },
                        } },
                    CLI::Command {
                        "integration",
                        "Generates shell integration script.",
                        CLI::OptionList {
                            CLI::Option { "shell",
                                          CLI::Value { ""s },
                                          integrationShellHelpText(),
                                          "SHELL",
                                          CLI::Presence::Required },
                            CLI::Option { "to",
                                          CLI::Value { ""s },
                                          "Output file name to store the shell integration file to. If - "
                                          "(dash) is given, the output will be written to standard output.",
                                          "FILE",
                                          CLI::Presence::Required },
                        } } } },
            CLI::Command {
                "cat",
                "Displays an image in the terminal using the Good Image Protocol.",
                CLI::OptionList {
                    CLI::Option { "resize",
                                  CLI::Value { "fit"s },
                                  "Sets the image resize policy.\n"
                                  "Policies available are:\n"
                                  " - no/none (no resize),\n"
                                  " - fit (resize to fit),\n"
                                  " - fill (resize to fill),\n"
                                  " - stretch (stretch to fill)." },
                    CLI::Option { "align",
                                  CLI::Value { "center"s },
                                  "Sets the image alignment policy.\n"
                                  "Possible values: top-start, top-center, top-end, "
                                  "middle-start, middle-center/center, middle-end, "
                                  "bottom-start, bottom-center, bottom-end." },
                    CLI::Option { "size",
                                  CLI::Value { "0x0"s },
                                  "Sets the amount of columns and rows to place the image onto "
                                  "(format: COLSxROWS, e.g. 80x24). "
                                  "The top-left of the area is the current cursor position." },
                    CLI::Option { "layer",
                                  CLI::Value { "1"s },
                                  "Sets the image layer relative to text.\n"
                                  "Values: 0/below (below text), 1/replace (replace text, default), "
                                  "2/above (above text)." } },
                CLI::CommandList {},
                CLI::CommandSelect::Explicit,
                CLI::Verbatim {
                    "IMAGE_FILE",
                    "Path to image to be displayed. Image formats supported are at least PNG, JPG." } },
            CLI::Command { "daemon-service",
                           "Installs, removes and controls an OS-managed Contour daemon that starts on its "
                           "own. A sibling verb rather than `daemon service` because options bind to the "
                           "command they follow, and the daemon verb already owns its own set.",
                           CLI::OptionList {},
                           daemonServiceCommands() },
            CLI::Command {
                "daemon",
                "(experimental) Runs the headless terminal multiplexer daemon, serving sessions "
                "to attaching clients over a control socket. Its options and wire protocol may "
                "still change between releases.",
                CLI::OptionList {
                    CLI::Option { "socket",
                                  CLI::Value { ""s },
                                  "Path of the control socket file. Defaults to the configuration's "
                                  "`daemon.socket`, else $XDG_RUNTIME_DIR/contour/LABEL (respecting "
                                  "$CONTOUR_MUX).",
                                  "PATH" },
                    CLI::Option { "label",
                                  CLI::Value { std::string { vthost::DefaultSocketLabel } },
                                  "Socket label distinguishing daemon instances.",
                                  "NAME" },
                    CLI::Option { "config",
                                  CLI::Value { ""s },
                                  "Path to the configuration file whose profile supplies the "
                                  "hosted sessions' terminal settings (history depth, terminal "
                                  "id, reflow, image limits).",
                                  "FILE" },
                    CLI::Option { "profile",
                                  CLI::Value { ""s },
                                  "Config profile the hosted sessions' terminal settings come "
                                  "from. Defaults to the configuration's default profile.",
                                  "NAME" },
                    CLI::Option { "background",
                                  CLI::Value { false },
                                  "Relaunches this command detached and returns once the daemon "
                                  "is accepting connections, instead of holding the terminal. "
                                  "Without --log-file the detached daemon logs beside its socket "
                                  "(SOCKET.log), since a detached process has no console to write "
                                  "its diagnostics to. On Linux the daemon runs in a systemd scope of "
                                  "its own where systemd answers before this command returns; "
                                  "otherwise it stays where it was started." },
                    CLI::Option { "stdio",
                                  CLI::Value { false },
                                  "Relays the native protocol between standard input/output and this "
                                  "user's daemon, starting it if needed. What `contour client --host` "
                                  "runs over ssh." },
                    CLI::Option { "in-place",
                                  CLI::Value { false },
                                  "Runs from the installed binary instead of a per-version copy; a "
                                  "package upgrade may then replace it under the running daemon." },
                    CLI::Option {
                        "lifecycle",
                        CLI::Value { std::string { vthost::nameOf(vthost::DaemonLifecycle::Persistent) } },
                        "When the daemon ends on its own: `persistent` (the default; keeps "
                        "serving with no sessions, so a daemon started by hand is still there "
                        "for the next client), `exit-with-last-session` (ends once its last "
                        "hosted session is gone; tmux spells this `exit-empty`) or "
                        "`exit-when-idle` (ends 10 s after it has no session and no client; "
                        "what `contour client` starts on demand).",
                        "POLICY" },
                    CLI::Option { "exit-with-last-session",
                                  CLI::Value { false },
                                  "Alias for --lifecycle=exit-with-last-session." },
                    CLI::Option { "size-policy",
                                  CLI::Value { ""s },
                                  "Which attached client's size the shared grid takes when several "
                                  "clients of different sizes are attached: `latest` (the client "
                                  "that resized most recently), `smallest` (the largest grid every "
                                  "client can fully display) or `largest` (the union, which smaller "
                                  "clients pan). Mirrors tmux's `window-size`. Defaults to the "
                                  "configuration's `daemon.size_policy` (`latest` unless set).",
                                  "POLICY" },
                    CLI::Option { "tmux-compat-socket",
                                  CLI::Value { ""s },
                                  "Additionally binds tmux's own discovery path "
                                  "/tmp/tmux-<uid>/<LABEL> so a plain `tmux -L LABEL -C "
                                  "attach-session` finds this daemon.",
                                  "LABEL" },
                    CLI::Option { "listen-tcp",
                                  CLI::Value { ""s },
                                  "Also serve the native protocol over TCP at HOST:PORT "
                                  "(opt-in; loopback e.g. 127.0.0.1:9090 by default). Always "
                                  "TLS-encrypted and token-authenticated.",
                                  "HOST:PORT" },
                    CLI::Option { "token",
                                  CLI::Value { ""s },
                                  "Preshared token every TCP client must present; required with "
                                  "--listen-tcp, which has no filesystem gate to fall back on. "
                                  "Note that a token given here is visible to other local users "
                                  "via the process list; prefer --token-file.",
                                  "TOKEN" },
                    CLI::Option { "token-file",
                                  CLI::Value { ""s },
                                  "Reads the preshared token from FILE instead of the command "
                                  "line, so the secret is protected by that file's permissions "
                                  "rather than exposed in the process list. Trailing newlines "
                                  "are ignored.",
                                  "FILE" },
                    CLI::Option { "access-file",
                                  CLI::Value { ""s },
                                  "Reads further TCP tokens from FILE, one per line: a grant list "
                                  "(`all`, or a comma list of read, screen, input and write), one "
                                  "space, then the token. A token that does not grant everything "
                                  "serves control API calls only; viewing needs all four. Blank "
                                  "lines and lines starting with `#` are ignored. The tokens are "
                                  "accepted beside --token, never instead of it, and may not repeat it.",
                                  "FILE" },
                    CLI::Option { "tls-cert",
                                  CLI::Value { ""s },
                                  "PEM certificate for the TCP listener. When omitted (with "
                                  "--tls-key) the daemon generates an ephemeral self-signed "
                                  "certificate (TOFU).",
                                  "FILE" },
                    CLI::Option {
                        "tls-key", CLI::Value { ""s }, "PEM private key matching --tls-cert.", "FILE" },
                    CLI::Option { "log",
                                  CLI::Value { ""s },
                                  "Enables logging for a comma (,) separated list of tags, or "
                                  "`all` (see `contour list-debug-tags`). Note the trailing `*` "
                                  "is a prefix match, so `vthost.*` includes the verbose "
                                  "`vthost.trace.*` tiers. Overrides $LOG.",
                                  "TAGS" },
                    CLI::Option { "log-file",
                                  CLI::Value { ""s },
                                  "Appends log output to FILE instead of standard error. If - "
                                  "(dash) is given, standard error is used explicitly.",
                                  "FILE" },
                },
                daemonVerbCommands() },
            CLI::Command {
                "capture",
                "Captures the screen buffer of the currently running terminal.",
                {
                    CLI::Option { "logical",
                                  CLI::Value { false },
                                  "Tells the terminal to use logical lines for counting and capturing." },
                    CLI::Option { "words",
                                  CLI::Value { false },
                                  "Splits each line into words and outputs only one word per line." },
                    CLI::Option { "timeout",
                                  CLI::Value { 1.0 },
                                  "Sets timeout seconds to wait for terminal to respond.",
                                  "SECONDS" },
                    CLI::Option { "lines", CLI::Value { 0u }, "The number of lines to capture", "COUNT" },
                    CLI::Option { "to",
                                  CLI::Value { ""s },
                                  "Output file name to store the screen capture to. If - (dash) is given, "
                                  "the capture will be written to standard output.",
                                  "FILE",
                                  CLI::Presence::Required },
                } },
            CLI::Command {
                "set",
                "Sets various aspects of the connected terminal.",
                CLI::OptionList {},
                CLI::CommandList {
                    CLI::Command { "profile",
                                   "Changes the terminal profile of the currently attached terminal to the "
                                   "given value.",
                                   CLI::OptionList { CLI::Option {
                                       "to",
                                       CLI::Value { ""s },
                                       "Profile name to activate in the currently connected terminal.",
                                       "NAME" } } } } } }
    };
    // NOLINTEND
    for (auto& verbCommand: controlVerbCommands())
        command.children.emplace_back(std::move(verbCommand));
    command.children.emplace_back(agentCommand());
    return command;
}

} // namespace contour::cli
