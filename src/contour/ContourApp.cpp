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
#include <contour/Config.h>
#include <contour/ContourApp.h>

#include "shell_integration_zsh.h"

#include <terminal/Capabilities.h>
#include <terminal/Parser.h>

#include <crispy/App.h>
#include <crispy/StackTrace.h>
#include <crispy/utils.h>

#include <fmt/format.h>
#include <fmt/chrono.h>

#include <cstdio>
#include <chrono>
#include <iostream>
#include <fstream>
#include <memory>

#include <signal.h>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/ioctl.h>
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

namespace contour {

namespace // {{{ helper
{
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

#if defined(__linux__)
    void crashLogger(std::ostream& out)
    {
        out
            << "Contour version: " << CONTOUR_VERSION_STRING << "\r\n"
            << "\r\n"
            << "Stack Trace:\r\n"
            << "------------\r\n"
            ;

        auto stackTrace = crispy::StackTrace();
        auto symbols = stackTrace.symbols();
        for (size_t i = 0; i < symbols.size(); ++i)
            out << symbols[i] << "\r\n";
    }

    // Have this directory string already pre-created, as in case of a SEGV
    // it may very well be that the memory was corrupted too.
    std::string crashLogDir;

    void segvHandler(int signum)
    {
        signal(signum, SIG_DFL);

        std::stringstream sstr;
        crashLogger(sstr);
        string crashLog = sstr.str();

        auto const logFileName = fmt::format(
            "contour-crash-{:%Y-%m-%d-%H-%M-%S}-pid-{}.log",
            std::chrono::system_clock::now(),
            getpid()
        );
        if (chdir(crashLogDir.c_str()) < 0)
            perror("chdir");
        char hostname[80] = {0};
        gethostname(hostname, sizeof(hostname));

        cerr
            << "\r\n"
            << "========================================================================\r\n"
            << "  An internal error caused the terminal to crash ;-( 😭\r\n"
            << "-------------------------------------------------------\r\n"
            << "\r\n"
            << "Please report this to https://github.com/contour-terminal/contour/issues/\r\n"
            << "\r\n"
            << crashLog
            << "========================================================================\r\n"
            << "\r\n"
            << "Please report the above information and help making this project better.\r\n"
            << "\r\n"
            << "This log will also be written to: \033[1m"
                << "\033]8;;file://" << hostname << "/"
                    << crashLogDir << "/" << logFileName
                << "\033\\"
                << crashLogDir << "/" << logFileName
                << "\033]8;;\033\\"
                << "\033[m\r\n"
            << "\r\n"
            ;
        cerr.flush();

        ofstream logFile(logFileName);
        logFile << crashLog;

        abort();
    }
#endif
} // }}}

ContourApp::ContourApp() :
    App("contour", "Contour Terminal Emulator", CONTOUR_VERSION_STRING, "Apache-2.0")
{
    using Project = crispy::cli::about::Project;
    crispy::cli::about::registerProjects(
        #if defined(CONTOUR_BUILD_WITH_MIMALLOC)
        Project{"mimalloc", "", ""},
        #endif
        Project{"Qt", "GPL", "https://www.qt.io/"},
        Project{"FreeType", "GPL, FreeType License", "https://freetype.org/"},
        Project{"HarfBuzz", "Old MIT", "https://harfbuzz.org/"},
        //Project{"Catch2", "BSL-1.0", "https://github.com/catchorg/Catch2"},
        Project{"libunicode", "Apache-2.0", "https://github.com/contour-terminal/libunicode"},
        Project{"range-v3", "Boost Software License 1.0", "https://github.com/ericniebler/range-v3"},
        Project{"yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp"},
        Project{"termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro"},
        Project{"fmt", "MIT", "https://github.com/fmtlib/fmt"}
    );

#if defined(__linux__)
    auto crashLogDirPath = crispy::App::instance()->localStateDir() / "crash";
    FileSystem::create_directories(crashLogDirPath);
    crashLogDir = crashLogDirPath.string();
    signal(SIGSEGV, segvHandler);
    signal(SIGABRT, segvHandler);
#endif

#if defined(_WIN32)
    // Enable VT output processing on Conhost.
    HANDLE stdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD savedModes{}; // NOTE: Is it required to restore that upon process exit?
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
    link("contour.parser-table", bind(&ContourApp::parserTableAction, this));
    link("contour.generate.terminfo", bind(&ContourApp::terminfoAction, this));
    link("contour.generate.config", bind(&ContourApp::configAction, this));
    link("contour.generate.integration", bind(&ContourApp::integrationAction, this));
}

template <typename Callback>
auto withOutput(crispy::cli::FlagStore const& _flags, std::string const& _name, Callback _callback)
{
    std::ostream* out = &cout;

    auto const& outputFileName = _flags.get<string>(_name); // TODO: support string_view
    auto ownedOutput = unique_ptr<std::ostream>{};
    if (outputFileName != "-")
    {
        ownedOutput = make_unique<std::ofstream>(outputFileName);
        out = ownedOutput.get();
    }

    return _callback(*out);
}

int ContourApp::integrationAction()
{
    return withOutput(parameters(), "contour.generate.integration.to", [&](auto& _stream) {
        auto const shell = parameters().get<string>("contour.generate.integration.shell");
        if (shell == "zsh")
        {
            _stream.write((char const*) shell_integration_zsh.data(), shell_integration_zsh.size());
            return EXIT_SUCCESS;
        }
        else
        {
            std::cerr << fmt::format("Cannot generate shell integration for an unsupported shell, {}.\n", shell);
            return EXIT_FAILURE;
        }
    });
}

int ContourApp::configAction()
{
    withOutput(parameters(), "contour.generate.config.to", [](auto& _stream) {
        _stream << config::createDefaultConfig();
    });
    return EXIT_SUCCESS;
}

int ContourApp::terminfoAction()
{
    withOutput(parameters(), "contour.generate.terminfo.to", [](auto& _stream) {
        _stream << terminal::capabilities::StaticDatabase{}.terminfo();
    });
    return EXIT_SUCCESS;
}

int ContourApp::captureAction()
{
    auto captureSettings = contour::CaptureSettings{};
    captureSettings.logicalLines = parameters().get<bool>("contour.capture.logical");
    captureSettings.timeout = parameters().get<double>("contour.capture.timeout");
    captureSettings.lineCount = parameters().get<unsigned>("contour.capture.lines");
    captureSettings.outputFile = parameters().get<string>("contour.capture.to");

    if (contour::captureScreen(captureSettings))
        return EXIT_SUCCESS;
    else
        return EXIT_FAILURE;
}

int ContourApp::parserTableAction()
{
    terminal::parser::dot(std::cout, terminal::parser::ParserTable::get());
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
    cout << fmt::format("\033P$p{}\033\\", profileName);
    return EXIT_SUCCESS;
}

crispy::cli::Command ContourApp::parameterDefinition() const
{
    return CLI::Command{
        "contour",
        "Contour Terminal Emulator " CONTOUR_VERSION_STRING " - https://github.com/contour-terminal/contour/ ;-)",
        CLI::OptionList{},
        CLI::CommandList{
            CLI::Command{"help", "Shows this help and exits."},
            CLI::Command{"version", "Shows the version and exits."},
            CLI::Command{"license", "Shows the license, and project URL of the used projects and Contour."},
            CLI::Command{"parser-table", "Dumps parser table"},
            CLI::Command{"list-debug-tags", "Lists all available debug tags and exits."},
            CLI::Command{
                "generate",
                "Generation utilities.",
                CLI::OptionList{},
                CLI::CommandList{
                    CLI::Command{
                        "terminfo",
                        "Generates the terminfo source file that will reflect the features of this version of contour. Using - as value will write to stdout instead.",
                        {
                            CLI::Option{
                                "to",
                                CLI::Value{""s},
                                "Output file name to store the screen capture to. If - (dash) is given, the output will be written to standard output.",
                                "FILE",
                                CLI::Presence::Required
                            },
                        }
                    },
                    CLI::Command{
                        "config",
                        "Generates configuration file with the default configuration.",
                        CLI::OptionList{
                            CLI::Option{
                                "to",
                                CLI::Value{""s},
                                "Output file name to store the config file to. If - (dash) is given, the output will be written to standard output.",
                                "FILE",
                                CLI::Presence::Required
                            },
                        }
                    },
                    CLI::Command{
                        "integration",
                        "Generates shell integration script.",
                        CLI::OptionList{
                            CLI::Option{
                                "shell",
                                CLI::Value{""s},
                                "Shell name to create the integration for. Currently only zsh is supported.",
                                "SHELL",
                                CLI::Presence::Required
                            },
                            CLI::Option{
                                "to",
                                CLI::Value{""s},
                                "Output file name to store the shell integration file to. If - (dash) is given, the output will be written to standard output.",
                                "FILE",
                                CLI::Presence::Required
                            },
                        }
                    }
                }
            },
            CLI::Command{
                "capture",
                "Captures the screen buffer of the currently running terminal.",
                {
                    CLI::Option{"logical", CLI::Value{false}, "Tells the terminal to use logical lines for counting and capturing."},
                    CLI::Option{"timeout", CLI::Value{1.0}, "Sets timeout seconds to wait for terminal to respond.", "SECONDS"},
                    CLI::Option{"lines", CLI::Value{0u}, "The number of lines to capture", "COUNT"},
                    CLI::Option{"to", CLI::Value{""s}, "Output file name to store the screen capture to. If - (dash) is given, the capture will be written to standard output.", "FILE", CLI::Presence::Required},
                }
            },
            CLI::Command{
                "set",
                "Sets various aspects of the connected terminal.",
                CLI::OptionList{},
                CLI::CommandList{
                    CLI::Command{
                        "profile",
                        "Changes the terminal profile of the currently attached terminal to the given value.",
                        CLI::OptionList{
                            CLI::Option{"to", CLI::Value{""s}, "Profile name to activate in the currently connected terminal.", "NAME"}
                        }
                    }
                }
            }
        }
    };
}

}
