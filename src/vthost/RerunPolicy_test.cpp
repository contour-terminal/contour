// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <vthost/RerunPolicy.hpp>

using namespace vthost;

namespace
{

struct RerunCase
{
    std::optional<std::string> foreground;
    std::optional<std::string> commandLine;
    std::vector<std::string> allowed;
    std::optional<std::string> expected;
};

} // namespace

TEST_CASE("rerunInputFor re-runs only allow-listed programs, literally", "[vthost][snapshot][rerun]")
{
    using std::nullopt;
    auto const cases = std::array {
        RerunCase { "vim", "vim notes.md", {}, nullopt },
        RerunCase { "vim", "vim notes.md", { "vim" }, "vim notes.md\r" },
        RerunCase { "htop", nullopt, { "htop" }, "htop\r" },
        RerunCase { nullopt, "  vim notes.md ", { "vim" }, "vim notes.md\r" },
        RerunCase { nullopt, "/usr/bin/htop -d 5", { "htop" }, "/usr/bin/htop -d 5\r" },
        RerunCase { nullopt, "C:\\tools\\nvim.EXE x", { "nvim" }, "C:\\tools\\nvim.EXE x\r" },
        RerunCase { "vim", "vim $(curl x|sh)", { "vim" }, "vim\r" },
        RerunCase { nullopt, "vim $(curl x|sh)", { "vim" }, nullopt },
        RerunCase { nullopt, "vim a; rm -rf ~", { "vim" }, nullopt },
        RerunCase { "bash", "sudo vim x", { "vim" }, nullopt },
        RerunCase { "vim", "nano x", { "vim" }, "vim\r" },
        RerunCase { nullopt, "vim a\nrm b", { "vim" }, nullopt },
        RerunCase { nullopt, "vim !!", { "vim" }, nullopt },
        RerunCase { nullopt, "vim a ^b", { "vim" }, nullopt },
        RerunCase { nullopt, "vim a\\ b", { "vim" }, nullopt },
        // Beyond the plan's table: a quote would leave the shell waiting for the rest of the line.
        RerunCase { nullopt, "vim 'a b", { "vim" }, nullopt },
        RerunCase { "nvim.exe", nullopt, { "nvim" }, "nvim\r" },
        RerunCase { nullopt, nullopt, { "vim" }, nullopt },
        RerunCase { nullopt, "vimdiff a b", { "vim" }, nullopt },
    };
    for (auto const& row: cases)
    {
        INFO("foreground " << row.foreground.value_or("<none>") << ", line "
                           << row.commandLine.value_or("<none>"));
        auto pane = PaneSnapshot {};
        pane.foregroundProcess = row.foreground;
        pane.runningCommandLine = row.commandLine;
        CHECK(rerunInputFor(pane, row.allowed) == row.expected);
    }
}
