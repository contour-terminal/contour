// SPDX-License-Identifier: Apache-2.0
///
/// @file
/// The canary that proves `Terminal` refuses a null output rather than dereferencing it.
///
/// Both constructors that take a `TerminalOutput` ask it its kind in their initializer lists, so a
/// null output used to be a null dereference before any body ran. It is a precondition now, and a
/// failed precondition ends the process, which a Catch case cannot observe. So the refusal runs
/// here, once per constructor (the mode), judged by `PASS_REGULAR_EXPRESSION` on the precondition's
/// own text and `FAIL_REGULAR_EXPRESSION` on the line printed if construction RETURNED
/// (`src/core/tui/CMakeLists.txt`) -- never by an exit code alone.
///
/// The fail handler reports and exits with 1 instead of letting `fail()` abort: an abort is a
/// signal, which ctest reports as an exception that no regular expression overrides.
/// `Require()` is not compiled out with `NDEBUG`, so this runs in every configuration.

#include <core/Assert.hpp>
#include <core/platform/Clock.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace
{

/// What the process exits with once the precondition has fired.
constexpr int RefusedExitCode = 1;

/// What the process exits with when it was asked for a mode it does not know.
constexpr int UsageExitCode = 2;

/// A query input that is never read: construction alone is under test.
class SilentQueryInput final: public core::tui::TerminalQueryInput
{
  public:
    [[nodiscard]] auto poll(int /*timeoutMs*/) -> std::vector<core::tui::InputEvent> override { return {}; }
    void unread(std::vector<core::tui::InputEvent> /*events*/) override {}
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: terminal-null-output-canary output|queryInput\n";
        return UsageExitCode;
    }
    auto const mode = std::string_view { argv[1] };

    core::setFailHandler(
        [](std::string_view text, std::string_view message, std::string_view file, int line) {
            std::cerr << std::format(
                "terminal-null-output-canary: [{}:{}] {} {}\n", file, line, message, text);
            std::_Exit(RefusedExitCode);
        });

    std::cerr << std::format(
        "terminal-null-output-canary: {}: about to construct a Terminal over a null output\n", mode);
    if (mode == "output")
    {
        auto const terminal = core::tui::Terminal { std::unique_ptr<core::tui::TerminalOutput> {} };
    }
    else if (mode == "queryInput")
    {
        auto input = SilentQueryInput {};
        auto clock = core::platform::ManualClock {};
        auto const terminal =
            core::tui::Terminal { std::unique_ptr<core::tui::TerminalOutput> {}, input, clock };
    }
    else
    {
        std::cerr << std::format("terminal-null-output-canary: unknown mode '{}'\n", mode);
        return UsageExitCode;
    }
    std::cerr << "terminal-null-output-canary: a null output was accepted\n";
    return 0;
}
