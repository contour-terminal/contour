// SPDX-License-Identifier: Apache-2.0
#include <contour/CaptureScreen.h>
#include <contour/Config.h>
#include <contour/ContourApp.h>

#include <vtbackend/Capabilities.h>
#include <vtbackend/Functions.h>

#include <vtparser/Parser.h>

#include <crispy/App.h>
#include <crispy/CLI.h>
#include <crispy/StackTrace.h>
#include <crispy/utils.h>

#include <QtCore/QFile>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>

#if !defined(_WIN32)
    #include <sys/ioctl.h>

    #include <unistd.h>
#endif

#if defined(_WIN32)
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

namespace CLI = crispy::cli;

namespace contour
{

// {{{ helper
namespace
{
#if defined(__linux__)
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
} // namespace
// }}}

ContourApp::ContourApp(): app("contour", "Contour Terminal Emulator", CONTOUR_VERSION_STRING, "Apache-2.0")
{
    using Project = crispy::cli::about::project;
    crispy::cli::about::registerProjects(
#if defined(CONTOUR_BUILD_WITH_MIMALLOC)
        Project { "mimalloc", "", "" },
#endif
        Project { "Qt", "GPL", "https://www.qt.io/" },
        Project { "FreeType", "GPL, FreeType License", "https://freetype.org/" },
        Project { "HarfBuzz", "Old MIT", "https://harfbuzz.github.io/" },
        // Project{"Catch2", "BSL-1.0", "https://github.com/catchorg/Catch2"},
        Project { "libunicode", "Apache-2.0", "https://github.com/contour-terminal/libunicode" },
        Project { "range-v3", "Boost Software License 1.0", "https://github.com/ericniebler/range-v3" },
        Project { "yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp" },
        Project { "termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro" },
        Project { "fmt", "MIT", "https://github.com/fmtlib/fmt" });

#if defined(__linux__)
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

#if defined(_WIN32)
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
}

template <typename Callback>
auto withOutput(crispy::cli::flag_store const& flags, std::string const& name, Callback callback)
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
    for (auto const& [action, description]: contour::actions::getDocumentation())
    {
        std::format_to(back, "| `{:<20}` | {:} |\n", action, description);
    }

    std::format_to(back, "\n");
    std::format_to(back, "Example of entries inside config file\n");
    std::format_to(back, "``` yaml\n");
    for (auto const& [action, description]: contour::actions::getDocumentation())
    {
        std::format_to(back, " - {{ mods: [Control], key: Enter, action: {:} }}\n", action);
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
    backend: OpenGL
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
    max_width: 0
    max_height: 0

```

The default profile is automatically the top (first) defined profile in the configuration file, but can be explicitly set to an order-independant name using `default_profile` configuration key.


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

Syntax for color shemes repeat the one of profiles. First color scheme inside configuration file must be named `default`, each other color schemes inherit values from `default` color scheme. Example of configuration for `color_schemes`
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
        QFile file;
        if (shell == "zsh")
            file.setFileName(":/contour/shell-integration/shell-integration.zsh");
        else if (shell == "fish")
            file.setFileName(":/contour/shell-integration/shell-integration.fish");
        else if (shell == "tcsh")
            file.setFileName(":/contour/shell-integration/shell-integration.tcsh");
        else if (shell == "bash")
            file.setFileName(":/contour/shell-integration/shell-integration.bash");
        else
        {
            std::cerr << std::format("Cannot generate shell integration for an unsupported shell, {}.\n",
                                     shell);
            return EXIT_FAILURE;
        }
        Require(file.open(QFile::ReadOnly));
        auto const contents = file.readAll();
        stream.write(contents.constData(), contents.size());
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
                        "Create documentaion for configuration file",
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
                                          "Shell name to create the integration for. "
                                          "Supported shells: fish, zsh, tcsh",
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
