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

#include <terminal/Capabilities.h>
#include <terminal/Parser.h>

#include <crispy/debuglog.h>
#include <crispy/utils.h>

#include <fstream>
#include <memory>

using std::bind;
using std::cout;
using std::make_unique;
using std::ofstream;
using std::string;
using std::string_view;
using std::unique_ptr;

using namespace std::string_literals;

namespace CLI = crispy::cli;

namespace contour {

ContourApp::ContourApp() :
    App("contour", "Contour Terminal Emulator", CONTOUR_VERSION_STRING)
{
    link("contour.capture", bind(&ContourApp::captureAction, this));
    link("contour.list-debug-tags", bind(&ContourApp::listDebugTagsAction, this));
    link("contour.set.profile", bind(&ContourApp::profileAction, this));
    link("contour.parser-table", bind(&ContourApp::parserTableAction, this));
    link("contour.generate.terminfo", bind(&ContourApp::terminfoAction, this));
    link("contour.generate.config", bind(&ContourApp::configAction, this));
}

int ContourApp::configAction()
{
    std::ostream* out = &cout;
    auto const& outputFileName = parameters().get<string>("contour.generate.config.output");
    auto ownedOutput = unique_ptr<std::ostream>{};
    if (outputFileName != "-")
    {
        ownedOutput = make_unique<std::ofstream>(outputFileName);
        out = ownedOutput.get();
    }
    *out << config::createDefaultConfig();
    return EXIT_SUCCESS;
}

int ContourApp::terminfoAction()
{
    auto const& outputFileName = parameters().get<string>("contour.generate.terminfo.output");
    auto ownedOutput = unique_ptr<std::ostream>{};
    std::ostream* out = &cout;
    if (outputFileName != "-")
    {
        ownedOutput = make_unique<std::ofstream>(outputFileName);
        out = ownedOutput.get();
    }
    *out << terminal::capabilities::StaticDatabase{}.terminfo();
    return EXIT_SUCCESS;
}

int ContourApp::captureAction()
{
    auto captureSettings = contour::CaptureSettings{};
    captureSettings.logicalLines = parameters().get<bool>("contour.capture.logical");
    captureSettings.timeout = parameters().get<double>("contour.capture.timeout");
    captureSettings.lineCount = parameters().get<unsigned>("contour.capture.lines");
    captureSettings.outputFile = parameters().get<string>("contour.capture.output");

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
        "Contour Terminal Emulator " CONTOUR_VERSION_STRING " - https://github.com/christianparpart/contour/ ;-)",
        CLI::OptionList{},
        CLI::CommandList{
            CLI::Command{"help", "Shows this help and exits."},
            CLI::Command{"version", "Shows The version and exits."},
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
                                "output",
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
                                "output",
                                CLI::Value{""s},
                                "Output file name to store the config file to. If - (dash) is given, the output will be written to standard output.",
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
                    CLI::Option{"timeout", CLI::Value{1.0}, "Sets timeout seconds to wait for terminal to respond."},
                    CLI::Option{"lines", CLI::Value{0u}, "The number of lines to capture"},
                    CLI::Option{"output", CLI::Value{""s}, "Output file name to store the screen capture to. If - (dash) is given, the capture will be written to standard output.", "FILE", CLI::Presence::Required},
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
                            CLI::Option{"to", CLI::Value{""s}, "Profile name to activate in the currently connected terminal."}
                        }
                    }
                }
            }
        }
    };
}

}
