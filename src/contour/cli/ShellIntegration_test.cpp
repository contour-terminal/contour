// SPDX-License-Identifier: Apache-2.0
//
// The embedded shell-integration scripts. What matters here is that the bytes the binary carries
// are the bytes shell-integration/ holds -- the scripts stopped travelling through Qt's resource
// system so that a GUI-less build needs no Qt, and a generator that quietly mangled them would be
// found only by a user whose shell broke.

#include <contour/cli/ShellIntegration.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

using namespace std::string_view_literals;

namespace
{

/// Reads a file as raw bytes with CRLF folded to LF, matching what the generator embeds.
/// @param path File to read.
/// @return Its contents, or an empty string if it could not be opened.
[[nodiscard]] std::string readNormalized(std::filesystem::path const& path)
{
    auto in = std::ifstream { path, std::ios::binary };
    if (!in)
        return {};

    auto text = std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::erase(text, '\r');
    return text;
}

} // namespace

TEST_CASE("shellIntegrationScript serves every shell the build compiled in", "[shell-integration]")
{
    auto const shells = contour::cli::supportedShells();
    REQUIRE(!shells.empty());

    for (auto const& row: shells)
    {
        INFO("shell: " << row.name);
        auto const script = contour::cli::shellIntegrationScript(row.name);
        REQUIRE(script.has_value());
        CHECK(!script->empty());
        CHECK(*script == row.script);

        // A shell script with CRLF in it is a broken shell script: the CR ends up part of every
        // command. The generator folds them, and a Windows checkout is exactly where it matters.
        CHECK(!script->contains('\r'));
    }
}

TEST_CASE("shellIntegrationScript rejects a shell it carries no script for", "[shell-integration]")
{
    auto const script = contour::cli::shellIntegrationScript("nosuchshell");

    REQUIRE(!script.has_value());
    CHECK(script.error() == contour::cli::ShellIntegrationError::UnsupportedShell);

    SECTION("and does not match a prefix of a supported one")
    {
        CHECK(!contour::cli::shellIntegrationScript("ba").has_value());
        CHECK(!contour::cli::shellIntegrationScript("bashx").has_value());
        CHECK(!contour::cli::shellIntegrationScript("").has_value());
    }
}

TEST_CASE("the embedded scripts are the ones in the source tree", "[shell-integration]")
{
    auto const shellIntegrationDir = std::filesystem::path { CONTOUR_PROJECT_SOURCE_DIR } / "src" / "contour"
                                     / "cli" / "shell-integration";

    for (auto const& row: contour::cli::supportedShells())
    {
        INFO("shell: " << row.name);
        auto const onDisk =
            readNormalized(shellIntegrationDir / ("shell-integration." + std::string { row.name }));
        REQUIRE(!onDisk.empty());
        CHECK(row.script == onDisk);
    }
}

TEST_CASE("supportedShellsText names exactly the compiled-in shells", "[shell-integration]")
{
    auto const text = contour::cli::supportedShellsText();
    auto const shells = contour::cli::supportedShells();

    for (auto const& row: shells)
    {
        INFO("shell: " << row.name);
        CHECK(text.contains(row.name));
    }

    // ...and names nothing else: a joined list has one separator fewer than it has entries.
    CHECK(std::ranges::count(text, ',') == static_cast<std::ptrdiff_t>(shells.size()) - 1);
}

TEST_CASE("the PowerShell integration is compiled in", "[shell-integration]")
{
    // Its own name, pwsh, as the executable is called: `contour generate integration shell pwsh`.
    auto const script = contour::cli::shellIntegrationScript("pwsh");
    REQUIRE(script.has_value());
    CHECK(script->contains("PSConsoleHostReadLine"));
}

TEST_CASE("natively integrating shells are named, not scripted", "[shell-integration]")
{
    auto const natives = contour::cli::nativeShellIntegrations();
    REQUIRE(!natives.empty());
    CHECK(std::ranges::contains(natives, "nu"sv, &contour::cli::NativeShellIntegrationRow::name));

    for (auto const& row: natives)
    {
        INFO("shell: " << row.name);
        // Scripted or native, never both: a script for a shell that marks its prompts itself would
        // mark every one of them twice.
        CHECK(!contour::cli::shellIntegrationScript(row.name).has_value());
        CHECK(contour::cli::nativeShellsText().contains(row.displayName));

        auto const message = contour::cli::unsupportedShellMessage(row.name);
        CHECK(message.contains(row.howToEnable));
        CHECK(message.ends_with('\n'));
    }
}

TEST_CASE("unsupportedShellMessage names the supported shells for an unknown one", "[shell-integration]")
{
    auto const message = contour::cli::unsupportedShellMessage("nosuchshell");
    CHECK(message.contains("unsupported shell, nosuchshell"));
    CHECK(message.contains(contour::cli::supportedShellsText()));
    CHECK(message.ends_with('\n'));
}

TEST_CASE("every script cuts a command line just above what the terminal keeps", "[shell-integration]")
{
    // A few bytes over the terminal's own limit, so that it still sees a longer line was cut. The scripts
    // spell the number out; this keeps each of them on the constant the terminal uses.
    constexpr auto Margin = size_t { 4 };
    struct Limit
    {
        std::string_view shell;      ///< The script's shell.
        std::string_view assignment; ///< What precedes the number in the script.
    };
    constexpr auto Limits = std::array {
        Limit { .shell = "bash", .assignment = "\n__contour_cmdline_max_bytes=" },
        Limit { .shell = "zsh", .assignment = "\ntypeset -g _contour_cmdline_max_bytes=" },
        Limit { .shell = "pwsh", .assignment = "\n    MaxCommandLineBytes = " },
    };
    for (auto const& [shell, assignment]: Limits)
    {
        INFO("shell: " << shell);
        auto const script = contour::cli::shellIntegrationScript(shell);
        REQUIRE(script.has_value());
        auto const at = script->find(assignment);
        REQUIRE(at != std::string_view::npos);
        auto const number = script->substr(at + assignment.size());
        auto value = size_t { 0 };
        auto const [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
        REQUIRE(error == std::errc {});
        CHECK(*end == '\n');
        CHECK(value == vtbackend::MaxRecordedCommandLineBytes + Margin);
    }
}
